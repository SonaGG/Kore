/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.backend.konan.optimizations

import org.jetbrains.kotlin.backend.common.IrElementTransformerVoidWithContext
import org.jetbrains.kotlin.backend.common.lower.createIrBuilder
import org.jetbrains.kotlin.backend.konan.Context
import org.jetbrains.kotlin.backend.konan.isInlined
import org.jetbrains.kotlin.backend.konan.lower.LOWERED_DELEGATING_CONSTRUCTOR_CALL
import org.jetbrains.kotlin.backend.konan.lower.loweredConstructorFunction
import org.jetbrains.kotlin.backend.konan.lower.originalConstructor
import org.jetbrains.kotlin.descriptors.ClassKind
import org.jetbrains.kotlin.descriptors.Modality
import org.jetbrains.kotlin.ir.IrElement
import org.jetbrains.kotlin.ir.IrStatement
import org.jetbrains.kotlin.ir.builders.*
import org.jetbrains.kotlin.ir.builders.declarations.buildField
import org.jetbrains.kotlin.ir.declarations.*
import org.jetbrains.kotlin.ir.expressions.*
import org.jetbrains.kotlin.ir.objcinterop.isObjCClass
import org.jetbrains.kotlin.ir.types.*
import org.jetbrains.kotlin.ir.util.*
import org.jetbrains.kotlin.ir.visitors.IrVisitor
import org.jetbrains.kotlin.ir.visitors.IrVisitorVoid
import org.jetbrains.kotlin.ir.visitors.acceptChildrenVoid
import org.jetbrains.kotlin.ir.visitors.acceptVoid
import org.jetbrains.kotlin.ir.visitors.transformChildrenVoid
import org.jetbrains.kotlin.name.Name

/**
 * Kore: stores fields of small immutable classes inline in the objects holding them, like the JVM's copying GC places
 * them next to each other (or Valhalla's value objects). For `class Entity(var pos: Vec3)` with
 * `data class Vec3(val x: Double, val y: Double, val z: Double)`, `Entity` gets the fields `pos$x`, `pos$y`, `pos$z`:
 * `e.pos.x` is one load from `e`, `e.pos = Vec3(a, b, c)` three stores, and other reads of `e.pos` create a new `Vec3`
 * from the fields, which escape analysis puts on the stack (or removes) where it doesn't escape.
 *
 * Runs on the whole program of an optimized executable (closed world), before the DFG is built, so that the pre-codegen
 * inliner and escape analysis see the rewritten code.
 *
 * Flattening changes the identity of values: each read creates a new object. A value class V qualifies only where that
 * is unobservable:
 *  - V is a final class with only `Any` as supertype (no interfaces), not inline, inner, external, ObjC or generic;
 *  - its instance fields are immutable non-null primitives, set exactly once each from the parameters of its only
 *    initializing constructor, with no other code in it (other constructors may only delegate to it);
 *  - the values of V never flow into a location of another static type: a parameter, variable, field, return value or
 *    cast of type `Any`, a type parameter, a vararg, a covariant override, an inline class wrapping V, etc. Then identity comparisons (`===` is a call with `Any?` parameters),
 *    `identityHashCode`, `WeakReference`, `freeze`, locks, generic containers and calls to the functions V inherits from
 *    `Any` (whose identity-based `equals`/`hashCode`/`toString` dispatch through `Any`) are all excluded, and so is
 *    everything we don't know about (external functions, unknown IR). Allowed exceptions: `x === null`, the
 *    `this === other` shortcut in V's generated data class `equals` (unobservable, as the structural comparison
 *    that follows is reflexive) and V-typed arguments of that `equals`.
 * A field F qualifies when it's an instance field of type exactly V (non-null) of a regular class or object C (not
 * inline, external, interop, enum), has no annotations (`@Volatile`, ...), no initializer, isn't `lateinit`, is only
 * used by field reads and writes inside functions, and C's instances are never created other than by its constructors,
 * each of which sets F before `this` can be seen by any other code (a read of an unset field would see a zero V instead
 * of `null`). Objects (whose instance other code can reach during initialization) must set F before calling anything.
 *
 * Racy (non-volatile, unsynchronized) concurrent reads of a flattened field can see a mix of two writes.
 */
internal class ValueFlattening(private val context: Context, private val module: IrModuleFragment) {
    private val irBuiltIns = context.irBuiltIns
    private val createUninitializedInstance = context.symbols.createUninitializedInstance
    private val log = System.getProperty("kore.valueFlattening.log") != null || System.getenv("KORE_VALUE_FLATTENING_LOG") != null

    // A value class: its fields, and its initializing constructor with the field each parameter sets.
    private class ValueClass(val irClass: IrClass, val fields: List<IrField>, val constructor: IrSimpleFunction, val parameterFields: List<IrField>)

    // A flattened field and the fields holding its components.
    private class Flattened(val field: IrField, val value: ValueClass, val components: Map<IrField, IrField>)

    private val valueClasses = mutableMapOf<IrClass, ValueClass>()
    private val rejectedValueClasses = mutableMapOf<IrClass, String>()
    private val rejectedFields = mutableMapOf<IrField, String>()
    private val holderFields = mutableSetOf<IrField>()
    // Classes whose instances may be created other than by a constructor call.
    private val unconstructedClasses = mutableSetOf<IrClass>()

    fun run() {
        module.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
            override fun visitClass(declaration: IrClass) {
                declaration.acceptChildrenVoid(this)
                valueClassOf(declaration)?.let { valueClasses[declaration] = it }
            }
        })
        if (log) println("ValueFlattening: candidates ${valueClasses.keys.map { it.fqNameWhenAvailable }}")
        if (valueClasses.isEmpty()) return
        module.accept(FlowChecker(), Use.Free)
        for ([irClass, reason] in rejectedValueClasses) {
            if (valueClasses.remove(irClass) != null && log) println("ValueFlattening: ${irClass.fqNameWhenAvailable} is not a value class: $reason")
        }
        if (valueClasses.isEmpty()) return

        val flattened = mutableMapOf<IrField, Flattened>()
        for (field in holderFields) {
            val value = valueClasses[field.type.classOrNull?.owner] ?: continue
            val reason = rejectedFields[field] ?: holderRejection(field)
            if (reason != null) {
                if (log) println("ValueFlattening: ${field.parentAsClass.fqNameWhenAvailable}.${field.name} is not flattened: $reason")
                continue
            }
            flattened[field] = flatten(field, value)
            if (log) println("ValueFlattening: flattened ${field.parentAsClass.name}.${field.name}")
        }
        if (flattened.isEmpty()) return

        module.transformChildrenVoid(Rewriter(flattened))

        // The layouts of the holders (and of their subclasses) may have been computed by lowerings already.
        module.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
            override fun visitClass(declaration: IrClass) {
                declaration.acceptChildrenVoid(this)
                if (declaration.getAllSuperclasses().any { superClass -> flattened.values.any { it.field.parent == superClass } }
                        || flattened.values.any { it.field.parent == declaration })
                    context.getLayoutBuilder(declaration).invalidateFields()
            }
        })
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Value classes.

    private fun IrClass.instanceFields(): List<IrField> = declarations.mapNotNull {
        when (it) {
            is IrField -> it
            is IrProperty -> it.backingField
            else -> null
        }
    }.filter { !it.isStatic }

    private fun valueClassOf(irClass: IrClass): ValueClass? {
        if (irClass.kind != ClassKind.CLASS || irClass.modality != Modality.FINAL) return null
        if (irClass.isInner || irClass.isValue || irClass.isExternal || irClass.isExpect || irClass.isInlined()) return null
        if (irClass.typeParameters.isNotEmpty() || irClass.isObjCClass()) return null
        if (irClass.superTypes.any { !it.isAny() }) return null
        if (irClass.annotations.isNotEmpty() && !irClass.isData) return null
        val fields = irClass.instanceFields()
        if (fields.isEmpty()) return null
        if (fields.any { !it.isFinal || it.initializer != null || it.annotations.isNotEmpty() || !it.type.isPrimitiveType(nullable = false) }) {
            if (log && irClass.isData) println("ValueFlattening: ${irClass.fqNameWhenAvailable} is not a value class: has mutable, nullable or non-primitive fields")
            return null
        }
        val constructors = irClass.declarations.filterIsInstance<IrConstructor>()
        val loweredConstructors = constructors.map { it.loweredConstructorFunction ?: return null }
        val initializing = loweredConstructors.filter { !delegatesOnly(it, loweredConstructors) }
        val constructor = initializing.singleOrNull()
        if (constructor == null) {
            if (log && irClass.isData) println("ValueFlattening: ${irClass.fqNameWhenAvailable} is not a value class: ${initializing.size} initializing constructors")
            return null
        }
        if (loweredConstructors.any { it != constructor && !delegatesOnly(it, listOf(constructor)) }) return null
        val parameterFields = initializedFields(constructor, fields)
        if (parameterFields == null) {
            if (log && irClass.isData) println("ValueFlattening: ${irClass.fqNameWhenAvailable} is not a value class: its constructor does more than set the fields")
            return null
        }
        return ValueClass(irClass, fields, constructor, parameterFields)
    }

    // The statements of a body, with composite statements expanded.
    private fun flatStatements(statements: List<IrStatement>): List<IrStatement> = statements.flatMap {
        if (it is IrComposite || it is IrBlock && it !is IrReturnableBlock) flatStatements(it.statements) else listOf(it)
    }

    private fun isUnitReturn(statement: IrStatement, function: IrFunction) = statement is IrReturn
            && statement.returnTargetSymbol == function.symbol
            && statement.value.let {
                it.type.isUnit() && (it is IrGetObjectValue || it is IrCall && it.arguments.isEmpty() && it.symbol.owner.name.asString() == "theUnitInstance")
            }

    private fun IrElement.uses(parameter: IrValueParameter): Boolean {
        var result = false
        acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) {
                if (!result) element.acceptChildrenVoid(this)
            }

            override fun visitGetValue(expression: IrGetValue) {
                if (expression.symbol == parameter.symbol) result = true
            }
        })
        return result
    }

    // Whether the lowered constructor [function] only delegates to one of [targets], without other uses of `this`.
    private fun delegatesOnly(function: IrSimpleFunction, targets: List<IrSimpleFunction>): Boolean {
        val body = function.body as? IrBlockBody ?: return false
        val thisParameter = function.dispatchReceiverParameter ?: return false
        var delegations = 0
        var fieldWrites = false
        var otherUses = false
        body.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
            override fun visitSetField(expression: IrSetField) {
                fieldWrites = true
                expression.acceptChildrenVoid(this)
            }

            override fun visitCall(expression: IrCall) {
                if (expression.origin == LOWERED_DELEGATING_CONSTRUCTOR_CALL && expression.symbol.owner in targets
                        && (expression.arguments[0] as? IrGetValue)?.symbol == thisParameter.symbol) {
                    delegations++
                    expression.arguments.drop(1).forEach { if (it?.uses(thisParameter) == true) otherUses = true }
                    expression.arguments.drop(1).forEach { it?.acceptVoid(this) }
                } else {
                    expression.acceptChildrenVoid(this)
                }
            }
        })
        if (delegations != 1 || fieldWrites) return false
        // `this` may only be the receiver of the delegating call.
        var thisUses = 0
        body.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
            override fun visitGetValue(expression: IrGetValue) {
                if (expression.symbol == thisParameter.symbol) thisUses++
            }
        })
        return !otherUses && thisUses == 1
    }

    // For each regular parameter of [constructor], the field it initializes, if the constructor sets each of [fields]
    // exactly once from a parameter, and does nothing else.
    private fun initializedFields(constructor: IrSimpleFunction, fields: List<IrField>): List<IrField>? {
        val body = constructor.body as? IrBlockBody ?: return null
        val thisParameter = constructor.dispatchReceiverParameter ?: return null
        val parameters = constructor.parameters.filter { it.kind == IrParameterKind.Regular }
        if (constructor.parameters.size != parameters.size + 1) return null
        val fieldOfParameter = arrayOfNulls<IrField>(parameters.size)
        val setFields = mutableSetOf<IrField>()
        val statements = flatStatements(body.statements)
        for ([index, statement] in statements.withIndex()) {
            if (index == statements.lastIndex && isUnitReturn(statement, constructor)) continue
            val setField = statement as? IrSetField ?: return null
            if ((setField.receiver as? IrGetValue)?.symbol != thisParameter.symbol) return null
            val field = setField.symbol.owner
            if (field !in fields || !setFields.add(field)) return null
            val parameter = (setField.value as? IrGetValue)?.symbol?.owner as? IrValueParameter ?: return null
            val parameterIndex = parameters.indexOf(parameter)
            if (parameterIndex < 0 || parameter.type != field.type) return null
            if (fieldOfParameter[parameterIndex] == null) fieldOfParameter[parameterIndex] = field
        }
        if (setFields.size != fields.size) return null
        return fieldOfParameter.map { it ?: return null }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Whole-program checks: where the values of each value class flow, and how the holder fields are used.

    private sealed class Use {
        // Flows into a location of the type.
        class To(val type: IrType) : Use()

        // Doesn't flow anywhere (discarded, or only inspected).
        object Free : Use()

        // Unknown.
        object Unknown : Use()
    }

    private fun valueClassOfType(type: IrType): IrClass? = type.classOrNull?.owner?.takeIf { it in valueClasses }

    private fun reject(irClass: IrClass, reason: String) {
        rejectedValueClasses.putIfAbsent(irClass, reason)
    }

    private fun isGeneratedEquals(function: IrFunction?, irClass: IrClass) = function is IrSimpleFunction
            && function.parent == irClass && function.name.asString() == "equals" && !function.isFakeOverride
            && function.origin == IrDeclarationOrigin.GENERATED_DATA_CLASS_MEMBER

    // V's implementation of [callee] (which a call with a V receiver dispatches to), if V declares one.
    private fun implementationIn(irClass: IrClass, callee: IrSimpleFunction): IrSimpleFunction? {
        if (callee.parent == irClass && !callee.isFakeOverride) return callee
        val real = (if (callee.isFakeOverride) callee.resolveFakeOverride() else callee) ?: return null
        if (real.parent == irClass) return real
        return irClass.declarations.filterIsInstance<IrSimpleFunction>().firstOrNull { !it.isFakeOverride && it.overrides(real) }
    }

    private inner class FlowChecker : IrVisitor<Unit, Use>() {
        private val functions = mutableListOf<IrFunction>()
        // `createUninitializedInstance` calls immediately followed by a constructor call on the instance.
        private val constructedAllocations = mutableSetOf<IrCall>()

        private fun check(expression: IrExpression, use: Use) {
            val irClass = valueClassOfType(expression.type) ?: return
            when (use) {
                is Use.Free -> {}
                is Use.To -> if (valueClassOfType(use.type) != irClass) reject(irClass, "flows into ${use.type.render()} at ${where(expression)}")
                is Use.Unknown -> reject(irClass, "used by ${expression::class.simpleName} at ${where(expression)}")
            }
        }

        private fun where(expression: IrExpression): String {
            val function = functions.lastOrNull() ?: return "?"
            return "${function.fqNameWhenAvailable ?: function.name}:${expression.startOffset}"
        }

        private fun IrElement.visit(use: Use) = accept(this@FlowChecker, use)

        private fun resultUse(type: IrType) = if (type.isUnit() || type.isNothing()) Use.Free else Use.To(type)

        override fun visitElement(element: IrElement, data: Use) {
            if (element is IrExpression) check(element, data)
            when (element) {
                is IrFunction -> {
                    // A covariant override returns the value to callers of the overridden function as another type.
                    valueClassOfType(element.returnType)?.let { irClass ->
                        if ((element as? IrSimpleFunction)?.overriddenSymbols?.any { valueClassOfType(it.owner.returnType) != irClass } == true)
                            reject(irClass, "returned by ${element.fqNameWhenAvailable}, which overrides a function of another return type")
                    }
                    functions += element
                    element.parameters.forEach { it.visit(Use.Free) }
                    when (val body = element.body) {
                        is IrExpressionBody -> body.expression.visit(Use.To(element.returnType))
                        is IrBlockBody -> body.visit(Use.Free)
                        null -> {}
                        else -> body.visit(Use.Unknown)
                    }
                    functions.removeAt(functions.lastIndex)
                }
                is IrValueParameter -> element.defaultValue?.expression?.visit(Use.To(element.type))
                is IrField -> {
                    noteHolderField(element)
                    // An inline class over a reference is represented by the reference itself, also as `Any`.
                    val parentClass = element.parent as? IrClass
                    if (parentClass != null && (parentClass.isValue || parentClass.isInlined()))
                        valueClassOfType(element.type)?.let { reject(it, "underlying value of the inline ${parentClass.fqNameWhenAvailable}") }
                    element.initializer?.expression?.visit(Use.To(element.type))
                }
                is IrVariable -> element.initializer?.visit(Use.To(element.type))
                is IrClass, is IrFile, is IrModuleFragment, is IrProperty, is IrAnonymousInitializer, is IrTypeAlias ->
                    element.acceptChildren(this, Use.Free)
                is IrBlockBody -> visitStatements(element.statements, null)
                is IrContainerExpression -> visitStatements(element.statements, element.type)
                is IrWhen -> element.branches.forEach {
                    it.condition.visit(Use.To(irBuiltIns.booleanType))
                    it.result.visit(resultUse(element.type))
                }
                is IrLoop -> {
                    element.condition.visit(Use.To(irBuiltIns.booleanType))
                    element.body?.visit(Use.Free)
                }
                is IrTry -> {
                    element.tryResult.visit(resultUse(element.type))
                    element.catches.forEach {
                        it.catchParameter.visit(Use.Free)
                        it.result.visit(resultUse(element.type))
                    }
                    element.finallyExpression?.visit(Use.Free)
                }
                is IrSetValue -> element.value.visit(Use.To(element.symbol.owner.type))
                is IrGetField -> {
                    noteFieldAccess(element)
                    element.receiver?.visit(Use.Free)
                }
                is IrSetField -> {
                    noteFieldAccess(element)
                    element.receiver?.visit(Use.Free)
                    element.value.visit(Use.To(element.symbol.owner.type))
                }
                is IrReturn -> {
                    val type = when (val target = element.returnTargetSymbol.owner) {
                        is IrFunction -> target.returnType
                        is IrReturnableBlock -> target.type
                        else -> null
                    }
                    element.value.visit(if (type == null) Use.Unknown else resultUse(type))
                }
                is IrTypeOperatorCall -> element.argument.visit(when (element.operator) {
                    IrTypeOperator.INSTANCEOF, IrTypeOperator.NOT_INSTANCEOF, IrTypeOperator.IMPLICIT_COERCION_TO_UNIT -> Use.Free
                    IrTypeOperator.CAST, IrTypeOperator.IMPLICIT_CAST, IrTypeOperator.SAFE_CAST, IrTypeOperator.IMPLICIT_NOTNULL,
                    IrTypeOperator.REINTERPRET_CAST -> Use.To(element.typeOperand)
                    else -> Use.Unknown
                })
                is IrThrow -> element.value.visit(Use.To(irBuiltIns.throwableType))
                is IrGetClass -> element.argument.visit(Use.Free)
                // The array of a vararg is generic (`Array<out T>`), so its elements can be read as `Any`.
                is IrVararg -> element.acceptChildren(this, Use.Unknown)
                is IrPropertyReference -> {
                    element.field?.let { rejectedFields.putIfAbsent(it.owner, "referenced by a property reference") }
                    element.acceptChildren(this, Use.Unknown)
                }
                is IrConstantObject -> {
                    unconstructedClasses += element.constructor.owner.constructedClass
                    element.acceptChildren(this, Use.Unknown)
                }
                is IrFunctionAccessExpression -> visitFunctionAccess(element)
                else -> element.acceptChildren(this, Use.Unknown)
            }
        }

        // The allocation call in `createUninitializedInstance<T>() as T` (the cast the generic return type needs).
        private fun unwrapNoCast(expression: IrExpression): IrExpression {
            var result = expression
            if (result is IrBlock && result.origin == STATEMENT_ORIGIN_NO_CAST_NEEDED)
                result = result.statements.singleOrNull() as? IrExpression ?: return expression
            if (result is IrTypeOperatorCall && result.operator == IrTypeOperator.IMPLICIT_CAST) result = result.argument
            return result
        }

        private fun visitStatements(statements: List<IrStatement>, type: IrType?) {
            for ([index, statement] in statements.withIndex()) {
                if (statement is IrVariable && index + 1 < statements.size) {
                    // The allocation of a lowered constructor call: `val inst = createUninitializedInstance<T>(); T.<init>(inst, ...)`.
                    val allocation = statement.initializer as? IrCall
                    val call = statements[index + 1] as? IrCall
                    if (allocation?.symbol == createUninitializedInstance && call?.symbol?.owner?.originalConstructor != null
                            && (call.arguments[0] as? IrGetValue)?.symbol == statement.symbol)
                        constructedAllocations += allocation
                }
                if (statement is IrSetField && statement.receiver == null && index + 1 < statements.size) {
                    // The allocation of an object, published in its static field (to be read by its own initialization)
                    // before the constructor call: `O.instance = createUninitializedInstance<O>(); O.<init>(O.instance)`.
                    // The constructor checks then make sure nothing can read it before its flattened fields are set.
                    val allocation = unwrapNoCast(statement.value) as? IrCall
                    val call = statements[index + 1] as? IrCall
                    val receiver = call?.arguments?.getOrNull(0) as? IrGetField
                    if (allocation?.symbol == createUninitializedInstance && call?.symbol?.owner?.originalConstructor != null
                            && receiver?.receiver == null && receiver?.symbol == statement.symbol)
                        constructedAllocations += allocation
                }
                val use = if (index == statements.lastIndex && type != null) resultUse(type) else Use.Free
                statement.visit(use)
            }
        }

        private fun visitFunctionAccess(expression: IrFunctionAccessExpression) {
            val callee = expression.symbol.owner
            if (callee.symbol == createUninitializedInstance && expression !in constructedAllocations)
                expression.typeArguments.firstOrNull()?.classOrNull?.owner?.let {
                    unconstructedClasses += it
                }
            if (callee.symbol == irBuiltIns.eqeqeqSymbol) {
                val comparesWithNull = expression.arguments.any { it is IrConst && it.value == null }
                val inGeneratedEquals = functions.lastOrNull().let { function ->
                    function != null && valueClasses.keys.any { isGeneratedEquals(function, it) }
                }
                expression.arguments.forEach {
                    val generatedEqualsThis = inGeneratedEquals && (it as? IrGetValue)?.symbol == functions.last().dispatchReceiverParameter?.symbol
                    it?.visit(if (comparesWithNull || generatedEqualsThis) Use.Free else Use.To(irBuiltIns.anyNType))
                }
                return
            }
            // The function a call on a value receiver dispatches to, which is V's own.
            var target: IrFunction = callee
            val dispatchReceiverIndex = callee.parameters.indexOfFirst { it.kind == IrParameterKind.DispatchReceiver }
            if (expression is IrCall && dispatchReceiverIndex >= 0 && expression.superQualifierSymbol == null) {
                val receiverClass = expression.arguments[dispatchReceiverIndex]?.type?.let { valueClassOfType(it) }
                if (receiverClass != null)
                    target = implementationIn(receiverClass, callee as IrSimpleFunction) ?: callee
            }
            val targetClass = target.parent as? IrClass
            val generatedEquals = targetClass != null && targetClass in valueClasses && isGeneratedEquals(target, targetClass)
            val external = target.isExternal || target.body == null && target.parent is IrExternalPackageFragment
                    && target.symbol != createUninitializedInstance
            expression.arguments.forEachIndexed { index, argument ->
                val parameter = target.parameters.getOrNull(index)
                val use = when {
                    parameter == null || external -> Use.Unknown
                    generatedEquals -> Use.Free
                    else -> Use.To(parameter.type)
                }
                argument?.visit(use)
            }
        }

        private fun noteHolderField(field: IrField) {
            if (!field.isStatic && !field.type.isMarkedNullable() && valueClassOfType(field.type) != null) holderFields += field
        }

        private fun noteFieldAccess(expression: IrFieldAccessExpression) {
            val field = expression.symbol.owner
            if (functions.isEmpty()) rejectedFields.putIfAbsent(field, "accessed outside of functions")
            // The fields of a value class are only set by its constructor.
            val owner = field.parent as? IrClass
            if (expression is IrSetField && owner != null && owner in valueClasses && functions.lastOrNull() != valueClasses[owner]?.constructor)
                reject(owner, "field ${field.name} set at ${where(expression)}")
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Holder fields.

    private fun holderRejection(field: IrField): String? {
        val holder = field.parent as? IrClass ?: return "not in a class"
        if (holder.kind != ClassKind.CLASS && holder.kind != ClassKind.OBJECT) return "in a ${holder.kind}"
        if (holder.isValue || holder.isInlined() || holder.isExternal || holder.isExpect || holder.isObjCClass()) return "in a special class"
        if (holder.annotations.any { it.type.classFqName?.asString()?.startsWith("kotlin.native.internal.") == true })
            return "in a class known to the runtime"
        if (field.annotations.isNotEmpty()) return "annotated"
        if (field.initializer != null) return "has an initializer"
        if (field.correspondingPropertySymbol?.owner?.isLateinit == true) return "lateinit"
        if (field.type.isMarkedNullable()) return "nullable"
        // An instance of the holder or of a subclass, with the field never set.
        if (unconstructedClasses.any { it == holder || holder in it.getAllSuperclasses() })
            return "instances are created other than by a constructor"
        val constructors = holder.declarations.filterIsInstance<IrConstructor>()
        if (constructors.isEmpty()) return "no constructors"
        for (constructor in constructors) {
            val function = constructor.loweredConstructorFunction ?: return "constructor not lowered"
            if (!setsBeforeUse(function, field, holder, mutableSetOf())) return "may be read before it is set"
        }
        return null
    }

    // Whether [element] uses `this` other than to read fields the constructor has already set.
    private fun IrElement.leaks(thisParameter: IrValueParameter, setFields: Set<IrField>): Boolean {
        var result = false
        acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) {
                if (!result) element.acceptChildrenVoid(this)
            }

            override fun visitGetField(expression: IrGetField) {
                if ((expression.receiver as? IrGetValue)?.symbol == thisParameter.symbol && expression.symbol.owner in setFields) return
                super.visitGetField(expression)
            }

            override fun visitGetValue(expression: IrGetValue) {
                if (expression.symbol == thisParameter.symbol) result = true
            }
        })
        return result
    }

    // Whether the lowered constructor [function] of [holder] sets [field] before `this` can be seen by other code.
    private fun setsBeforeUse(function: IrSimpleFunction, field: IrField, holder: IrClass, visiting: MutableSet<IrSimpleFunction>): Boolean {
        if (!visiting.add(function)) return false
        val body = function.body as? IrBlockBody ?: return false
        val thisParameter = function.dispatchReceiverParameter ?: return false
        val isObject = holder.kind == ClassKind.OBJECT
        val setFields = mutableSetOf<IrField>()
        for (statement in flatStatements(body.statements)) {
            if (statement is IrSetField && (statement.receiver as? IrGetValue)?.symbol == thisParameter.symbol) {
                if (statement.value.leaks(thisParameter, setFields)) return false
                if (isObject && !isSimple(statement.value)) return false
                if (statement.symbol.owner == field) return true
                setFields += statement.symbol.owner
                continue
            }
            if (statement is IrCall && statement.origin == LOWERED_DELEGATING_CONSTRUCTOR_CALL
                    && (statement.arguments[0] as? IrGetValue)?.symbol == thisParameter.symbol) {
                if (statement.arguments.drop(1).any { it?.leaks(thisParameter, setFields) == true }) return false
                if (isObject && statement.arguments.drop(1).any { it != null && !isSimple(it) }) return false
                val target = statement.symbol.owner
                if (target.parent == holder) return setsBeforeUse(target, field, holder, visiting)
                if (!doesNotLeak(target, mutableSetOf())) return false
                continue
            }
            if (statement.leaks(thisParameter, setFields)) return false
            if (isObject && !isSimple(statement)) return false
        }
        return false
    }

    // Whether a superclass constructor never lets `this` be seen by other code.
    private fun doesNotLeak(function: IrSimpleFunction, visiting: MutableSet<IrSimpleFunction>): Boolean {
        if (!visiting.add(function)) return false
        val body = function.body as? IrBlockBody ?: return false
        val thisParameter = function.dispatchReceiverParameter ?: return false
        val setFields = mutableSetOf<IrField>()
        for (statement in flatStatements(body.statements)) {
            if (statement is IrSetField && (statement.receiver as? IrGetValue)?.symbol == thisParameter.symbol) {
                if (statement.value.leaks(thisParameter, setFields)) return false
                setFields += statement.symbol.owner
                continue
            }
            if (statement is IrCall && statement.origin == LOWERED_DELEGATING_CONSTRUCTOR_CALL
                    && (statement.arguments[0] as? IrGetValue)?.symbol == thisParameter.symbol) {
                if (statement.arguments.drop(1).any { it?.leaks(thisParameter, setFields) == true }) return false
                if (!doesNotLeak(statement.symbol.owner, visiting)) return false
                continue
            }
            if (statement.leaks(thisParameter, setFields)) return false
        }
        return true
    }

    // Whether [element] calls no code that could reach an object under initialization: only constructors of value
    // classes and primitive operations.
    private fun isSimple(element: IrElement): Boolean {
        var simple = true
        element.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) {
                if (!simple) return
                when (element) {
                    is IrGetObjectValue -> simple = false
                    is IrFunctionAccessExpression -> {
                        val callee = element.symbol.owner
                        val allowed = callee.symbol == createUninitializedInstance
                                || callee.parent.let { it is IrClass && it in valueClasses && callee == valueClasses[it]?.constructor }
                                || callee.parent.let { it is IrClass && it.defaultType.isPrimitiveType() }
                                || callee.parent.let { it is IrExternalPackageFragment && it.packageFqName.asString() == "kotlin.internal.ir" }
                        if (!allowed) simple = false
                    }
                    is IrDeclaration -> if (element !is IrVariable) simple = false
                }
                element.acceptChildrenVoid(this)
            }
        })
        return simple
    }

    private fun flatten(field: IrField, value: ValueClass): Flattened {
        val holder = field.parentAsClass
        val components = value.fields.associateWith { component ->
            context.irFactory.buildField {
                startOffset = field.startOffset
                endOffset = field.endOffset
                name = Name.identifier("${field.name.asString()}\$${component.name.asString()}")
                type = component.type
                visibility = field.visibility
                isFinal = field.isFinal
                isStatic = false
            }.also { it.parent = holder }
        }
        val property = field.correspondingPropertySymbol?.owner
        if (property != null && property.backingField == field) {
            property.backingField = null
        }
        val index = holder.declarations.indexOf(field)
        if (index >= 0) holder.declarations.removeAt(index)
        holder.declarations.addAll(if (index >= 0) index else holder.declarations.size, components.values)
        return Flattened(field, value, components)
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Rewriting the accesses.

    private inner class Rewriter(private val flattened: Map<IrField, Flattened>) : IrElementTransformerVoidWithContext() {
        private fun builder(expression: IrExpression) =
                context.createIrBuilder(currentScope!!.scope.scopeOwnerSymbol, expression.startOffset, expression.endOffset)

        override fun visitGetField(expression: IrGetField): IrExpression {
            // `holder.f.x` reads `holder.f$x`.
            val inner = expression.receiver as? IrGetField
            val innerFlattened = inner?.let { flattened[it.symbol.owner] }
            val component = innerFlattened?.components?.get(expression.symbol.owner)
            if (component != null) {
                val receiver = inner.receiver!!.transform(this, null)
                return builder(expression).irGetField(receiver, component)
            }
            expression.transformChildrenVoid(this)
            val flat = flattened[expression.symbol.owner] ?: return expression
            val value = flat.value
            return builder(expression).irBlock(resultType = expression.type) {
                val holder = irTemporary(expression.receiver!!, nameHint = "holder")
                val instance = irTemporary(irCall(createUninitializedInstance, value.irClass.defaultType, listOf(value.irClass.defaultType)), nameHint = "inst")
                +irCall(value.constructor).apply {
                    arguments[0] = irGet(instance)
                    value.parameterFields.forEachIndexed { index, field ->
                        arguments[index + 1] = irGetField(irGet(holder), flat.components.getValue(field))
                    }
                }
                +irGet(instance)
            }
        }

        override fun visitSetField(expression: IrSetField): IrExpression {
            expression.transformChildrenVoid(this)
            val flat = flattened[expression.symbol.owner] ?: return expression
            val value = flat.value
            val newValue = expression.value
            return builder(expression).irBlock(resultType = expression.type) {
                val holder = irTemporary(expression.receiver!!, nameHint = "holder")
                val arguments = constructorArguments(newValue, value)
                if (arguments != null) {
                    // `holder.f = V(a, b)`: the arguments, in order, then the stores.
                    val temporaries = arguments.map { irTemporary(it, nameHint = "arg") }
                    for (field in value.fields) {
                        val parameterIndex = value.parameterFields.indexOf(field)
                        +irSetField(irGet(holder), flat.components.getValue(field), irGet(temporaries[parameterIndex]))
                    }
                } else {
                    val temporary = irTemporary(newValue, nameHint = "value")
                    for (field in value.fields)
                        +irSetField(irGet(holder), flat.components.getValue(field), irGetField(irGet(temporary), field))
                }
            }
        }

        // The arguments of [expression] if it is a lowered call of the initializing constructor of [value]:
        // `{ val inst = createUninitializedInstance<V>(); V.<init>(inst, args); inst }`.
        private fun constructorArguments(expression: IrExpression, value: ValueClass): List<IrExpression>? {
            val block = expression as? IrBlock ?: return null
            val statements = block.statements
            if (statements.size != 3) return null
            val instance = statements[0] as? IrVariable ?: return null
            if ((instance.initializer as? IrCall)?.symbol != createUninitializedInstance) return null
            val call = statements[1] as? IrCall ?: return null
            if (call.symbol.owner != value.constructor || (call.arguments[0] as? IrGetValue)?.symbol != instance.symbol) return null
            if ((statements[2] as? IrGetValue)?.symbol != instance.symbol) return null
            val arguments = call.arguments.drop(1)
            if (arguments.any { it == null || it.uses(instance) }) return null
            return arguments.map { it!! }
        }

        private fun IrElement.uses(variable: IrVariable): Boolean {
            var result = false
            acceptVoid(object : IrVisitorVoid() {
                override fun visitElement(element: IrElement) {
                    if (!result) element.acceptChildrenVoid(this)
                }

                override fun visitGetValue(expression: IrGetValue) {
                    if (expression.symbol == variable.symbol) result = true
                }
            })
            return result
        }
    }
}
