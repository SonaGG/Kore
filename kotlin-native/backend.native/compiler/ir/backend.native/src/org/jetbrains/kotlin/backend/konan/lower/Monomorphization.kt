/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.backend.konan.lower

import org.jetbrains.kotlin.backend.common.lower.createIrBuilder
import org.jetbrains.kotlin.backend.konan.Context
import org.jetbrains.kotlin.descriptors.ClassKind
import org.jetbrains.kotlin.descriptors.DescriptorVisibilities
import org.jetbrains.kotlin.descriptors.Modality
import org.jetbrains.kotlin.ir.IrElement
import org.jetbrains.kotlin.ir.IrStatement
import org.jetbrains.kotlin.ir.builders.declarations.addConstructor
import org.jetbrains.kotlin.ir.builders.declarations.addValueParameter
import org.jetbrains.kotlin.ir.builders.irBlockBody
import org.jetbrains.kotlin.ir.builders.irNull
import org.jetbrains.kotlin.ir.declarations.*
import org.jetbrains.kotlin.ir.expressions.*
import org.jetbrains.kotlin.ir.expressions.impl.*
import org.jetbrains.kotlin.ir.symbols.IrClassSymbol
import org.jetbrains.kotlin.ir.symbols.IrTypeParameterSymbol
import org.jetbrains.kotlin.ir.types.*
import org.jetbrains.kotlin.ir.util.*
import org.jetbrains.kotlin.ir.visitors.IrElementTransformerVoid
import org.jetbrains.kotlin.ir.visitors.IrVisitorVoid
import org.jetbrains.kotlin.ir.visitors.acceptChildrenVoid
import org.jetbrains.kotlin.ir.visitors.acceptVoid
import org.jetbrains.kotlin.ir.visitors.transformChildrenVoid
import org.jetbrains.kotlin.name.Name

/**
 * Kore: monomorphization of generic classes (and the generic functions they call) for primitive type arguments, like
 * C++ templates or .NET's generics over value types, so that e.g. `ArrayList<Int>` keeps its elements in an `IntArray`.
 *
 * For each allocation `C<..., P, ...>()` of a generic class whose IR is available here, with non-null primitive type
 * arguments `P`, the class is copied with those type parameters substituted (the copy keeps its type parameters, the
 * substituted ones unused), and in the copy:
 *  - `Array<P>` becomes the primitive array of `P` (fields, variables, parameters, results), with the array's members
 *    and the standard extensions used on it (`copyInto`, `copyOfUninitializedElements`, ...) replaced by those of the
 *    primitive array, and `resetAt`/`resetRange` (which only release references) dropped;
 *  - generic functions with bodies here that it calls with `P` (or `Array<P>`) are copied the same way;
 *  - other generic classes it allocates with `P` are copied the same way (e.g. its iterator classes).
 * If any use of an `Array<P>` cannot be mapped, the instantiation is left alone.
 *
 * The copy is a subclass of the original class (made open, in this compilation only), and overrides all its members,
 * so that it can be used wherever the original type is: calls and type checks on the original class still work (the
 * bridges for the members whose signatures now use primitives are built by the usual bridge lowering). Its
 * constructors delegate to a constructor added to the original class that runs none of its initializers.
 *
 * Runs after all inline functions have been inlined, before the other lowerings, over all the modules compiled here.
 */
internal class Monomorphization(private val context: Context, private val modules: List<IrModuleFragment>) {
    private val irBuiltIns = context.irBuiltIns
    private val debug = System.getenv("KORE_DEBUG_MONOMORPHIZATION") != null

    // Instantiations are the same when their (primitive) type arguments are the same classes.
    private class Key(val declaration: IrDeclaration, val arguments: List<IrType?>) {
        private val names = arguments.map { it?.classFqName?.asString() }
        override fun equals(other: Any?) = other is Key && other.declaration == declaration && other.names == names
        override fun hashCode() = declaration.hashCode() * 31 + names.hashCode()
    }

    private val classCopies = mutableMapOf<Key, IrClass?>()
    private val functionCopies = mutableMapOf<Key, IrSimpleFunction?>()
    private val noInitConstructors = mutableMapOf<IrClass, IrConstructor?>()
    private val inProgress = mutableSetOf<Key>()
    private val available = mutableSetOf<IrDeclaration>()

    // Extensions of primitive arrays, by name and receiver class.
    private val primitiveArrayExtensions = mutableMapOf<Pair<String, IrClassSymbol>, MutableList<IrSimpleFunction>>()
    private val primitiveArrays: Map<IrClassSymbol, IrClassSymbol> by lazy {
        irBuiltIns.primitiveArrayForType.entries.associate { it.key!!.classOrFail to it.value }
    }

    fun run() {
        for (module in modules) for (file in module.files) {
            file.acceptVoid(object : IrVisitorVoid() {
                override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
                override fun visitClass(declaration: IrClass) {
                    available += declaration
                    declaration.acceptChildrenVoid(this)
                }
                override fun visitSimpleFunction(declaration: IrSimpleFunction) {
                    available += declaration
                    val receiver = declaration.parameters.firstOrNull { it.kind == IrParameterKind.ExtensionReceiver }
                    val receiverClass = receiver?.type?.classOrNull
                    if (receiverClass != null && receiverClass in primitiveArrays.values)
                        primitiveArrayExtensions.getOrPut(declaration.name.asString() to receiverClass) { mutableListOf() } += declaration
                    declaration.acceptChildrenVoid(this)
                }
            })
        }
        // Specialize the allocations in the original code; the copies are processed when they are made.
        val files = modules.flatMap { it.files }
        for (file in files) specializeAllocations(file)
        if (debug) System.err.println("Monomorphization: ${classCopies.count { it.value != null }} classes, " +
                "${functionCopies.count { it.value != null }} functions; failed: ${classCopies.filter { it.value == null }.keys.map { (it.declaration as IrClass).name }}")
    }

    private fun specializeAllocations(element: IrElement) {
        element.transformChildrenVoid(object : IrElementTransformerVoid() {
            override fun visitConstructorCall(expression: IrConstructorCall): IrExpression {
                expression.transformChildrenVoid(this)
                val copy = classCopyFor(expression) ?: return expression
                val original = expression.symbol.owner
                val index = original.constructedClass.declarations.indexOf(original)
                val constructor = copy.declarations[index] as IrConstructor
                return IrConstructorCallImpl.fromSymbolOwner(
                        expression.startOffset, expression.endOffset, copy.typeWith(expression.typeArguments.map { it!! }), constructor.symbol
                ).also {
                    for (i in expression.typeArguments.indices) it.typeArguments[i] = expression.typeArguments[i]
                    for (i in expression.arguments.indices) it.arguments[i] = expression.arguments[i]
                }
            }
        })
    }

    private fun IrType.isSpecializableArgument() = isPrimitiveType(nullable = false)

    private fun classCopyFor(call: IrConstructorCall): IrClass? {
        val irClass = call.symbol.owner.constructedClass
        if (irClass.typeParameters.isEmpty() || call.typeArguments.size != irClass.typeParameters.size) return null
        // Copies are already specialized (their remaining type parameters are unused).
        if (irClass in copyRoots) return null
        if (call.typeArguments.none { it != null && it.isSpecializableArgument() }) return null
        if (irClass.kind != ClassKind.CLASS || irClass.isInner || irClass.isValue || irClass.isExpect || irClass.isExternal ||
                irClass.modality == Modality.ABSTRACT || irClass.modality == Modality.SEALED || irClass !in available ||
                irClass.symbol == irBuiltIns.arrayClass) return null
        if (irClass.getPackageFragment().packageFqName.asString().let { name -> EXCLUDED_PACKAGES.any { name == it || name.startsWith("$it.") } }) return null
        val key = Key(irClass, call.typeArguments.map { it?.takeIf { type -> type.isSpecializableArgument() } })
        classCopies[key]?.let { return it }
        if (key in classCopies || key in inProgress) return null
        inProgress += key
        val copy = try { copyClass(irClass, key.arguments) } catch (e: Abort) {
            if (debug) System.err.println("Monomorphization: ${irClass.name}${key.arguments}: ${e.message}")
            null
        }
        inProgress -= key
        classCopies[key] = copy
        return copy
    }

    private class Abort(message: String) : Exception(message)

    private fun substitution(typeParameters: List<IrTypeParameter>, arguments: List<IrType?>): Map<IrTypeParameterSymbol, IrType> =
            typeParameters.zip(arguments).mapNotNull { pair -> pair.second?.let { pair.first.symbol to it } }.toMap()

    // Substitutes types in a copy (after `deepCopyWithSymbols`, whose own type remapper must stay a plain
    // `DeepCopyTypeRemapper`: it is connected to the copier only then).
    private class SubstitutingTypeRemapper(private val substitution: Map<IrTypeParameterSymbol, IrType>) : TypeRemapper {
        override fun enterScope(irTypeParametersContainer: IrTypeParametersContainer) {}
        override fun leaveScope() {}
        override fun remapType(type: IrType): IrType = type.substitute(substitution)
    }

    private inline fun <reified T : IrTypeParametersContainer> T.copyWithSubstitution(parent: IrDeclarationParent, arguments: List<IrType?>): T {
        val copy = deepCopyWithSymbols(parent)
        copy.remapTypes(SubstitutingTypeRemapper(substitution(copy.typeParameters, arguments)))
        return copy
    }

    private fun suffix(arguments: List<IrType?>) = arguments.joinToString("") { "\$" + (it?.classOrNull?.owner?.name?.asString() ?: "_") }

    private fun IrDeclaration.isNoInitConstructor() =
            this is IrConstructor && parameters.singleOrNull()?.name?.asString() == NO_INIT_PARAMETER

    private fun copyClass(irClass: IrClass, arguments: List<IrType?>): IrClass {
        val copy = irClass.copyWithSubstitution(irClass.parent, arguments)
        copy.declarations.removeAll { it.isNoInitConstructor() }
        copy.name = Name.identifier(irClass.name.asString() + suffix(arguments))
        copy.modality = Modality.FINAL
        copyRoots += copy
        copy.annotations = emptyList()
        // First the arrays (the original is not changed until this succeeds).
        mapPrimitiveArrays(copy)
        val noInit = noInitConstructor(irClass) ?: throw Abort("no initializer-free constructor")
        // Members: overrides of the original's.
        val originalMembers = irClass.declarations.filter { !it.isNoInitConstructor() }
        if (originalMembers.size != copy.declarations.size) throw Abort("declaration mismatch")
        for (pair in originalMembers.zip(copy.declarations)) linkMember(pair.first, pair.second, noInit)
        copy.superTypes = listOf(irClass.symbol.typeWith(copy.typeParameters.mapIndexed { i, parameter -> arguments[i] ?: parameter.defaultType }))
        // Committed: the original becomes open, the copy joins it.
        if (irClass.modality == Modality.FINAL) irClass.modality = Modality.OPEN
        when (val parent = irClass.parent) {
            is IrFile -> parent.declarations += copy
            is IrClass -> parent.declarations += copy
            else -> throw Abort("unexpected parent")
        }
        copy.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) {
                if (element is IrClass || element is IrSimpleFunction) available += element as IrDeclaration
                element.acceptChildrenVoid(this)
            }
        })
        specializeAllocations(copy)
        return copy
    }

    private fun linkMember(original: IrDeclaration, member: IrDeclaration, noInit: IrConstructor) {
        when (member) {
            is IrSimpleFunction -> {
                original as IrSimpleFunction
                if (original.visibility == DescriptorVisibilities.PRIVATE || original.dispatchReceiverParameter == null) {
                    member.overriddenSymbols = emptyList()
                } else {
                    member.overriddenSymbols = listOf(original.symbol)
                    if (original.modality == Modality.FINAL) original.modality = Modality.OPEN
                }
            }
            is IrProperty -> {
                original as IrProperty
                if (original.visibility != DescriptorVisibilities.PRIVATE) {
                    member.overriddenSymbols = listOf(original.symbol)
                    if (original.modality == Modality.FINAL) original.modality = Modality.OPEN
                }
                listOf(original.getter to member.getter, original.setter to member.setter).forEach { pair ->
                    val o = pair.first
                    val m = pair.second
                    if (o != null && m != null) linkMember(o, m, noInit)
                }
            }
            is IrConstructor -> {
                // A delegation to the original's superclass becomes one to the original's initializer-free constructor.
                member.body?.transformChildrenVoid(object : IrElementTransformerVoid() {
                    override fun visitDelegatingConstructorCall(expression: IrDelegatingConstructorCall): IrExpression {
                        if (expression.symbol.owner.constructedClass == member.constructedClass) return expression
                        return IrDelegatingConstructorCallImpl.fromSymbolOwner(expression.startOffset, expression.endOffset,
                                irBuiltIns.unitType, noInit.symbol).also { it.arguments[0] = IrConstImpl.constNull(
                                expression.startOffset, expression.endOffset, irBuiltIns.nothingNType) }
                    }
                    override fun visitInstanceInitializerCall(expression: IrInstanceInitializerCall): IrExpression = expression
                })
            }
            else -> {}
        }
    }

    // A constructor of [irClass] that only calls its superclass's constructor (as its primary one does), without
    // initializing its fields, for its copies.
    private fun noInitConstructor(irClass: IrClass): IrConstructor? = noInitConstructors.getOrPut(irClass) {
        val primary = irClass.primaryConstructor ?: return@getOrPut null
        val superCall = (primary.body as? IrBlockBody)?.statements?.firstNotNullOfOrNull { it as? IrDelegatingConstructorCall }
                ?: return@getOrPut null
        if (superCall.arguments.any { it != null && it !is IrConst }) return@getOrPut null
        irClass.addConstructor {
            visibility = DescriptorVisibilities.PUBLIC
            origin = IrDeclarationOrigin.DEFINED
        }.also { constructor ->
            constructor.addValueParameter(NO_INIT_PARAMETER, irBuiltIns.nothingNType)
            constructor.body = context.createIrBuilder(constructor.symbol).irBlockBody {
                +superCall.deepCopyWithSymbols(constructor)
            }
        }
    }

    // ---------------------------- primitive arrays ----------------------------

    private fun IrType.primitiveArrayOrNull(): IrClassSymbol? {
        val simple = this as? IrSimpleType ?: return null
        if (simple.classifier != irBuiltIns.arrayClass) return null
        val element = (simple.arguments.singleOrNull() as? IrTypeProjection)?.type ?: return null
        if (!element.isPrimitiveType(nullable = false)) return null
        return primitiveArrays[element.classOrFail]
    }

    // The primitive array an `Array<P>` becomes, also for an expression already retyped to it (e.g. a call of a copy).
    private fun IrType.arrayStorageOrNull(): IrClassSymbol? =
            primitiveArrayOrNull() ?: classOrNull?.takeIf { it in primitiveArrays.values }

    private fun IrType.mapped(): IrType {
        val array = primitiveArrayOrNull() ?: return this
        return if (isMarkedNullable()) array.defaultType.makeNullable() else array.defaultType
    }

    /** Replaces `Array<P>` by the primitive array of P in [root] (a class or function copy); aborts if it cannot. */
    private fun mapPrimitiveArrays(root: IrDeclaration) {
        root.transformChildrenVoid(object : IrElementTransformerVoid() {
            override fun visitConstructorCall(expression: IrConstructorCall): IrExpression {
                expression.transformChildrenVoid(this)
                val array = expression.type.primitiveArrayOrNull() ?: return expression
                // `Array<P>(size)`: the primitive array's constructor of the same shape.
                val constructor = array.owner.declarations.filterIsInstance<IrConstructor>()
                        .singleOrNull { it.parameters.size == expression.arguments.size && it.parameters.all { p -> p.type.isInt() } }
                        ?: throw Abort("array constructor")
                return IrConstructorCallImpl.fromSymbolOwner(expression.startOffset, expression.endOffset, array.defaultType, constructor.symbol)
                        .also { for (i in expression.arguments.indices) it.arguments[i] = expression.arguments[i] }
            }

            override fun visitCall(expression: IrCall): IrExpression {
                expression.transformChildrenVoid(this)
                val callee = expression.symbol.owner
                // Members of `Array<P>`.
                val dispatch = callee.dispatchReceiverParameter
                if (dispatch != null) {
                    val array = expression.arguments[0]?.type?.arrayStorageOrNull()
                    if (array != null && callee.parentClassOrNull?.symbol == irBuiltIns.arrayClass) {
                        val target = array.owner.declarations.asSequence().flatMap {
                            when (it) {
                                is IrSimpleFunction -> sequenceOf(it)
                                is IrProperty -> listOfNotNull(it.getter).asSequence()
                                else -> emptySequence()
                            }
                        }.singleOrNull { it.name == callee.name && it.parameters.size == callee.parameters.size } ?: throw Abort("array member ${callee.name}")
                        return retarget(expression, target)
                    }
                }
                // Extensions of `Array<P>`.
                val receiverIndex = callee.parameters.indexOfFirst { it.kind == IrParameterKind.ExtensionReceiver }
                val receiverArray = if (receiverIndex >= 0 && callee.parameters[receiverIndex].type.classOrNull == irBuiltIns.arrayClass) expression.arguments[receiverIndex]?.type?.arrayStorageOrNull() else null
                if (receiverArray != null) {
                    val name = callee.name.asString()
                    if (name == "resetAt" || name == "resetRange") return IrGetObjectValueImpl(expression.startOffset, expression.endOffset,
                            irBuiltIns.unitType, irBuiltIns.unitClass)
                    val target = primitiveArrayExtensions[name to receiverArray]?.singleOrNull { it.parameters.size == callee.parameters.size }
                    if (target != null) return retarget(expression, target)
                }
                // Generic functions called with primitives (or their arrays): copies for them.
                if (callee.typeParameters.isNotEmpty() && expression.typeArguments.any { it != null && it.isSpecializableArgument() }) {
                    val copy = functionCopyFor(callee, expression.typeArguments)
                    if (copy != null) {
                        expression.symbol = copy.symbol
                        expression.type = expression.type.mapped()
                        return expression
                    }
                }
                // Members of copies (this one, or ones made before) get their types mapped as well.
                if (isInCopy(callee)) {
                    expression.type = expression.type.mapped()
                    return expression
                }
                if (expression.arguments.any { it?.type?.primitiveArrayOrNull() != null } || expression.type.primitiveArrayOrNull() != null)
                    throw Abort("call of ${callee.name} with a primitive Array")
                return expression
            }

            override fun visitTypeOperator(expression: IrTypeOperatorCall): IrExpression {
                expression.transformChildrenVoid(this)
                if (expression.typeOperand.primitiveArrayOrNull() != null) {
                    // Implicit casts between the same array types are fine; others are not mapped.
                    if (expression.operator != IrTypeOperator.IMPLICIT_CAST) throw Abort("type operator on a primitive Array")
                    expression.typeOperand = expression.typeOperand.mapped()
                }
                return expression
            }
        })
        // The types of declarations and expressions.
        root.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) {
                if (element is IrExpression) element.type = element.type.mapped()
                element.acceptChildrenVoid(this)
            }
            override fun visitField(declaration: IrField) { declaration.type = declaration.type.mapped(); super.visitField(declaration) }
            override fun visitVariable(declaration: IrVariable) { declaration.type = declaration.type.mapped(); super.visitVariable(declaration) }
            override fun visitValueParameter(declaration: IrValueParameter) { declaration.type = declaration.type.mapped(); super.visitValueParameter(declaration) }
            override fun visitFunction(declaration: IrFunction) { declaration.returnType = declaration.returnType.mapped(); super.visitFunction(declaration) }
        })
    }

    // Whether [function] is in a copy made by this pass (or being made), whose array types are mapped.
    private val copyRoots = mutableSetOf<IrDeclaration>()
    private fun isInCopy(function: IrDeclaration): Boolean {
        var current: IrDeclaration? = function
        while (current != null) {
            if (current in copyRoots) return true
            current = current.parent as? IrDeclaration
        }
        return false
    }

    private fun retarget(call: IrCall, target: IrSimpleFunction): IrExpression =
            IrCallImpl(call.startOffset, call.endOffset, call.type.mapped(), target.symbol, typeArgumentsCount = target.typeParameters.size).also {
                for (i in call.arguments.indices) it.arguments[i] = call.arguments[i]
            }

    private fun functionCopyFor(function: IrSimpleFunction, typeArguments: List<IrType?>): IrSimpleFunction? {
        if (function in copyRoots) return null
        if (function !in available || function.body == null || function.isExternal || function.isFakeOverride ||
                function.dispatchReceiverParameter != null) return null
        val key = Key(function, typeArguments.map { it?.takeIf { type -> type.isSpecializableArgument() } })
        functionCopies[key]?.let { return it }
        if (key in functionCopies || key in inProgress) return null
        inProgress += key
        val copy = try {
            function.copyWithSubstitution(function.parent, key.arguments).also {
                it.name = Name.identifier(function.name.asString() + suffix(key.arguments))
                copyRoots += it
                it.annotations = emptyList()
                mapPrimitiveArrays(it)
                when (val parent = function.parent) {
                    is IrFile -> parent.declarations += it
                    is IrClass -> parent.declarations += it
                    else -> throw Abort("unexpected parent")
                }
                available += it
                specializeAllocations(it)
            }
        } catch (e: Abort) {
            if (debug) System.err.println("Monomorphization: function ${function.name}${key.arguments}: ${e.message}")
            null
        }
        inProgress -= key
        functionCopies[key] = copy
        return copy
    }

    private companion object {
        const val NO_INIT_PARAMETER = "koreMonomorphized"
        // Classes the compiler handles specially.
        val EXCLUDED_PACKAGES = listOf("kotlin.native", "kotlin.internal", "kotlin.coroutines", "kotlin.concurrent", "kotlin.reflect", "kotlinx.cinterop", "kotlin.jvm", "kotlin.js", "kotlin.wasm")
    }
}
