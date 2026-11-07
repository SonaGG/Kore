/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.backend.konan.optimizations

import org.jetbrains.kotlin.backend.common.FileLoweringPass
import org.jetbrains.kotlin.backend.common.lower.createIrBuilder
import org.jetbrains.kotlin.backend.konan.Context
import org.jetbrains.kotlin.ir.IrElement
import org.jetbrains.kotlin.ir.IrStatement
import org.jetbrains.kotlin.ir.builders.*
import org.jetbrains.kotlin.ir.declarations.*
import org.jetbrains.kotlin.ir.expressions.*
import org.jetbrains.kotlin.ir.expressions.impl.IrContinueImpl
import org.jetbrains.kotlin.ir.symbols.IrSimpleFunctionSymbol
import org.jetbrains.kotlin.ir.symbols.IrValueSymbol
import org.jetbrains.kotlin.ir.types.IrType
import org.jetbrains.kotlin.ir.util.functions
import org.jetbrains.kotlin.ir.util.getPackageFragment
import org.jetbrains.kotlin.ir.util.parentClassOrNull
import org.jetbrains.kotlin.ir.visitors.IrVisitorVoid
import org.jetbrains.kotlin.ir.visitors.acceptChildrenVoid
import org.jetbrains.kotlin.ir.visitors.acceptVoid
import org.jetbrains.kotlin.name.FqName

/**
 * Kore: sequence pipeline fusion. A `for` loop over a chain of `filter`, `filterNot`, `map` and `take` calls on a
 * sequence (as the inlined bodies of `sumOf`, `forEach`, `count`, `fold` and the like are) becomes one loop over the
 * iterator of the sequence at the start of the chain, which applies the operations to each element in turn:
 * ```
 * for (e in s.take(n).filter(p)) body       // TakeSequence and FilteringSequence iterators, one virtual call per layer
 * ```
 * becomes
 * ```
 * val it = s.iterator(); var left = n
 * while (left > 0 && it.hasNext()) { left--; val e = it.next(); if (!p(e)) continue; body }
 * ```
 * The sequence, the functions and the counts are evaluated where the chain was, in the same order, and the source
 * iterator is created and pulled exactly as the chain's iterators would: an element a filter rejects is followed by the
 * next one, a `take` that is done ends the loop before anything else is pulled (its count does not change for elements
 * that an outer filter rejects, so checking it again before each pull is the same as checking it once).
 */
internal class SequenceLoopFusion(private val context: Context) : FileLoweringPass {
    private val irBuiltIns = context.irBuiltIns
    private val sequencesPackage = FqName("kotlin.sequences")

    private val function1Invoke: IrSimpleFunctionSymbol by lazy {
        irBuiltIns.functionN(1).functions.single { it.name.asString() == "invoke" }.symbol
    }
    private val intMinus: IrSimpleFunctionSymbol by lazy {
        irBuiltIns.intClass.owner.functions.single {
            it.name.asString() == "minus" && it.parameters.size == 2 && it.parameters[1].type == irBuiltIns.intType
        }.symbol
    }

    private enum class Kind { FILTER, FILTER_NOT, MAP, TAKE }

    private class Op(val kind: Kind, val argument: IrExpression, val resultElementType: IrType)

    private class Match(
            val loopBlock: IrBlock,
            val iterator: IrVariable,
            val loop: IrWhileLoop,
            val element: IrVariable,
            val iteratorCall: IrCall,
            val hasNextCall: IrCall,
            val nextCall: IrCall,
            val aliases: List<IrVariable>,
            val base: IrExpression,
            val ops: List<Op>, // From the innermost (applied first).
    )

    override fun lower(irFile: IrFile) {
        irFile.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
            override fun visitFunction(declaration: IrFunction) {
                declaration.acceptChildrenVoid(this)
                declaration.body?.let { fuse(declaration, it) }
            }
        })
    }

    private fun unwrap(expression: IrExpression): IrExpression {
        var e = expression
        while (e is IrTypeOperatorCall && e.operator == IrTypeOperator.IMPLICIT_CAST) e = e.argument
        return e
    }

    private fun op(expression: IrExpression): Pair<Op, IrExpression>? {
        val call = unwrap(expression) as? IrCall ?: return null
        val callee = call.symbol.owner
        if (callee.parentClassOrNull != null || callee.getPackageFragment().packageFqName != sequencesPackage) return null
        if (call.arguments.size != 2 || callee.parameters.size != 2) return null
        if (callee.parameters[0].kind != IrParameterKind.ExtensionReceiver) return null
        val receiver = call.arguments[0] ?: return null
        val argument = call.arguments[1] ?: return null
        val elementType = call.typeArguments.getOrNull(0) ?: return null
        val op = when (callee.name.asString()) {
            "filter" -> Op(Kind.FILTER, argument, elementType)
            "filterNot" -> Op(Kind.FILTER_NOT, argument, elementType)
            "map" -> Op(Kind.MAP, argument, call.typeArguments.getOrNull(1) ?: return null)
            // A positive constant count only: `take(0)` does not iterate its sequence at all, a negative one throws.
            "take" -> {
                val n = (unwrap(argument) as? IrConst)?.value as? Int ?: return null
                if (n <= 0) return null
                Op(Kind.TAKE, argument, elementType)
            }
            else -> return null
        }
        return op to receiver
    }

    private fun isSequenceIterator(call: IrCall): Boolean {
        val callee = call.symbol.owner
        if (callee.name.asString() != "iterator" || call.arguments.size != 1) return false
        val cls = callee.parentClassOrNull ?: return false
        return cls.name.asString() == "Sequence" && cls.getPackageFragment().packageFqName == sequencesPackage
    }

    private fun fuse(function: IrFunction, body: IrBody) {
        // Uses of every value (in nested functions too: a captured value is used there).
        val uses = HashMap<IrValueSymbol, Int>()
        body.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
            override fun visitGetValue(expression: IrGetValue) {
                uses[expression.symbol] = (uses[expression.symbol] ?: 0) + 1
            }
            override fun visitSetValue(expression: IrSetValue) {
                uses[expression.symbol] = (uses[expression.symbol] ?: 0) + 1
                expression.acceptChildrenVoid(this)
            }
        })
        // The statement lists that declare variables (an alias must be declared in one to be replaced).
        val containers = HashMap<IrVariable, IrStatementContainer>()
        body.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) {
                if (element is IrStatementContainer) for (statement in element.statements) if (statement is IrVariable) containers[statement] = element
                element.acceptChildrenVoid(this)
            }
        })
        // The `for` loops of this function (not of nested ones, which are lowered on their own).
        val matches = ArrayList<Match>()
        body.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
            override fun visitFunction(declaration: IrFunction) {}
            override fun visitClass(declaration: IrClass) {}
            override fun visitBlock(expression: IrBlock) {
                expression.acceptChildrenVoid(this)
                if (expression.origin == IrStatementOrigin.FOR_LOOP) match(expression, uses, containers)?.let { matches.add(it) }
            }
        })
        if (matches.isEmpty()) return
        // Aliases are declared before their loop; the declaration holding the chain is replaced by its parts.
        val replacements = HashMap<IrVariable, IrStatement>()
        for (match in matches) rewrite(function, match, replacements)
        for (entry in replacements) {
            val variable = entry.key
            val replacement = entry.value
            val statements = containers.getValue(variable).statements
            statements[statements.indexOf(variable)] = replacement
        }
    }

    private fun match(block: IrBlock, uses: Map<IrValueSymbol, Int>, containers: Map<IrVariable, IrStatementContainer>): Match? {
        if (block.statements.size != 2) return null
        val iterator = block.statements[0] as? IrVariable ?: return null
        val loop = block.statements[1] as? IrWhileLoop ?: return null
        if (iterator.origin != IrDeclarationOrigin.FOR_LOOP_ITERATOR || loop.origin != IrStatementOrigin.FOR_LOOP_INNER_WHILE) return null
        if (uses[iterator.symbol] != 2) return null
        val iteratorCall = iterator.initializer as? IrCall ?: return null
        if (!isSequenceIterator(iteratorCall)) return null
        val hasNextCall = loop.condition as? IrCall ?: return null
        if ((hasNextCall.arguments.singleOrNull() as? IrGetValue)?.symbol != iterator.symbol) return null
        val loopBody = loop.body as? IrContainerExpression ?: return null
        val element = loopBody.statements.firstOrNull() as? IrVariable ?: return null
        if (element.origin != IrDeclarationOrigin.FOR_LOOP_VARIABLE) return null
        val nextCall = element.initializer as? IrCall ?: return null
        if ((nextCall.arguments.singleOrNull() as? IrGetValue)?.symbol != iterator.symbol) return null
        // Through the temporaries of the inliner (each used once, only here) to the chain.
        val aliases = ArrayList<IrVariable>()
        var expression = unwrap(iteratorCall.arguments[0] ?: return null)
        while (expression is IrGetValue) {
            val variable = expression.symbol.owner as? IrVariable ?: break
            if (variable.isVar || uses[variable.symbol] != 1) break
            val initializer = variable.initializer ?: break
            if (variable !in containers) break
            aliases.add(variable)
            expression = unwrap(initializer)
        }
        val ops = ArrayList<Op>()
        while (true) {
            val parsed = op(expression) ?: break
            val op = parsed.first
            val receiver = parsed.second
            ops.add(op)
            expression = receiver
        }
        if (ops.isEmpty()) return null
        // Without an alias, the chain is the receiver of the `iterator()` call: there must be none between.
        if (aliases.isEmpty() && unwrap(iteratorCall.arguments[0]!!) !is IrCall) return null
        ops.reverse()
        return Match(block, iterator, loop, element, iteratorCall, hasNextCall, nextCall, aliases, expression, ops)
    }

    private fun rewrite(function: IrFunction, match: Match, replacements: MutableMap<IrVariable, IrStatement>) {
        val builder = context.createIrBuilder(function.symbol, match.loopBlock.startOffset, match.loopBlock.endOffset)
        // The parts of the chain, where it was evaluated.
        val argumentTemps = ArrayList<IrVariable>()
        lateinit var base: IrVariable
        val parts = builder.irComposite {
            base = irTemporary(match.base, nameHint = "fusedSequence")
            for (op in match.ops) argumentTemps.add(irTemporary(op.argument, nameHint = "fusedArgument"))
        }
        if (match.aliases.isEmpty()) {
            // The chain is the receiver of the `iterator()` call: its parts go first in the loop block.
            match.loopBlock.statements.add(0, parts)
        } else {
            replacements[match.aliases.last()] = parts
            for (alias in match.aliases.dropLast(1)) replacements[alias] = builder.irComposite {}
        }
        // The source iterator and a count per `take`.
        val counts = HashMap<Int, IrVariable>()
        lateinit var sourceIterator: IrVariable
        val setup = builder.irComposite {
            sourceIterator = irTemporary(irCall(match.iteratorCall.symbol, match.iteratorCall.type).apply {
                arguments[0] = irGet(base)
            }, nameHint = "fusedIterator")
            match.ops.forEachIndexed { index, op ->
                if (op.kind == Kind.TAKE) counts[index] = irTemporary(irGet(argumentTemps[index]), nameHint = "fusedLeft", isMutable = true)
            }
        }
        match.loopBlock.statements[match.loopBlock.statements.indexOf(match.iterator)] = setup
        // No longer the shape of a `for` loop (which the for-loop lowerings expect of that origin).
        match.loopBlock.origin = null
        match.loop.origin = IrStatementOrigin.WHILE_LOOP
        // Every `take` has elements left, then the source has one.
        with(builder) {
            var condition: IrExpression = irCall(match.hasNextCall.symbol, match.hasNextCall.type).apply {
                arguments[0] = irGet(sourceIterator)
            }
            for (index in match.ops.indices.reversed()) {
                val count = counts[index] ?: continue
                val positive = irCall(irBuiltIns.greaterFunByOperandType.getValue(irBuiltIns.intClass)).apply {
                    arguments[0] = irGet(count)
                    arguments[1] = irInt(0)
                }
                condition = irIfThenElse(irBuiltIns.booleanType, positive, condition, irFalse())
            }
            match.loop.condition = condition
        }
        // The element: pulled from the source, through the operations.
        match.element.initializer = builder.irBlock(resultType = match.element.type) {
            var current = irTemporary(irCall(match.nextCall.symbol, match.nextCall.type).apply {
                arguments[0] = irGet(sourceIterator)
            }, nameHint = "fusedElement")
            match.ops.forEachIndexed { index, op ->
                fun invoke(type: IrType) = irCall(function1Invoke, type).apply {
                    arguments[0] = irGet(argumentTemps[index])
                    arguments[1] = irGet(current)
                }
                val skip = IrContinueImpl(startOffset, endOffset, irBuiltIns.nothingType, match.loop)
                when (op.kind) {
                    Kind.TAKE -> +irSet(counts[index]!!, irCall(intMinus).apply {
                        arguments[0] = irGet(counts[index]!!)
                        arguments[1] = irInt(1)
                    })
                    Kind.FILTER -> +irIfThenElse(irBuiltIns.unitType, invoke(irBuiltIns.booleanType), irUnit(), skip)
                    Kind.FILTER_NOT -> +irIfThenElse(irBuiltIns.unitType, invoke(irBuiltIns.booleanType), skip, irUnit())
                    Kind.MAP -> current = irTemporary(invoke(op.resultElementType), nameHint = "fusedElement")
                }
            }
            +irImplicitCast(irGet(current), match.element.type)
        }
    }
}
