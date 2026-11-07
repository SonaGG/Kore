/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.backend.konan.optimizations

import org.jetbrains.kotlin.backend.common.FileLoweringPass
import org.jetbrains.kotlin.backend.common.IrElementTransformerVoidWithContext
import org.jetbrains.kotlin.backend.common.lower.createIrBuilder
import org.jetbrains.kotlin.backend.konan.Context
import org.jetbrains.kotlin.backend.konan.ir.KonanNameConventions
import org.jetbrains.kotlin.ir.IrStatement
import org.jetbrains.kotlin.ir.builders.*
import org.jetbrains.kotlin.ir.declarations.*
import org.jetbrains.kotlin.ir.expressions.*
import org.jetbrains.kotlin.ir.symbols.IrClassSymbol
import org.jetbrains.kotlin.ir.symbols.IrSimpleFunctionSymbol
import org.jetbrains.kotlin.ir.symbols.IrValueSymbol
import org.jetbrains.kotlin.ir.types.classOrNull
import org.jetbrains.kotlin.ir.util.deepCopyWithSymbols
import org.jetbrains.kotlin.ir.util.functions
import org.jetbrains.kotlin.ir.util.parentClassOrNull
import org.jetbrains.kotlin.ir.visitors.IrElementTransformerVoid
import org.jetbrains.kotlin.ir.visitors.IrVisitorVoid
import org.jetbrains.kotlin.ir.visitors.acceptChildrenVoid
import org.jetbrains.kotlin.ir.visitors.acceptVoid
import org.jetbrains.kotlin.ir.visitors.transformChildrenVoid

/**
 * Kore: unroll-and-jam of reductions in loop nests, such as the dot products of a matrix multiplication:
 *
 * ```
 * for (j in a..b) {
 *     var s = 0.0
 *     for (k in c..d) s += x[i][k] * y[j][k]
 *     z[i][j] = s
 * }
 * ```
 *
 * Each sum is a chain of additions, each waiting for the previous one (floating point additions may not be reordered),
 * so the loop runs at the latency of an addition while the CPU could do several at once. This lowering runs
 * [FACTOR] iterations of the outer loop together: one inner loop updates [FACTOR] independent sums, each in its original
 * order (the results are bit for bit the same), and the stores follow in their original order.
 *
 * The jammed loop evaluates the sums of later outer iterations before the stores of earlier ones, and without bounds
 * checks. So before each group of iterations, it checks that none of their array accesses can fail and that the arrays
 * it stores into are not arrays it reads: when any check fails, that group and the rest of the loop run the original
 * code, which then fails (or reads the stored values) exactly where it did. The sums only read arrays and local values,
 * with arithmetic that cannot throw, so evaluating them earlier is not observable otherwise.
 */
internal class ReductionUnrollAndJam(private val context: Context) : FileLoweringPass {
    private val irBuiltIns = context.irBuiltIns
    private val symbols = context.symbols

    private val arrayClasses: Set<IrClassSymbol> by lazy {
        setOf(irBuiltIns.arrayClass, irBuiltIns.doubleArray, irBuiltIns.floatArray, irBuiltIns.intArray, irBuiltIns.longArray)
    }
    private val numberClasses: Set<IrClassSymbol> by lazy {
        setOf(irBuiltIns.doubleClass, irBuiltIns.floatClass, irBuiltIns.intClass, irBuiltIns.longClass)
    }
    private val floatingClasses: Set<IrClassSymbol> by lazy { setOf(irBuiltIns.doubleClass, irBuiltIns.floatClass) }

    private fun arrayGet(cls: IrClassSymbol) = symbols.arrayGet[cls]
    private fun arraySet(cls: IrClassSymbol) = symbols.arraySet[cls]
    private fun uncheckedGet(cls: IrClassSymbol) = cls.owner.functions.single { it.name == KonanNameConventions.getWithoutBoundCheck }.symbol
    private fun uncheckedSet(cls: IrClassSymbol) = cls.owner.functions.single { it.name == KonanNameConventions.setWithoutBoundCheck }.symbol

    private val intToLong by lazy {
        irBuiltIns.intClass.owner.functions.single { it.name.asString() == "toLong" && it.parameters.size == 1 }.symbol
    }
    private val longMinus by lazy {
        irBuiltIns.longClass.owner.functions.single {
            it.name.asString() == "minus" && it.parameters.size == 2 && it.parameters[1].type.classOrNull == irBuiltIns.longClass
        }.symbol
    }

    override fun lower(irFile: IrFile) {
        irFile.transformChildrenVoid(object : IrElementTransformerVoidWithContext() {
            override fun visitBlock(expression: IrBlock): IrExpression {
                if (expression.origin == IrStatementOrigin.FOR_LOOP) {
                    val owner = currentDeclarationParent
                    val scopeOwner = currentScope?.scope?.scopeOwnerSymbol
                    if (owner != null && scopeOwner != null) {
                        val nest = match(expression)
                        if (nest != null) return transform(nest, scopeOwner, owner)
                    }
                }
                return super.visitBlock(expression)
            }
        })
    }

    // ---------------------------------------------------------------------------------------------------------------------
    // Matching.

    private class ForLoop(val block: IrBlock, val range: IrCall, val variable: IrVariable, val body: List<IrStatement>,
                          val inclusive: Boolean)

    private class Read(val call: IrCall, val arrayClass: IrClassSymbol, val indexedByInner: Boolean)

    private class Nest(val outer: ForLoop, val inner: ForLoop, val sum: IrVariable, val plus: IrSimpleFunctionSymbol,
                       val term: IrExpression, val termReads: List<Read>, val stores: List<IrCall>,
                       val storeReads: List<Read>)

    /** `for (v in start..end)` (or `until`, `..<`) over Ints, before the for loops lowering. */
    private fun matchForLoop(block: IrBlock): ForLoop? {
        if (block.origin != IrStatementOrigin.FOR_LOOP || block.statements.size != 2) return null
        val iterator = block.statements[0] as? IrVariable ?: return null
        val loop = block.statements[1] as? IrWhileLoop ?: return null
        if (loop.origin != IrStatementOrigin.FOR_LOOP_INNER_WHILE) return null
        val iteratorCall = iterator.initializer as? IrCall ?: return null
        val range = iteratorCall.arguments.singleOrNull() as? IrCall ?: return null
        val rangeFunction = range.symbol.owner
        val inclusive = when (rangeFunction.name.asString()) {
            "rangeTo" -> true
            "until", "rangeUntil" -> false
            else -> return null
        }
        if (range.arguments.size != 2 || range.arguments.any { it?.type?.classOrNull != irBuiltIns.intClass }) return null
        if (range.type.classOrNull?.owner?.name?.asString() != "IntRange") return null
        val body = loop.body as? IrBlock ?: return null
        val variable = body.statements.firstOrNull() as? IrVariable ?: return null
        if (variable.origin != IrDeclarationOrigin.FOR_LOOP_VARIABLE) return null
        var rest = body.statements.drop(1)
        while (rest.size == 1 && (rest[0] as? IrBlock)?.let { it.origin == null && it !is IrReturnableBlock } == true)
            rest = (rest[0] as IrBlock).statements
        return ForLoop(block, range, variable, rest, inclusive)
    }

    private fun match(block: IrBlock): Nest? {
        val outer = matchForLoop(block) ?: return null
        if (outer.body.size < 3) return null
        val sum = outer.body[0] as? IrVariable ?: return null
        if (!sum.isVar || sum.type.classOrNull !in numberClasses || sum.initializer !is IrConst) return null
        val innerBlock = outer.body[1] as? IrBlock ?: return null
        val inner = matchForLoop(innerBlock) ?: return null
        // The inner bounds are evaluated once per group: they must not depend on the outer iteration.
        val outerLocals = setOf<IrValueSymbol>(outer.variable.symbol, sum.symbol)
        if (inner.range.arguments.any { !isInvariantBound(it!!, outerLocals) }) return null
        val update = inner.body.singleOrNull() as? IrSetValue ?: return null
        if (update.symbol != sum.symbol) return null
        val plusCall = update.value as? IrCall ?: return null
        val plus = plusCall.symbol
        if (plus.owner.name.asString() != "plus" || plus.owner.parentClassOrNull?.symbol != sum.type.classOrNull) return null
        if (plusCall.arguments.size != 2 || (plusCall.arguments[0] as? IrGetValue)?.symbol != sum.symbol) return null
        if (plusCall.arguments[1]!!.type.classOrNull != sum.type.classOrNull) return null
        val term = plusCall.arguments[1]!!
        val termReads = mutableListOf<Read>()
        if (!isPureTerm(term, inner.variable.symbol, sum.symbol, termReads)) return null
        val stores = mutableListOf<IrCall>()
        val storeReads = mutableListOf<Read>()
        for (statement in outer.body.drop(2)) {
            val store = statement as? IrCall ?: return null
            val arrayClass = store.arguments.getOrNull(0)?.type?.classOrNull ?: return null
            if (arrayClass !in arrayClasses || store.symbol != arraySet(arrayClass) || store.arguments.size != 3) return null
            if (!isPureTerm(store.arguments[0]!!, null, sum.symbol, storeReads)) return null
            if (!isPureTerm(store.arguments[1]!!, null, sum.symbol, storeReads)) return null
            if ((store.arguments[2] as? IrGetValue)?.symbol != sum.symbol) return null
            stores.add(store)
        }
        return Nest(outer, inner, sum, plus, term, termReads, stores, storeReads)
    }

    /** Constants, values and Int arithmetic that does not read the given values. */
    private fun isInvariantBound(expression: IrExpression, excluded: Set<IrValueSymbol>): Boolean = when (expression) {
        is IrConst -> true
        is IrGetValue -> expression.symbol !in excluded
        is IrCall -> {
            val function = expression.symbol.owner
            function.parentClassOrNull?.symbol == irBuiltIns.intClass && function.name.asString() in setOf("plus", "minus", "times") &&
                    expression.arguments.all { it != null && isInvariantBound(it, excluded) }
        }
        else -> false
    }

    /**
     * An expression with no effect but possibly failing array reads: constants, values (but [sum]), arithmetic that cannot
     * throw, and array reads whose index is invariant in the inner loop or its variable [inner] itself. The reads go to
     * [reads], each after the ones it depends on.
     */
    private fun isPureTerm(expression: IrExpression, inner: IrValueSymbol?, sum: IrValueSymbol, reads: MutableList<Read>): Boolean {
        return when (expression) {
            is IrConst -> true
            is IrGetValue -> expression.symbol != sum
            is IrCall -> {
                val function = expression.symbol.owner
                val parentClass = function.parentClassOrNull?.symbol
                if (parentClass in arrayClasses && expression.symbol == arrayGet(parentClass!!)) {
                    val receiver = expression.arguments[0] ?: return false
                    val index = expression.arguments[1] ?: return false
                    if (!isPureTerm(receiver, inner, sum, reads)) return false
                    if (inner != null && mentions(receiver, inner)) return false
                    val byInner = inner != null && (index as? IrGetValue)?.symbol == inner
                    if (!byInner) {
                        if (!isPureTerm(index, inner, sum, reads)) return false
                        if (inner != null && mentions(index, inner)) return false
                    }
                    reads.add(Read(expression, parentClass, byInner))
                    true
                } else if (parentClass in numberClasses) {
                    val name = function.name.asString()
                    val allowed = name in setOf("plus", "minus", "times", "unaryMinus") ||
                            (name == "div" && parentClass in floatingClasses &&
                                    expression.arguments.getOrNull(1)?.type?.classOrNull in floatingClasses)
                    allowed && expression.arguments.all { it != null && it.type.classOrNull in numberClasses && isPureTerm(it, inner, sum, reads) }
                } else false
            }
            else -> false
        }
    }

    private fun mentions(expression: IrExpression, value: IrValueSymbol): Boolean {
        var found = false
        expression.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: org.jetbrains.kotlin.ir.IrElement) { if (!found) element.acceptChildrenVoid(this) }
            override fun visitGetValue(expression: IrGetValue) { if (expression.symbol == value) found = true }
        })
        return found
    }

    // ---------------------------------------------------------------------------------------------------------------------
    // Transformation.

    /** Replaces reads of values (after a deep copy) and makes array accesses unchecked if asked. */
    private class Rewriter(val values: Map<IrValueSymbol, () -> IrExpression>, val unchecked: (IrCall) -> IrCall?) : IrElementTransformerVoid() {
        override fun visitGetValue(expression: IrGetValue): IrExpression = values[expression.symbol]?.invoke() ?: expression
        override fun visitCall(expression: IrCall): IrExpression {
            expression.transformChildrenVoid(this)
            return unchecked(expression) ?: expression
        }
    }

    private fun IrBuilderWithScope.uncheckedAccess(call: IrCall): IrCall? {
        val cls = call.symbol.owner.parentClassOrNull?.symbol ?: return null
        if (cls !in arrayClasses) return null
        val replacement = when (call.symbol) {
            arrayGet(cls) -> uncheckedGet(cls)
            arraySet(cls) -> uncheckedSet(cls)
            else -> return null
        }
        return irCall(replacement, call.type).also { copy -> call.arguments.forEachIndexed { i, a -> copy.arguments[i] = a } }
    }

    private fun IrExpression.rewritten(values: Map<IrValueSymbol, () -> IrExpression>, unchecked: (IrCall) -> IrCall?): IrExpression =
            deepCopyWithSymbols().transform(Rewriter(values, unchecked), null)

    private fun transform(nest: Nest, scopeOwner: org.jetbrains.kotlin.ir.symbols.IrSymbol, parent: IrDeclarationParent): IrExpression {
        val outer = nest.outer
        val builder = context.createIrBuilder(scopeOwner, outer.block.startOffset, outer.block.endOffset)
        return builder.irBlock(resultType = irBuiltIns.unitType) {
            val start = irTemporary(outer.range.arguments[0]!!, nameHint = "jamStart")
            val end = irTemporary(outer.range.arguments[1]!!, nameHint = "jamEnd")
            val current = irTemporary(irGet(start), nameHint = "jam", isMutable = true)

            fun intPlus(a: IrExpression, b: Int) = irCall(irBuiltIns.intPlusSymbol).apply { arguments[0] = a; arguments[1] = irInt(b) }
            fun toLong(a: IrExpression) = irCall(intToLong).apply { arguments[0] = a }
            fun lessThan(a: IrExpression, b: IrExpression) =
                    irCall(irBuiltIns.lessFunByOperandType.getValue(irBuiltIns.intClass)).apply { arguments[0] = a; arguments[1] = b }
            fun not(a: IrExpression) = irCall(irBuiltIns.booleanNotSymbol).apply { arguments[0] = a }
            fun size(array: IrExpression, cls: IrClassSymbol) = irCall(symbols.arraySize[cls]!!).apply { arguments[0] = array }

            // A group needs FACTOR iterations left: end - j >= FACTOR - 1 (inclusive) or FACTOR (exclusive), in Longs.
            val needed = if (outer.inclusive) FACTOR - 1L else FACTOR.toLong()
            val fast = irWhile().apply {
                condition = irCall(irBuiltIns.greaterOrEqualFunByOperandType.getValue(irBuiltIns.longClass)).apply {
                    arguments[0] = irCall(longMinus).apply { arguments[0] = toLong(irGet(end)); arguments[1] = toLong(irGet(current)) }
                    arguments[1] = irLong(needed)
                }
            }
            fast.body = irBlock(resultType = irBuiltIns.unitType) {
                val js = (0 until FACTOR).map { u -> irTemporary(if (u == 0) irGet(current) else intPlus(irGet(current), u), nameHint = "jam$u") }
                val kStart = irTemporary(nest.inner.range.arguments[0]!!.deepCopyWithSymbols(parent), nameHint = "jamInnerStart")
                val kEnd = irTemporary(nest.inner.range.arguments[1]!!.deepCopyWithSymbols(parent), nameHint = "jamInnerEnd")
                // The last value of the inner variable, when the inner loop runs at all.
                val innerRuns = irTemporary(
                        if (nest.inner.inclusive)
                            irCall(irBuiltIns.lessOrEqualFunByOperandType.getValue(irBuiltIns.intClass)).apply { arguments[0] = irGet(kStart); arguments[1] = irGet(kEnd) }
                        else lessThan(irGet(kStart), irGet(kEnd)), nameHint = "jamInnerRuns")
                fun breakUnless(condition: IrExpression) { +irIfThen(irBuiltIns.unitType, not(condition), irBreak(fast)) }
                val outerVar = outer.variable.symbol

                // Checks, for each iteration of the group, in the order of the reads (each after those it depends on).
                val allReads = nest.termReads + nest.storeReads
                for (u in 0 until FACTOR) {
                    val values = mapOf<IrValueSymbol, () -> IrExpression>(outerVar to { irGet(js[u]) })
                    for (read in allReads) {
                        val receiver = { read.call.arguments[0]!!.rewritten(values) { null } }
                        fun intGe(a: IrExpression, b: IrExpression) =
                                irCall(irBuiltIns.greaterOrEqualFunByOperandType.getValue(irBuiltIns.intClass)).apply { arguments[0] = a; arguments[1] = b }
                        fun intLe(a: IrExpression, b: IrExpression) =
                                irCall(irBuiltIns.lessOrEqualFunByOperandType.getValue(irBuiltIns.intClass)).apply { arguments[0] = a; arguments[1] = b }
                        if (read.indexedByInner) {
                            // When the inner loop runs, every inner index, start..end (or until end), is within the array.
                            breakUnless(irIfThenElse(irBuiltIns.booleanType, irGet(innerRuns), intGe(irGet(kStart), irInt(0)), irTrue()))
                            breakUnless(irIfThenElse(irBuiltIns.booleanType, irGet(innerRuns),
                                    if (nest.inner.inclusive) lessThan(irGet(kEnd), size(receiver(), read.arrayClass))
                                    else intLe(irGet(kEnd), size(receiver(), read.arrayClass)), irTrue()))
                        } else {
                            val index = irTemporary(read.call.arguments[1]!!.rewritten(values) { null }, nameHint = "jamIndex")
                            breakUnless(intGe(irGet(index), irInt(0)))
                            breakUnless(lessThan(irGet(index), size(receiver(), read.arrayClass)))
                        }
                    }
                    for (store in nest.stores) {
                        val cls = store.arguments[0]!!.type.classOrNull!!
                        val index = irTemporary(store.arguments[1]!!.rewritten(values) { null }, nameHint = "jamStoreIndex")
                        breakUnless(irCall(irBuiltIns.greaterOrEqualFunByOperandType.getValue(irBuiltIns.intClass)).apply {
                            arguments[0] = irGet(index); arguments[1] = irInt(0)
                        })
                        breakUnless(lessThan(irGet(index), size(store.arguments[0]!!.rewritten(values) { null }, cls)))
                    }
                }
                // No array the group stores into is one it reads (of the same class: other arrays cannot be the same).
                for (store in nest.stores) {
                    val storeClass = store.arguments[0]!!.type.classOrNull!!
                    for (read in allReads.filter { it.arrayClass == storeClass }) {
                        for (u in 0 until FACTOR) for (v in 0 until FACTOR) {
                            val readValues = mapOf<IrValueSymbol, () -> IrExpression>(outerVar to { irGet(js[u]) })
                            val storeValues = mapOf<IrValueSymbol, () -> IrExpression>(outerVar to { irGet(js[v]) })
                            breakUnless(not(irEqeqeq(read.call.arguments[0]!!.rewritten(readValues) { null },
                                    store.arguments[0]!!.rewritten(storeValues) { null })))
                        }
                    }
                }

                // The sums, updated together by one inner loop.
                val sums = (0 until FACTOR).map { u ->
                    irTemporary(nest.sum.initializer!!.deepCopyWithSymbols(parent), nameHint = "jamSum$u", isMutable = true,
                            irType = nest.sum.type)
                }
                val innerCopy = nest.inner.block.deepCopyWithSymbols(parent)
                val innerLoop = matchForLoop(innerCopy)!!
                innerLoop.range.arguments[0] = irGet(kStart)
                innerLoop.range.arguments[1] = irGet(kEnd)
                val update = innerLoop.body.single() as IrSetValue
                val term = (update.value as IrCall).arguments[1]!!
                val updates = (0 until FACTOR).map { u ->
                    val values = mapOf<IrValueSymbol, () -> IrExpression>(outerVar to { irGet(js[u]) })
                    irSet(sums[u], irCall(nest.plus).apply {
                        arguments[0] = irGet(sums[u])
                        arguments[1] = term.rewritten(values) { uncheckedAccess(it) }
                    })
                }
                replaceStatement(innerCopy, update, irBlock(resultType = irBuiltIns.unitType) { updates.forEach { +it } })
                +innerCopy

                // The stores, in their order.
                for (u in 0 until FACTOR) {
                    val values = mapOf<IrValueSymbol, () -> IrExpression>(outerVar to { irGet(js[u]) }, nest.sum.symbol to { irGet(sums[u]) })
                    for (store in nest.stores) +store.rewritten(values) { uncheckedAccess(it) }
                }
                +irSet(current, intPlus(irGet(current), FACTOR))
            }
            +fast

            // The rest (and the groups that failed a check): the original loop, from the current iteration.
            outer.range.arguments[0] = irGet(current)
            outer.range.arguments[1] = irGet(end)
            +outer.block
        }
    }

    /** Replaces [old] (a statement of a block nested in [root]) by [new]. */
    private fun replaceStatement(root: org.jetbrains.kotlin.ir.IrElement, old: IrStatement, new: IrStatement) {
        root.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: org.jetbrains.kotlin.ir.IrElement) = element.acceptChildrenVoid(this)
            override fun visitContainerExpression(expression: IrContainerExpression) {
                val i = expression.statements.indexOf(old)
                if (i >= 0) expression.statements[i] = new else expression.acceptChildrenVoid(this)
            }
        })
    }

    private companion object {
        const val FACTOR = 4
    }
}
