/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.backend.konan.lower

import org.jetbrains.kotlin.backend.konan.Context
import org.jetbrains.kotlin.ir.IrElement
import org.jetbrains.kotlin.ir.declarations.*
import org.jetbrains.kotlin.ir.expressions.*
import org.jetbrains.kotlin.ir.expressions.impl.*
import org.jetbrains.kotlin.ir.symbols.IrClassSymbol
import org.jetbrains.kotlin.ir.symbols.IrSimpleFunctionSymbol
import org.jetbrains.kotlin.ir.types.*
import org.jetbrains.kotlin.ir.util.*
import org.jetbrains.kotlin.ir.visitors.IrVisitorVoid
import org.jetbrains.kotlin.ir.visitors.acceptChildrenVoid
import org.jetbrains.kotlin.ir.visitors.acceptVoid

/**
 * Kore: local `HashMap<Int, Int>`s become `IntIntHashMap`s, which keep their keys and values in `IntArray`s, and their
 * `put`/`get`/`containsKey` calls take and return primitives, so that nothing is boxed.
 *
 * Only maps whose every use is known are replaced: a `val` initialized by `HashMap<Int, Int>()` or
 * `HashMap<Int, Int>(capacity)` (or `LinkedHashMap`), all of whose reads (also through the temporaries of inlined
 * functions such as `MutableMap.set`) are receivers of map member calls or parts of string templates, so the map never
 * leaves the function (no code can check its class, cast it or put other types into it). `IntIntHashMap` has the same
 * behavior as `HashMap` (see its documentation).
 *
 * `val t: Int? = map.get(k)` whose uses are all null checks and smart casts becomes a `Long` with
 * `IntIntHashMap.MISSING` for null (the `?:`, `if (t != null)` patterns), so the value is not boxed either.
 *
 * Runs after all inline functions have been inlined (the shapes above are those inlining produces), before the other
 * lowerings (boxing is inserted later, by the types this pass leaves).
 */
internal class PrimitiveCollections(private val context: Context, private val modules: List<IrModuleFragment>) {
    private val irBuiltIns = context.irBuiltIns
    // Found in the modules compiled here (not in a cached standard library, whose IR is not available).
    private fun findClass(fqName: String): IrClassSymbol? {
        for (module in modules) for (file in module.files) {
            if (file.packageFqName.asString() != "kotlin.collections") continue
            for (declaration in file.declarations)
                if (declaration is IrClass && declaration.fqNameWhenAvailable?.asString() == fqName) return declaration.symbol
        }
        return null
    }

    private val hashMapClass: IrClassSymbol? by lazy { findClass("kotlin.collections.HashMap") }
    private val intIntMapClass: IrClassSymbol? by lazy { context.symbols.intIntHashMap.takeIf { it.isBound } }

    private val longToInt: IrSimpleFunctionSymbol by lazy {
        irBuiltIns.longClass.owner.declarations.filterIsInstance<IrSimpleFunction>()
                .single { it.name.asString() == "toInt" && it.parameters.size == 1 }.symbol
    }

    private val debug = System.getenv("KORE_DEBUG_PRIMITIVE_COLLECTIONS") != null

    fun run() {
        if (debug) System.err.println("PrimitiveCollections: HashMap=${hashMapClass != null} IntIntHashMap=${intIntMapClass != null} modules=${modules.map { it.name }}")
        val hashMap = hashMapClass ?: return
        val intIntMap = intIntMapClass ?: return
        for (module in modules) for (file in module.files) {
            file.acceptVoid(object : IrVisitorVoid() {
                override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)

                override fun visitFunction(declaration: IrFunction) {
                    declaration.acceptChildrenVoid(this)
                    declaration.body?.let { FunctionRewriter(it, hashMap, intIntMap).run() }
                }
            })
        }
    }

    private inner class FunctionRewriter(val body: IrBody, val hashMap: IrClassSymbol, val intIntMap: IrClassSymbol) {
        // The parent of every element of the body (in the function itself, not in local functions).
        private val parents = mutableMapOf<IrElement, IrElement>()
        private val reads = mutableMapOf<IrValueDeclaration, MutableList<IrGetValue>>()
        private val variables = mutableListOf<IrVariable>()

        fun run() {
            body.acceptChildrenVoid(object : IrVisitorVoid() {
                private val stack = ArrayList<IrElement>()

                override fun visitElement(element: IrElement) {
                    stack.lastOrNull()?.let { parents[element] = it } ?: run { parents[element] = body }
                    stack += element
                    element.acceptChildrenVoid(this)
                    stack.removeAt(stack.lastIndex)
                }

                override fun visitFunction(declaration: IrFunction) {
                    // Reads in local functions and lambdas are captures: they make the map escape.
                    declaration.acceptVoid(object : IrVisitorVoid() {
                        override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
                        override fun visitGetValue(expression: IrGetValue) {
                            reads.getOrPut(expression.symbol.owner) { mutableListOf() } += expression
                            parents[expression] = declaration
                        }
                    })
                }

                override fun visitVariable(declaration: IrVariable) {
                    variables += declaration
                    visitElement(declaration)
                }

                override fun visitGetValue(expression: IrGetValue) {
                    reads.getOrPut(expression.symbol.owner) { mutableListOf() } += expression
                    visitElement(expression)
                }
            })
            for (variable in variables) {
                val init = variable.initializer as? IrConstructorCall ?: continue
                if (!isIntIntHashMapCandidate(init) || variable.isVar) continue
                val aliases = mutableListOf<IrVariable>()
                val ok = collectAliases(variable, aliases)
                if (debug) System.err.println("PrimitiveCollections: ${variable.name} replaced=$ok aliases=${aliases.map { it.name }}")
                if (!ok) continue
                rewrite(variable, init, aliases)
            }
        }

        private fun isIntIntHashMapCandidate(call: IrConstructorCall): Boolean {
            val constructor = call.symbol.owner
            if (constructor.constructedClass.symbol != hashMap) return false
            val params = constructor.parameters
            if (params.size > 1 || (params.size == 1 && !params[0].type.isInt())) return false
            return call.typeArguments.size == 2 && call.typeArguments.all { it != null && it.isInt() && !it.isMarkedNullable() }
        }

        // The map variable and the temporaries holding it; false if one of their reads is not a known use.
        private fun collectAliases(variable: IrVariable, aliases: MutableList<IrVariable>): Boolean {
            if (variable in aliases) return true
            aliases += variable
            for (read in reads[variable].orEmpty()) {
                var use: IrElement = read
                var parent = parents[use] ?: return false
                // Implicit casts to supertypes (e.g. to `MutableMap<Any?, Any?>` for an inlined extension).
                while (parent is IrTypeOperatorCall && parent.operator == IrTypeOperator.IMPLICIT_CAST) {
                    use = parent
                    parent = parents[use] ?: return false
                }
                when {
                    parent is IrVariable && parent.initializer === use && !parent.isVar -> {
                        if (!collectAliases(parent, aliases)) return false
                    }
                    parent is IrCall && parent.arguments.getOrNull(0) === use && parent.symbol.owner.dispatchReceiverParameter != null -> {
                        if (counterpart(parent.symbol.owner) == null) return false
                    }
                    parent is IrStringConcatenation -> {}
                    else -> return false
                }
            }
            return true
        }

        private val supportedNames = setOf(
                "get", "put", "containsKey", "containsValue", "remove", "clear", "isEmpty", "<get-size>", "<get-keys>",
                "<get-values>", "<get-entries>", "putAll", "toString", "hashCode", "equals"
        )

        // The member of `IntIntHashMap` a call of a map member becomes.
        private fun counterpart(function: IrSimpleFunction): IrSimpleFunction? {
            val name = function.name.asString()
            if (name !in supportedNames) return null
            val arity = function.parameters.size
            return intIntMap.owner.declarations.asSequence().flatMap {
                when (it) {
                    is IrSimpleFunction -> sequenceOf(it)
                    is IrProperty -> listOfNotNull(it.getter).asSequence()
                    else -> emptySequence()
                }
            }.firstOrNull { it.name.asString() == name && it.parameters.size == arity }
        }

        private fun function(name: String): IrSimpleFunction =
                intIntMap.owner.declarations.filterIsInstance<IrSimpleFunction>().single { it.name.asString() == name }

        private fun rewrite(variable: IrVariable, init: IrConstructorCall, aliases: List<IrVariable>) {
            val mapType = intIntMap.typeWith(irBuiltIns.intType, irBuiltIns.intType)
            // The allocation.
            val constructor = intIntMap.owner.declarations.filterIsInstance<IrConstructor>()
                    .single { it.parameters.size == init.arguments.size }
            variable.initializer = IrConstructorCallImpl.fromSymbolOwner(init.startOffset, init.endOffset, mapType, constructor.symbol).also {
                it.typeArguments[0] = irBuiltIns.intType
                it.typeArguments[1] = irBuiltIns.intType
                init.arguments.forEachIndexed { index, argument -> it.arguments[index] = argument }
            }
            for (alias in aliases) {
                alias.type = mapType
                if (alias !== variable) alias.initializer = stripImplicitCasts(alias.initializer!!).also { it.type = mapType }
                for (read in reads[alias].orEmpty()) {
                    read.type = mapType
                    val parent = parents[read]
                    if (parent is IrCall && parent.arguments.getOrNull(0) === read) retarget(parent)
                    else if (parent is IrTypeOperatorCall) retargetThroughCasts(parent)
                }
            }
        }

        private fun retargetThroughCasts(cast: IrTypeOperatorCall) {
            var use: IrElement = cast
            var parent = parents[use]
            while (parent is IrTypeOperatorCall && parent.operator == IrTypeOperator.IMPLICIT_CAST) {
                use = parent
                parent = parents[use]
            }
            if (parent is IrCall && parent.arguments.getOrNull(0) === use) {
                parent.arguments[0] = stripImplicitCasts(use)
                retarget(parent)
            }
        }

        private fun stripImplicitCasts(expression: IrExpression): IrExpression {
            var result = expression
            while (result is IrTypeOperatorCall && result.operator == IrTypeOperator.IMPLICIT_CAST) result = result.argument
            return result
        }

        private fun retarget(call: IrCall) {
            val target = counterpart(call.symbol.owner) ?: return
            val name = target.name.asString()
            when {
                name == "put" -> {
                    val key = intSource(call.arguments[1]!!)
                    val value = intSource(call.arguments[2]!!)
                    if (key != null && value != null && parents[call].let { it is IrTypeOperatorCall && it.operator == IrTypeOperator.IMPLICIT_COERCION_TO_UNIT }) {
                        call.symbol = function("putInt").symbol
                        call.arguments[1] = key
                        call.arguments[2] = value
                        call.type = irBuiltIns.longType
                        return
                    }
                }
                name == "containsKey" -> {
                    val key = intSource(call.arguments[1]!!)
                    if (key != null) {
                        call.symbol = function("containsKeyInt").symbol
                        call.arguments[1] = key
                        return
                    }
                }
                name == "get" -> {
                    val key = intSource(call.arguments[1]!!)
                    val holder = parents[call] as? IrVariable
                    if (key != null && holder != null && holder.initializer === call && !holder.isVar && narrowable(holder)) {
                        call.symbol = function("getOrMissing").symbol
                        call.arguments[1] = key
                        call.type = irBuiltIns.longType
                        narrow(holder)
                        return
                    }
                }
            }
            call.symbol = target.symbol
        }

        // The `Int` an argument is, through implicit casts to `Any?` and temporaries read only here; changes those
        // temporaries to `Int`s. Null if the argument is not known to be an `Int`.
        private fun intSource(argument: IrExpression): IrExpression? {
            val stripped = stripImplicitCasts(argument)
            if (stripped.type.isInt() && !stripped.type.isMarkedNullable()) return stripped
            if (stripped !is IrGetValue) return null
            val variable = stripped.symbol.owner as? IrVariable ?: return null
            if (variable.isVar || reads[variable]?.size != 1) return null
            val initializer = variable.initializer ?: return null
            val source = intSource(initializer) ?: return null
            variable.initializer = source
            variable.type = irBuiltIns.intType
            stripped.type = irBuiltIns.intType
            return stripped
        }

        // Whether every use of an `Int?` holding a lookup result is a null check or a read as an `Int`.
        private fun narrowable(holder: IrVariable): Boolean = reads[holder].orEmpty().all { read ->
            val parent = parents[read]
            when {
                parent is IrCall && parent.symbol == irBuiltIns.eqeqSymbol ->
                    parent.arguments.any { it is IrConst && it.value == null }
                parent is IrTypeOperatorCall && parent.typeOperand.isInt() && !parent.typeOperand.isMarkedNullable() &&
                        parent.operator in setOf(IrTypeOperator.IMPLICIT_CAST, IrTypeOperator.IMPLICIT_NOTNULL, IrTypeOperator.CAST) -> true
                // The `else` of `?:`: a branch result of a `when` of type `Int`.
                parent is IrBranch && parent.result === read -> (parents[parent] as? IrWhen)?.type?.let { it.isInt() && !it.isMarkedNullable() } == true
                else -> false
            }
        }

        private fun narrow(holder: IrVariable) {
            holder.type = irBuiltIns.longType
            for (read in reads[holder].orEmpty()) {
                read.type = irBuiltIns.longType
                val parent = parents[read]
                when {
                    parent is IrCall && parent.symbol == irBuiltIns.eqeqSymbol -> {
                        val index = parent.arguments.indexOfFirst { it is IrConst && it.value == null }
                        parent.arguments[index] = IrConstImpl.long(read.startOffset, read.endOffset, irBuiltIns.longType, Long.MIN_VALUE)
                    }
                    parent is IrTypeOperatorCall -> {
                        parent.operator = IrTypeOperator.IMPLICIT_CAST
                        parent.typeOperand = irBuiltIns.intType
                        parent.argument = toInt(read)
                    }
                    parent is IrBranch -> parent.result = toInt(read)
                }
            }
        }

        private fun toInt(read: IrGetValue): IrExpression =
                IrCallImpl(read.startOffset, read.endOffset, irBuiltIns.intType, longToInt).also { it.arguments[0] = read }
    }
}
