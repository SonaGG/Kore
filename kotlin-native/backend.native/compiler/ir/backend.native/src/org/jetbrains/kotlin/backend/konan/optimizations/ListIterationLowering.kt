/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.backend.konan.optimizations

import org.jetbrains.kotlin.backend.common.lower.createIrBuilder
import org.jetbrains.kotlin.backend.konan.Context
import org.jetbrains.kotlin.ir.IrElement
import org.jetbrains.kotlin.ir.IrStatement
import org.jetbrains.kotlin.ir.builders.*
import org.jetbrains.kotlin.ir.declarations.*
import org.jetbrains.kotlin.ir.expressions.*
import org.jetbrains.kotlin.ir.util.*
import org.jetbrains.kotlin.ir.visitors.IrVisitorVoid
import org.jetbrains.kotlin.ir.visitors.acceptChildrenVoid
import org.jetbrains.kotlin.ir.visitors.acceptVoid

/**
 * Kore: turns `for (x in list)` loops over lists whose iterator is `AbstractList.IteratorImpl` (e.g. `Enum.entries`,
 * `asList()` views, subLists) into index loops, like a JIT's scalar replacement of the iterator.
 *
 * Once devirtualized, such a loop calls `IteratorImpl.hasNext()` (`index < size`) and `IteratorImpl.next()` (`if
 * (!hasNext()) throw NoSuchElementException(); return get(index++)`). Even inlined, the iterator object keeps its
 * `index` in memory: as a stack object with a reference field, it is registered as a GC root. This replaces the calls
 * with the same `size` and `get` calls on the iterator's list, with the index in a local variable, which leaves the
 * iterator object unused. It only applies to iterator variables used for nothing else, so the iterator's final state
 * is unobservable.
 *
 * Runs after the first devirtualization round (which resolves the iterator calls) and before the second one (which
 * resolves the new `size` and `get` calls).
 */
internal class ListIterationLowering(private val context: Context, private val module: IrModuleFragment) {
    fun run() {
        module.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)

            override fun visitFunction(declaration: IrFunction) {
                declaration.acceptChildrenVoid(this)
                (declaration as? IrSimpleFunction ?: declaration as? IrConstructor)?.let { lowerFunction(it) }
            }
        })
    }

    private fun lowerFunction(function: IrFunction) {
        val body = function.body ?: return
        // Uses of each variable: the loops whose iterator calls are the only ones are lowered.
        val uses = mutableMapOf<IrVariable, Int>()
        body.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
            override fun visitGetValue(expression: IrGetValue) {
                (expression.symbol.owner as? IrVariable)?.let { uses[it] = (uses[it] ?: 0) + 1 }
            }
        })
        body.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)

            override fun visitBlockBody(body: IrBlockBody) {
                body.acceptChildrenVoid(this)
                lowerStatements(function, body.statements, uses)
            }

            override fun visitContainerExpression(expression: IrContainerExpression) {
                expression.acceptChildrenVoid(this)
                lowerStatements(function, expression.statements, uses)
            }
        })
    }

    private fun lowerStatements(function: IrFunction, statements: MutableList<IrStatement>, uses: Map<IrVariable, Int>) {
        var index = 0
        while (index < statements.size - 1) {
            val iterator = statements[index] as? IrVariable
            val loop = statements[index + 1] as? IrWhileLoop
            if (iterator != null && loop != null && iterator.origin == IrDeclarationOrigin.FOR_LOOP_ITERATOR) {
                val replacement = lowerLoop(function, iterator, loop, uses[iterator] ?: 0)
                if (replacement != null) {
                    val [before, after] = replacement
                    statements.addAll(index + 1, after)
                    statements.addAll(index, before)
                    index += before.size + after.size
                }
            }
            ++index
        }
    }

    // [creation]: the `AbstractList.iterator()` call that created the iterator, if known.
    private class IteratorCalls(val hasNext: IrCall, val next: IrCall, val iteratorImpl: IrClass, val creation: IrCall?)

    // The loop's `hasNext()` and `next()` calls, if they are `AbstractList.IteratorImpl`'s on [iterator].
    private fun iteratorCalls(iterator: IrVariable, loop: IrWhileLoop): IteratorCalls? {
        val hasNext = (loop.condition as? IrCall ?: (loop.condition as? IrBlock)?.statements?.singleOrNull() as? IrCall)
                ?: return null
        val created = createdIteratorImpl(iterator)
        val iteratorImpl = iteratorImplOf(hasNext, "hasNext", iterator, created) ?: return null
        val body = loop.body as? IrContainerExpression ?: return null
        val variable = body.statements.firstOrNull() as? IrVariable ?: return null
        if (variable.origin != IrDeclarationOrigin.FOR_LOOP_VARIABLE) return null
        var next: IrCall? = null
        variable.initializer?.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
            override fun visitCall(expression: IrCall) {
                if (iteratorImplOf(expression, "next", iterator, created) == iteratorImpl) next = expression
                else expression.acceptChildrenVoid(this)
            }
        })
        return IteratorCalls(hasNext, next ?: return null, iteratorImpl, creatingCall(iterator))
    }

    // `AbstractList.IteratorImpl` if [iterator] is initialized by a (devirtualized) call to `AbstractList.iterator()`, which
    // creates exactly an `IteratorImpl`: calls on it can only reach `IteratorImpl`'s implementations.
    private fun createdIteratorImpl(iterator: IrVariable): IrClass? {
        val call = creatingCall(iterator) ?: return null
        return (call.symbol.owner.parent as IrClass).declarations.filterIsInstance<IrClass>().singleOrNull { it.name.asString() == "IteratorImpl" }
    }

    private fun creatingCall(iterator: IrVariable): IrCall? {
        var initializer = iterator.initializer
        while (true) {
            initializer = when (initializer) {
                is IrTypeOperatorCall -> if (initializer.operator == IrTypeOperator.IMPLICIT_CAST) initializer.argument else return null
                is IrBlock -> initializer.statements.singleOrNull() as? IrExpression ?: return null
                else -> break
            }
        }
        val call = initializer as? IrCall ?: return null
        val callee = call.symbol.owner
        if (call.superQualifierSymbol == null || callee.name.asString() != "iterator" || callee.isFakeOverride) return null
        val abstractList = callee.parent as? IrClass ?: return null
        if (abstractList.fqNameWhenAvailable?.asString() != "kotlin.collections.AbstractList") return null
        return call
    }

    private fun iteratorImplOf(call: IrCall, name: String, iterator: IrVariable, created: IrClass?): IrClass? {
        val callee = call.symbol.owner
        if (callee.name.asString() != name) return null
        if ((call.arguments.getOrNull(0) as? IrGetValue)?.symbol != iterator.symbol) return null
        if (call.superQualifierSymbol == null) return created
        val iteratorImpl = callee.parent as? IrClass ?: return null
        if (iteratorImpl.name.asString() != "IteratorImpl" || callee.isFakeOverride) return null
        val outer = iteratorImpl.parent as? IrClass ?: return null
        if (outer.fqNameWhenAvailable?.asString() != "kotlin.collections.AbstractList") return null
        return iteratorImpl
    }

    // The statements to insert before the iterator variable and between it and the loop.
    private fun lowerLoop(function: IrFunction, iterator: IrVariable, loop: IrWhileLoop, iteratorUses: Int): Pair<List<IrStatement>, List<IrStatement>>? {
        val calls = iteratorCalls(iterator, loop) ?: return null
        // The iterator must be used by these two calls only.
        if (iteratorUses != 2) return null
        val iteratorImpl = calls.iteratorImpl
        // The symbols `IteratorImpl` itself uses: its `index` field, its list's `size` getter and `get` function.
        val hasNextFunction = iteratorImpl.functions.singleOrNull { it.name.asString() == "hasNext" && it.parameters.size == 1 } ?: return null
        val nextFunction = iteratorImpl.functions.singleOrNull { it.name.asString() == "next" && it.parameters.size == 1 } ?: return null
        val outerField = iteratorImpl.fields.singleOrNull { it.origin == IrDeclarationOrigin.FIELD_FOR_OUTER_THIS } ?: return null
        val indexField = fieldsReadIn(hasNextFunction).singleOrNull { it.name.asString() == "index" } ?: return null
        // The calls may already be devirtualized (into a type switch): call the function they override, virtually.
        val sizeGetter = callsIn(hasNextFunction).firstOrNull { it.name.asString() == "<get-size>" }?.let { rootOf(it) } ?: return null
        val get = callsIn(nextFunction).firstOrNull { it.name.asString() == "get" && it.parameters.size == 2 }?.let { rootOf(it) } ?: return null
        val noElement = nextFunction.body?.let { findThrow(it) } ?: return null

        val builder = context.createIrBuilder(function.symbol, loop.startOffset, loop.endOffset)
        // With the creating call, the list is its receiver (whose type the devirtualization analysis knows precisely), and
        // the new iterator's index 0; otherwise both are read from the iterator.
        val creation = calls.creation
        val list = builder.run {
            scope.createTemporaryVariable(
                    creation?.arguments?.get(0) ?: irGetField(irGet(iterator), outerField), nameHint = "list")
        }
        creation?.arguments?.set(0, builder.irGet(list))
        val position = builder.run {
            scope.createTemporaryVariable(
                    if (creation != null) irInt(0) else irGetField(irGet(iterator), indexField), nameHint = "index", isMutable = true)
        }
        fun IrBuilderWithScope.hasNext(): IrExpression = irCall(context.irBuiltIns.lessFunByOperandType[context.irBuiltIns.intClass]!!).apply {
            arguments[0] = irGet(position)
            arguments[1] = irCall(sizeGetter).apply { arguments[0] = irGet(list) }
        }
        loop.condition = builder.hasNext()
        val next = builder.irBlock(resultType = calls.next.type) {
            // `next()` checks `hasNext()` again.
            +irIfThen(context.irBuiltIns.unitType, irCall(context.irBuiltIns.booleanNotSymbol).apply { arguments[0] = hasNext() },
                    noElement.deepCopyWithSymbols(function))
            val current = irTemporary(irGet(position), nameHint = "current")
            +irSet(position, irCall(context.irBuiltIns.intClass.owner.functions.single {
                it.name.asString() == "plus" && it.parameters.getOrNull(1)?.type == context.irBuiltIns.intType
            }).apply {
                arguments[0] = irGet(current)
                arguments[1] = irInt(1)
            })
            +irCall(get).apply {
                arguments[0] = irGet(list)
                arguments[1] = irGet(current)
            }
        }
        replaceCall(loop.body!!, calls.next, next)
        return if (creation != null) listOf(list) to listOf(position) else emptyList<IrStatement>() to listOf(list, position)
    }

    // The declaration in `kotlin.collections.List` or `Collection` that [function] overrides.
    private fun rootOf(function: IrSimpleFunction): IrSimpleFunction? {
        val fqName = function.parentClassOrNull?.fqNameWhenAvailable?.asString()
        if (fqName == "kotlin.collections.List" || fqName == "kotlin.collections.Collection") return function
        return function.overriddenSymbols.firstNotNullOfOrNull { rootOf(it.owner) }
    }

    private fun callsIn(function: IrFunction): Set<IrSimpleFunction> = buildSet {
        function.body?.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
            override fun visitCall(expression: IrCall) {
                add(expression.symbol.owner)
                expression.acceptChildrenVoid(this)
            }
        })
    }

    private fun fieldsReadIn(function: IrFunction): Set<IrField> = buildSet {
        function.body?.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
            override fun visitGetField(expression: IrGetField) {
                add(expression.symbol.owner)
                expression.acceptChildrenVoid(this)
            }
        })
    }

    private fun findThrow(body: IrBody): IrThrow? {
        var result: IrThrow? = null
        body.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) {
                if (result == null) element.acceptChildrenVoid(this)
            }
            override fun visitThrow(expression: IrThrow) {
                if (result == null) result = expression
            }
        })
        return result
    }

    private fun replaceCall(root: IrElement, call: IrCall, replacement: IrExpression) {
        root.transformChildren(object : org.jetbrains.kotlin.ir.visitors.IrTransformer<Nothing?>() {
            override fun visitCall(expression: IrCall, data: Nothing?): IrElement =
                    if (expression === call) replacement else super.visitCall(expression, data)
        }, null)
    }
}
