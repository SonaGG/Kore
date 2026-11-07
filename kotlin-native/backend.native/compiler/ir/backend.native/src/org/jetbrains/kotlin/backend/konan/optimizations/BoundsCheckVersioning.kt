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
import org.jetbrains.kotlin.ir.IrElement
import org.jetbrains.kotlin.ir.builders.*
import org.jetbrains.kotlin.ir.declarations.*
import org.jetbrains.kotlin.ir.expressions.*
import org.jetbrains.kotlin.ir.symbols.IrClassSymbol
import org.jetbrains.kotlin.ir.symbols.IrValueSymbol
import org.jetbrains.kotlin.ir.types.classOrNull
import org.jetbrains.kotlin.ir.types.isMarkedNullable
import org.jetbrains.kotlin.ir.util.deepCopyWithSymbols
import org.jetbrains.kotlin.ir.util.dump
import org.jetbrains.kotlin.ir.util.functions
import org.jetbrains.kotlin.ir.util.parentClassOrNull
import org.jetbrains.kotlin.ir.visitors.IrElementTransformerVoid
import org.jetbrains.kotlin.ir.visitors.IrVisitorVoid
import org.jetbrains.kotlin.ir.visitors.acceptChildrenVoid
import org.jetbrains.kotlin.ir.visitors.acceptVoid
import org.jetbrains.kotlin.ir.visitors.transformChildrenVoid

/**
 * Kore: loop versioning for array bounds checks. A loop `for (i in a until b)` (or `a..b`) whose body reads or writes
 * arrays held in values that do not change (`val`s and parameters) at index `i` runs, when one check before it shows
 * that every such index is within its array (`a >= 0 && b <= x.size && ...`), as a copy without those bounds checks;
 * otherwise it runs as it is. Array lengths never change, so the check decides once for the whole loop.
 *
 * LLVM's IRCE does the same on its own, but not on every loop (after induction variable simplification rewrote some
 * checks into forms it does not recognize), and the bounds checks it leaves keep the vectorizer from the loop.
 */
internal class BoundsCheckVersioning(private val context: Context) : FileLoweringPass {
    private val irBuiltIns = context.irBuiltIns
    private val symbols = context.symbols

    private val arrayClasses: Set<IrClassSymbol> by lazy {
        setOf(irBuiltIns.arrayClass, irBuiltIns.doubleArray, irBuiltIns.floatArray, irBuiltIns.intArray, irBuiltIns.longArray,
                irBuiltIns.shortArray, irBuiltIns.byteArray, irBuiltIns.charArray, irBuiltIns.booleanArray)
    }

    override fun lower(irFile: IrFile) {
        irFile.transformChildrenVoid(object : IrElementTransformerVoidWithContext() {
            override fun visitBlock(expression: IrBlock): IrExpression {
                // Inner loops first: they run the most.
                val result = super.visitBlock(expression)
                if (result !== expression || expression.origin != IrStatementOrigin.FOR_LOOP) return result
                val owner = currentDeclarationParent ?: return result
                val scopeOwner = currentScope?.scope?.scopeOwnerSymbol ?: return result
                return version(expression, scopeOwner, owner) ?: result
            }
        })
    }

    private class Loop(val range: IrCall, val variable: IrVariable, val inclusive: Boolean)

    private fun matchLoop(block: IrBlock): Loop? {
        if (block.statements.size != 2) return null
        val iterator = block.statements[0] as? IrVariable ?: return null
        val loop = block.statements[1] as? IrWhileLoop ?: return null
        if (loop.origin != IrStatementOrigin.FOR_LOOP_INNER_WHILE) return null
        val range = (iterator.initializer as? IrCall)?.arguments?.singleOrNull() as? IrCall ?: return null
        val inclusive = when (range.symbol.owner.name.asString()) {
            "rangeTo" -> true
            "until", "rangeUntil" -> false
            else -> return null
        }
        if (range.arguments.size != 2 || range.arguments.any { it?.type?.classOrNull != irBuiltIns.intClass }) return null
        if (range.type.classOrNull?.owner?.name?.asString() != "IntRange") return null
        val body = loop.body as? IrBlock ?: return null
        val variable = body.statements.firstOrNull() as? IrVariable ?: return null
        if (variable.origin != IrDeclarationOrigin.FOR_LOOP_VARIABLE) return null
        return Loop(range, variable, inclusive)
    }

    /** An array access whose index is the loop variable plus or minus an invariant offset. */
    private class Access(val array: IrValueSymbol, val cls: IrClassSymbol, val offset: IrExpression?, val negative: Boolean) {
        val key: String = array.toString() + (if (offset == null) "" else (if (negative) "-" else "+") + offset.dump())
    }

    /** Constants and unchanging values outside the loop, with Int arithmetic. */
    private fun isInvariant(expression: IrExpression, loopVariable: IrValueSymbol, declaredInLoop: Set<IrValueSymbol>): Boolean =
            when (expression) {
                is IrConst -> expression.type.classOrNull == irBuiltIns.intClass
                is IrGetValue -> expression.symbol != loopVariable && expression.symbol !in declaredInLoop &&
                        (expression.symbol.owner as? IrVariable)?.isVar != true && expression.type.classOrNull == irBuiltIns.intClass
                is IrCall -> expression.symbol.owner.parentClassOrNull?.symbol == irBuiltIns.intClass &&
                        expression.symbol.owner.name.asString() in setOf("plus", "minus", "times") &&
                        expression.arguments.size == 2 &&
                        expression.arguments.all { it != null && isInvariant(it, loopVariable, declaredInLoop) }
                else -> false
            }

    /** An access `x[i + c]` (or `x[i]`, `x[c + i]`, `x[i - c]`, reads or writes), `c` invariant, of an array `x` in an unchanging value. */
    private fun access(call: IrCall, loopVariable: IrValueSymbol, declaredInLoop: Set<IrValueSymbol>): Access? {
        val cls = call.symbol.owner.parentClassOrNull?.symbol ?: return null
        if (cls !in arrayClasses) return null
        if (call.symbol != symbols.arrayGet[cls] && call.symbol != symbols.arraySet[cls]) return null
        val receiver = call.arguments[0] as? IrGetValue ?: return null
        if (receiver.type.isMarkedNullable() || receiver.symbol in declaredInLoop) return null
        if ((receiver.symbol.owner as? IrVariable)?.isVar == true) return null
        fun isLoopVariable(e: IrExpression?) = (e as? IrGetValue)?.symbol == loopVariable
        val index = call.arguments.getOrNull(1) ?: return null
        if (isLoopVariable(index)) return Access(receiver.symbol, cls, null, false)
        val arithmetic = index as? IrCall ?: return null
        if (arithmetic.symbol.owner.parentClassOrNull?.symbol != irBuiltIns.intClass || arithmetic.arguments.size != 2) return null
        val a = arithmetic.arguments[0] ?: return null
        val b = arithmetic.arguments[1] ?: return null
        return when (arithmetic.symbol.owner.name.asString()) {
            "plus" -> when {
                isLoopVariable(a) && isInvariant(b, loopVariable, declaredInLoop) -> Access(receiver.symbol, cls, b, false)
                isLoopVariable(b) && isInvariant(a, loopVariable, declaredInLoop) -> Access(receiver.symbol, cls, a, false)
                else -> null
            }
            "minus" -> if (isLoopVariable(a) && isInvariant(b, loopVariable, declaredInLoop)) Access(receiver.symbol, cls, b, true) else null
            else -> null
        }
    }

    private val intToLong by lazy { irBuiltIns.intClass.owner.functions.single { it.name.asString() == "toLong" && it.parameters.size == 1 }.symbol }
    private fun longOp(name: String) = irBuiltIns.longClass.owner.functions.single {
        it.name.asString() == name && it.parameters.size == 2 && it.parameters[1].type.classOrNull == irBuiltIns.longClass
    }.symbol

    private fun version(block: IrBlock, scopeOwner: org.jetbrains.kotlin.ir.symbols.IrSymbol, parent: IrDeclarationParent): IrExpression? {
        val loop = matchLoop(block) ?: return null
        // Values declared in the loop change between iterations.
        val declaredInLoop = HashSet<IrValueSymbol>()
        var hasLocalFunctions = false
        var nestedLoops = 0
        block.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
            override fun visitFunction(declaration: IrFunction) { hasLocalFunctions = true }
            override fun visitLoop(loop: IrLoop) { nestedLoops++; loop.acceptChildrenVoid(this) }
            override fun visitVariable(declaration: IrVariable) { declaredInLoop.add(declaration.symbol); declaration.acceptChildrenVoid(this) }
        })
        // Innermost loops only (the loop itself counts once): versioning outer loops would copy their inner loops too.
        if (hasLocalFunctions || nestedLoops > 1) return null
        val accesses = LinkedHashMap<String, Access>()
        block.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
            override fun visitCall(expression: IrCall) {
                expression.acceptChildrenVoid(this)
                access(expression, loop.variable.symbol, declaredInLoop)?.let { accesses.putIfAbsent(it.key, it) }
            }
        })
        if (accesses.isEmpty()) return null

        val builder = context.createIrBuilder(scopeOwner, block.startOffset, block.endOffset)
        val longPlus = longOp("plus")
        val longMinus = longOp("minus")
        return builder.irBlock(resultType = irBuiltIns.unitType) {
            val start = irTemporary(loop.range.arguments[0]!!, nameHint = "versionStart")
            val end = irTemporary(loop.range.arguments[1]!!, nameHint = "versionEnd")
            fun toLong(e: IrExpression) = irCall(intToLong).apply { arguments[0] = e }
            fun shifted(value: IrExpression, access: Access): IrExpression = access.offset?.let { offset ->
                irCall(if (access.negative) longMinus else longPlus).apply {
                    arguments[0] = value
                    arguments[1] = toLong(offset.deepCopyWithSymbols(parent))
                }
            } ?: value
            fun and(a: IrExpression, b: IrExpression) = irIfThenElse(irBuiltIns.booleanType, a, b, irFalse())
            // For every access: start + c >= 0 and end + c < size (inclusive) or <= size (exclusive), in Longs.
            var condition: IrExpression? = null
            for (access in accesses.values) {
                val size = toLong(irCall(symbols.arraySize[access.cls]!!).apply { arguments[0] = irGet(access.array.owner) })
                val low = irCall(irBuiltIns.greaterOrEqualFunByOperandType.getValue(irBuiltIns.longClass)).apply {
                    arguments[0] = shifted(toLong(irGet(start)), access)
                    arguments[1] = irLong(0)
                }
                val high = irCall((if (loop.inclusive) irBuiltIns.lessFunByOperandType else irBuiltIns.lessOrEqualFunByOperandType)
                        .getValue(irBuiltIns.longClass)).apply {
                    arguments[0] = shifted(toLong(irGet(end)), access)
                    arguments[1] = size
                }
                val fits = and(low, high)
                condition = condition?.let { and(it, fits) } ?: fits
            }
            val fast = block.deepCopyWithSymbols(parent)
            val fastLoop = matchLoop(fast)!!
            fastLoop.range.arguments[0] = irGet(start)
            fastLoop.range.arguments[1] = irGet(end)
            val fastDeclared = HashSet<IrValueSymbol>()
            fast.acceptVoid(object : IrVisitorVoid() {
                override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
                override fun visitVariable(declaration: IrVariable) { fastDeclared.add(declaration.symbol); declaration.acceptChildrenVoid(this) }
            })
            fast.transformChildrenVoid(object : IrElementTransformerVoid() {
                override fun visitCall(expression: IrCall): IrExpression {
                    expression.transformChildrenVoid(this)
                    val access = access(expression, fastLoop.variable.symbol, fastDeclared) ?: return expression
                    if (access.key !in accesses) return expression
                    val cls = access.cls
                    val unchecked = cls.owner.functions.single {
                        it.name == (if (expression.symbol == symbols.arrayGet[cls]) KonanNameConventions.getWithoutBoundCheck
                        else KonanNameConventions.setWithoutBoundCheck)
                    }.symbol
                    return irCall(unchecked, expression.type).also { copy ->
                        expression.arguments.forEachIndexed { i, argument -> copy.arguments[i] = argument }
                    }
                }
            })
            loop.range.arguments[0] = irGet(start)
            loop.range.arguments[1] = irGet(end)
            +irIfThenElse(irBuiltIns.unitType, condition!!, fast, block)
        }
    }
}
