/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.backend.konan.optimizations

import org.jetbrains.kotlin.backend.common.lower.createIrBuilder
import org.jetbrains.kotlin.backend.konan.Context
import org.jetbrains.kotlin.backend.konan.ir.isBox
import org.jetbrains.kotlin.backend.konan.ir.isUnbox
import org.jetbrains.kotlin.ir.IrElement
import org.jetbrains.kotlin.ir.builders.*
import org.jetbrains.kotlin.ir.declarations.*
import org.jetbrains.kotlin.ir.expressions.*
import org.jetbrains.kotlin.ir.expressions.impl.IrBlockImpl
import org.jetbrains.kotlin.ir.expressions.impl.IrConstImpl
import org.jetbrains.kotlin.ir.symbols.IrReturnTargetSymbol
import org.jetbrains.kotlin.ir.types.*
import org.jetbrains.kotlin.ir.util.*
import org.jetbrains.kotlin.ir.visitors.IrVisitorVoid
import org.jetbrains.kotlin.ir.visitors.acceptChildrenVoid
import org.jetbrains.kotlin.ir.visitors.acceptVoid
import java.util.IdentityHashMap

/**
 * Kore: fuses collection pipelines such as `(0 until n).map { f(it) }.filter { g(it) }.sumOf { h(it) }`.
 *
 * After inlining, such a chain is a sequence of loops: each one fills a fresh `ArrayList` that the next one iterates. When
 * such a list is only filled by one producing region and then iterated once by a consuming loop, the consumer's body is
 * moved to where the producer adds each element, and the list is never built.
 *
 * Kotlin's eager semantics run the whole producer (e.g. all `map` lambdas) before the consumer (all `filter` lambdas), and
 * fusing interleaves them. So one of the two sides must be unobservable: it calls only known pure, non-throwing operations
 * (arithmetic, comparisons, boxing), writes no heap memory, writes only local variables the other side does not touch,
 * cannot throw, always terminates and does not jump out. Then the order of its steps relative to the other side cannot be
 * told apart. The code that runs between the two loops must be pure too. In any other case, nothing is changed.
 *
 * Afterwards, primitive values that were boxed only to be added and unboxed again are no longer boxed.
 *
 * Runs on the whole program (closed world) before the DFG is built, so the analyses see the simplified loops.
 */
internal class CollectionFusion(private val context: Context, private val module: IrModuleFragment) {
    private val log = System.getProperty("kore.collectionFusion.log") != null || System.getenv("KORE_FUSION_LOG") != null
    private val dump = System.getenv("KORE_FUSION_DUMP")
    private var currentFunction: IrFunction? = null

    fun run() {
        module.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
            override fun visitFunction(declaration: IrFunction) {
                declaration.acceptChildrenVoid(this)
                lowerFunction(declaration)
            }
        })
    }

    private fun lowerFunction(function: IrFunction) {
        val body = function.body ?: return
        if (!hasListCreation(body)) return
        currentFunction = function
        var fused = 0
        while (fused < 32) {
            val state = FunctionState(body)
            if (state.creations.none { tryFuse(function, state, it) }) break
            ++fused
        }
        if (fused > 0) foldBoxes(function, body)
        if (dump != null && function.name.asString() == dump) println(function.dump())
    }

    private fun reject(reason: String): Boolean {
        if (log) println("CollectionFusion: ${currentFunction?.fqNameWhenAvailable}: not fused: $reason")
        return false
    }

    // ---------------------------------------------------------------------------------------------------------------------
    // The function's tree.

    private class FunctionState(body: IrBody) {
        val parents = IdentityHashMap<IrElement, IrElement>()
        val order = IdentityHashMap<IrElement, Int>()
        val gets = HashMap<IrValueDeclaration, MutableList<IrGetValue>>()
        val returns = HashMap<IrReturnTargetSymbol, MutableList<IrReturn>>()
        val creations = ArrayList<IrVariable>()

        init {
            body.acceptVoid(object : IrVisitorVoid() {
                val stack = ArrayList<IrElement>()
                override fun visitElement(element: IrElement) {
                    stack.lastOrNull()?.let { parents[element] = it }
                    order[element] = order.size
                    when (element) {
                        is IrGetValue -> gets.getOrPut(element.symbol.owner) { mutableListOf() }.add(element)
                        is IrReturn -> returns.getOrPut(element.returnTargetSymbol) { mutableListOf() }.add(element)
                        is IrVariable -> if (isListCreation(element)) creations.add(element)
                    }
                    stack.add(element)
                    element.acceptChildrenVoid(this)
                    stack.removeAt(stack.size - 1)
                }
            })
        }

        // The ancestors of [element], from the root down to [element].
        fun path(element: IrElement): List<IrElement> {
            val result = ArrayList<IrElement>()
            var current: IrElement? = element
            while (current != null) {
                result.add(current)
                current = parents[current]
            }
            result.reverse()
            return result
        }

        fun lowestCommonAncestor(elements: List<IrElement>): IrElement? {
            var common: List<IrElement> = path(elements.first())
            for (element in elements.drop(1)) {
                val other = path(element)
                var n = 0
                while (n < common.size && n < other.size && common[n] === other[n]) ++n
                common = common.subList(0, n)
            }
            return common.lastOrNull()
        }

        // The child of [ancestor] on the path to [element].
        fun childOn(ancestor: IrElement, element: IrElement): IrElement? {
            var current = element
            while (true) {
                val parent = parents[current] ?: return null
                if (parent === ancestor) return current
                current = parent
            }
        }

        fun isAncestor(ancestor: IrElement, element: IrElement): Boolean {
            var current: IrElement? = element
            while (current != null) {
                if (current === ancestor) return true
                current = parents[current]
            }
            return false
        }
    }

    private fun hasListCreation(body: IrBody): Boolean {
        var found = false
        body.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) {
                if (!found) element.acceptChildrenVoid(this)
            }

            override fun visitVariable(declaration: IrVariable) {
                if (isListCreation(declaration)) found = true else super.visitVariable(declaration)
            }
        })
        return found
    }

    // ---------------------------------------------------------------------------------------------------------------------
    // The uses of a list.

    private class ListUses(val creation: IrVariable) {
        // The variables holding the list, and the returnable blocks returning it.
        val vars = LinkedHashSet<IrVariable>()
        val returnableBlocks = HashSet<IrReturnableBlock>()
        var init: IrCall? = null
        val adds = ArrayList<IrCall>()
        val iterators = ArrayList<IrVariable>()

        // `collectionSizeOrDefault(list, default)` calls passed as the capacity of another list.
        val sizeHints = ArrayList<IrCall>()
    }

    private fun analyzeUses(state: FunctionState, creation: IrVariable): ListUses? {
        if (creation.isVar) return null
        val uses = ListUses(creation)
        val worklist = ArrayDeque<IrVariable>()
        uses.vars.add(creation)
        worklist.add(creation)
        while (worklist.isNotEmpty()) {
            val variable = worklist.removeFirst()
            for (get in state.gets[variable].orEmpty()) {
                if (!classifyUse(state, uses, get, worklist)) {
                    reject("list ${creation.name} escapes in ${state.parents[get]?.let { it::class.simpleName }}")
                    return null
                }
            }
        }
        // A returnable block holds the list only if all its returns return the list.
        for (block in uses.returnableBlocks) {
            if (state.returns[block.symbol].orEmpty().any { !isListValue(uses, it.value) }) return null
        }
        if (uses.init == null) return null
        return uses
    }

    private fun isListValue(uses: ListUses, expression: IrExpression): Boolean = when (expression) {
        is IrGetValue -> expression.symbol.owner in uses.vars
        is IrTypeOperatorCall -> expression.operator == IrTypeOperator.IMPLICIT_CAST && isListValue(uses, expression.argument)
        is IrReturnableBlock -> expression in uses.returnableBlocks
        is IrContainerExpression -> (expression.statements.lastOrNull() as? IrExpression)?.let { isListValue(uses, it) } == true
        else -> false
    }

    // Follows the value of [get] to where it is used. False if it may escape or is used in an unknown way.
    private fun classifyUse(state: FunctionState, uses: ListUses, get: IrGetValue, worklist: ArrayDeque<IrVariable>): Boolean {
        var current: IrElement = get
        while (true) {
            val parent = state.parents[current] ?: return false
            when {
                parent is IrTypeOperatorCall && parent.operator == IrTypeOperator.IMPLICIT_CAST -> current = parent
                parent is IrTypeOperatorCall && parent.operator == IrTypeOperator.IMPLICIT_COERCION_TO_UNIT -> return true
                parent is IrReturnableBlock || parent is IrInlinedFunctionBlock -> return parent.statements.lastOrNull() !== current
                parent is IrContainerExpression -> {
                    if (parent.statements.lastOrNull() !== current || parent.type.isUnit()) return true
                    current = parent
                }
                parent is IrReturn && parent.value === current -> {
                    val target = parent.returnTargetSymbol.owner as? IrReturnableBlock ?: return false
                    uses.returnableBlocks.add(target)
                    current = target
                }
                parent is IrVariable && parent.initializer === current -> {
                    if (parent.isVar) return false
                    if (uses.vars.add(parent)) worklist.add(parent)
                    return true
                }
                parent is IrCall -> return classifyCall(state, uses, parent, current)
                else -> return false
            }
        }
    }

    private fun isCollectionsMember(function: IrSimpleFunction) =
            function.dispatchReceiverParameter != null && function.parentClassOrNull?.packageFqName?.asString() == "kotlin.collections"

    private fun classifyCall(state: FunctionState, uses: ListUses, call: IrCall, argument: IrElement): Boolean {
        val callee = call.symbol.owner
        val name = callee.name.asString()
        if (call.superQualifierSymbol != null) return false
        if (call.arguments.getOrNull(0) !== argument) {
            // The list's size as the capacity of another list.
            if (name == "collectionSizeOrDefault" && callee.fqNameWhenAvailable?.asString() == "kotlin.collections.collectionSizeOrDefault"
                    && call.arguments.getOrNull(1) is IrConst) {
                val parent = state.parents[call] as? IrCall ?: return false
                if (!isArrayListInit(parent.symbol.owner) || parent.arguments.getOrNull(1) !== call) return false
                uses.sizeHints.add(call)
                return true
            }
            return false
        }
        when {
            name == "<init>" -> {
                if (!isArrayListInit(callee) || uses.init != null) return false
                if ((argument as? IrGetValue)?.symbol?.owner !== uses.creation) return false
                if (callee.parameters.size !in 1..2) return false
                if (callee.parameters.size == 2 && !callee.parameters[1].type.isInt()) return false
                uses.init = call
                return true
            }
            name == "add" && callee.parameters.size == 2 && isCollectionsMember(callee) -> {
                // Only as a statement: the result (always true) is not used.
                val parent = state.parents[call]
                val discarded = (parent is IrTypeOperatorCall && parent.operator == IrTypeOperator.IMPLICIT_COERCION_TO_UNIT)
                        || (parent is IrContainerExpression && parent !is IrReturnableBlock && parent.statements.lastOrNull() !== call)
                if (!discarded) return false
                uses.adds.add(call)
                return true
            }
            name == "iterator" && callee.parameters.size == 1 && isCollectionsMember(callee) -> {
                var parent = state.parents[call]
                while (parent is IrTypeOperatorCall && parent.operator == IrTypeOperator.IMPLICIT_CAST) parent = state.parents[parent]
                if (parent !is IrVariable || parent.origin != IrDeclarationOrigin.FOR_LOOP_ITERATOR) return false
                uses.iterators.add(parent)
                return true
            }
            else -> return false
        }
    }

    private fun isArrayListInit(function: IrSimpleFunction) =
            function.name.asString() == "<init>" && function.parentClassOrNull?.fqNameWhenAvailable?.asString() == "kotlin.collections.ArrayList"

    private class IterationLoop(val container: IrContainerExpression, val iterator: IrVariable, val loop: IrWhileLoop,
                                val body: IrContainerExpression, val element: IrVariable)

    // `val it = list.iterator(); while (it.hasNext()) { val element = it.next(); ... }`, with no other use of `it`.
    private fun iterationLoop(state: FunctionState, iterator: IrVariable): IterationLoop? {
        val container = state.parents[iterator] as? IrContainerExpression ?: return null
        val index = container.statements.indexOf(iterator)
        val loop = container.statements.getOrNull(index + 1) as? IrWhileLoop ?: return null
        if (state.gets[iterator]?.size != 2) return null
        val hasNext = loop.condition as? IrCall ?: return null
        if (hasNext.symbol.owner.name.asString() != "hasNext" || (hasNext.arguments.getOrNull(0) as? IrGetValue)?.symbol != iterator.symbol) return null
        val body = loop.body as? IrContainerExpression ?: return null
        val element = body.statements.firstOrNull() as? IrVariable ?: return null
        if (element.origin != IrDeclarationOrigin.FOR_LOOP_VARIABLE) return null
        var next = element.initializer
        while (next is IrTypeOperatorCall && next.operator == IrTypeOperator.IMPLICIT_CAST) next = next.argument
        if (next !is IrCall || next.symbol.owner.name.asString() != "next" || (next.arguments.getOrNull(0) as? IrGetValue)?.symbol != iterator.symbol) return null
        return IterationLoop(container, iterator, loop, body, element)
    }

    // ---------------------------------------------------------------------------------------------------------------------
    // Effects.

    private class Effects {
        var heapWrite = false   // or any unknown operation
        var heapRead = false
        var throws = false
        var mayNotTerminate = false
        var hasTry = false
        val reads = HashSet<IrValueDeclaration>()
        val writes = HashSet<IrValueDeclaration>()
        val declared = HashSet<IrVariable>()
        val outward = ArrayList<IrElement>() // targets of returns, breaks and continues outside the analyzed code

        val isPure get() = !heapWrite && !throws && !mayNotTerminate && outward.isEmpty()
    }

    private enum class CallKind { PURE, THROWS, UNKNOWN }

    private val primitiveClasses = setOf("kotlin.Int", "kotlin.Long", "kotlin.Short", "kotlin.Byte", "kotlin.Char", "kotlin.Boolean",
            "kotlin.Float", "kotlin.Double")
    private val pureOperators = setOf("plus", "minus", "times", "unaryMinus", "unaryPlus", "inc", "dec", "and", "or", "xor", "shl",
            "shr", "ushr", "inv", "not", "compareTo", "equals", "toByte", "toShort", "toInt", "toLong", "toFloat", "toDouble", "toChar")
    private val divisionOperators = setOf("div", "rem", "mod", "floorDiv")
    private val pureIntrinsics = setOf("kotlin.internal.ir.less", "kotlin.internal.ir.lessOrEqual", "kotlin.internal.ir.greater",
            "kotlin.internal.ir.greaterOrEqual", "kotlin.internal.ir.EQEQ", "kotlin.internal.ir.EQEQEQ", "kotlin.internal.ir.ieee754equals",
            "kotlin.native.internal.reinterpret", "kotlin.native.internal.areEqualByValue",
            "kotlin.native.internal.createUninitializedInstance")

    private fun callKind(call: IrCall, constants: Set<IrCall>): CallKind {
        val callee = call.symbol.owner
        if (call.superQualifierSymbol != null) return CallKind.UNKNOWN
        if (callee.isBox() || callee.isUnbox()) return CallKind.PURE
        val name = callee.name.asString()
        val parentClass = callee.parent as? IrClass
        if (parentClass != null) {
            val className = parentClass.fqNameWhenAvailable?.asString()
            if (className in primitiveClasses && callee.dispatchReceiverParameter != null) {
                if (name in pureOperators) return CallKind.PURE
                if (name in divisionOperators) {
                    if (className == "kotlin.Float" || className == "kotlin.Double") return CallKind.PURE
                    val divisor = (call.arguments.getOrNull(1) as? IrConst)?.value as? Number ?: return CallKind.THROWS
                    return if (divisor.toLong() != 0L) CallKind.PURE else CallKind.THROWS
                }
                return CallKind.UNKNOWN
            }
            if (isArrayListInit(callee)) {
                // Allocates a list: unobservable, unless the capacity is negative.
                val capacity = call.arguments.getOrNull(1) ?: return CallKind.PURE
                if (capacity in constants) return CallKind.PURE
                val value = (capacity as? IrConst)?.value as? Int ?: return CallKind.THROWS
                return if (value >= 0) CallKind.PURE else CallKind.THROWS
            }
            return CallKind.UNKNOWN
        }
        return if (callee.fqNameWhenAvailable?.asString() in pureIntrinsics) CallKind.PURE else CallKind.UNKNOWN
    }

    /**
     * The effects of running [roots]. [adds] are analyzed as their element argument only, [constants] are treated as constants,
     * and [skipped] are ignored.
     */
    private fun effects(roots: List<IrElement>, adds: Set<IrCall> = emptySet(), constants: Set<IrCall> = emptySet(),
                        skipped: Set<IrElement> = emptySet()): Effects {
        val effects = Effects()
        val inner = HashSet<IrElement>()
        val visitor = object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) {
                if (element in skipped) return
                element.acceptChildrenVoid(this)
            }

            override fun visitExpression(expression: IrExpression) {
                // Anything not handled below.
                effects.heapWrite = true
                effects.throws = true
                expression.acceptChildrenVoid(this)
            }

            override fun visitDeclaration(declaration: IrDeclarationBase) {
                effects.heapWrite = true
                effects.throws = true
            }

            override fun visitVariable(declaration: IrVariable) {
                if (declaration in skipped) return
                effects.declared.add(declaration)
                declaration.acceptChildrenVoid(this)
            }

            override fun visitConst(expression: IrConst) {}

            override fun visitGetValue(expression: IrGetValue) {
                effects.reads.add(expression.symbol.owner)
            }

            override fun visitSetValue(expression: IrSetValue) {
                effects.writes.add(expression.symbol.owner)
                expression.acceptChildrenVoid(this)
            }

            override fun visitContainerExpression(expression: IrContainerExpression) {
                if (expression in skipped) return
                if (expression is IrReturnableBlock) inner.add(expression)
                expression.acceptChildrenVoid(this)
            }

            override fun visitWhen(expression: IrWhen) = expression.acceptChildrenVoid(this)

            override fun visitLoop(loop: IrLoop) {
                if (loop in skipped) return
                inner.add(loop)
                // Lowered `for` loops over progressions are counted. Those over iterators call `hasNext()`, which is unknown.
                if (loop.origin != IrStatementOrigin.FOR_LOOP_INNER_WHILE) effects.mayNotTerminate = true
                loop.acceptChildrenVoid(this)
            }

            override fun visitReturn(expression: IrReturn) {
                val target = expression.returnTargetSymbol.owner
                if (target !in inner) effects.outward.add(target)
                expression.acceptChildrenVoid(this)
            }

            override fun visitBreakContinue(jump: IrBreakContinue) {
                if (jump.loop !in inner) effects.outward.add(jump.loop)
            }

            override fun visitThrow(expression: IrThrow) {
                effects.throws = true
                expression.acceptChildrenVoid(this)
            }

            override fun visitTry(aTry: IrTry) {
                effects.hasTry = true
                effects.heapWrite = true
                effects.throws = true
                aTry.acceptChildrenVoid(this)
            }

            override fun visitGetField(expression: IrGetField) {
                if (!expression.symbol.owner.isFinal || expression.receiver == null) effects.heapRead = true
                expression.acceptChildrenVoid(this)
            }

            override fun visitSetField(expression: IrSetField) {
                effects.heapWrite = true
                expression.acceptChildrenVoid(this)
            }

            override fun visitGetObjectValue(expression: IrGetObjectValue) {
                // Other objects may need to be initialized.
                if (!expression.type.isUnit()) {
                    effects.heapWrite = true
                    effects.throws = true
                }
            }

            override fun visitTypeOperator(expression: IrTypeOperatorCall) {
                when (expression.operator) {
                    IrTypeOperator.IMPLICIT_CAST, IrTypeOperator.IMPLICIT_COERCION_TO_UNIT, IrTypeOperator.IMPLICIT_INTEGER_COERCION,
                    IrTypeOperator.INSTANCEOF, IrTypeOperator.NOT_INSTANCEOF, IrTypeOperator.SAFE_CAST, IrTypeOperator.REINTERPRET_CAST -> {}
                    else -> effects.throws = true
                }
                expression.acceptChildrenVoid(this)
            }

            override fun visitCall(expression: IrCall) {
                if (expression in skipped || expression in constants) return
                if (expression in adds) {
                    expression.arguments[1]?.acceptVoid(this)
                    return
                }
                when (callKind(expression, constants)) {
                    CallKind.PURE -> {}
                    CallKind.THROWS -> effects.throws = true
                    CallKind.UNKNOWN -> {
                        effects.heapWrite = true
                        effects.throws = true
                    }
                }
                expression.acceptChildrenVoid(this)
            }
        }
        roots.forEach { it.acceptVoid(visitor) }
        return effects
    }

    private fun declaredIn(root: IrElement): Set<IrVariable> = buildSet {
        root.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
            override fun visitVariable(declaration: IrVariable) {
                add(declaration)
                declaration.acceptChildrenVoid(this)
            }
        })
    }

    // Whether the code from [ancestor] down to [element] runs straight: [element] runs exactly once each time [ancestor] runs to
    // its end (at most once with [allowBranches]).
    private fun isStraight(state: FunctionState, ancestor: IrElement, element: IrElement, allowBranches: Boolean): Boolean {
        var current = element
        while (current !== ancestor) {
            val parent = state.parents[current] ?: return false
            val ok = when (parent) {
                is IrContainerExpression -> true
                is IrVariable -> parent.initializer === current
                is IrReturn -> parent.value === current && parent.returnTargetSymbol.owner is IrReturnableBlock
                is IrTypeOperatorCall -> parent.operator == IrTypeOperator.IMPLICIT_CAST || parent.operator == IrTypeOperator.IMPLICIT_COERCION_TO_UNIT
                is IrCall -> parent.symbol.owner.let { it.isBox() || it.isUnbox() }
                is IrWhen -> allowBranches
                is IrBranch -> allowBranches && parent.result === current
                else -> false
            }
            if (!ok) return false
            current = parent
        }
        return true
    }

    // ---------------------------------------------------------------------------------------------------------------------
    // Fusion.

    private fun tryFuse(function: IrFunction, state: FunctionState, creation: IrVariable): Boolean {
        val uses = analyzeUses(state, creation) ?: return false
        if (uses.iterators.size != 1) return reject("${uses.iterators.size} iterations")
        if (uses.adds.isEmpty() || uses.adds.size > 2) return reject("${uses.adds.size} adds")
        val iteration = iterationLoop(state, uses.iterators.single()) ?: return reject("unknown iteration")
        val init = uses.init!!
        val creationContainer = state.parents[creation] as? IrContainerExpression ?: return false
        if (state.parents[init] !== creationContainer ||
                creationContainer.statements.indexOf(init) != creationContainer.statements.indexOf(creation) + 1) return reject("init")

        // K: the block whose statement Kp contains the adds and whose later statement Kc contains the consuming loop.
        val k = state.lowestCommonAncestor(uses.adds + iteration.loop) as? IrContainerExpression ?: return reject("no common block")
        val kp = state.childOn(k, uses.adds.first())!!
        if (uses.adds.any { state.childOn(k, it) !== kp }) return reject("adds in different statements")
        val kc = state.childOn(k, iteration.loop)!!
        val kpIndex = k.statements.indexOf(kp)
        val kcIndex = k.statements.indexOf(kc)
        if (kpIndex < 0 || kcIndex <= kpIndex) return reject("order")

        // The list is created once before the adds, each time K runs.
        val kCreation = state.childOn(k, creation) ?: return reject("created outside")
        val kCreationIndex = k.statements.indexOf(kCreation)
        if (kCreationIndex < 0 || kCreationIndex > kpIndex) return reject("created after")
        if (!isStraight(state, k, creation, allowBranches = false)) return reject("created conditionally")
        if (uses.adds.any { state.order[it]!! < state.order[creation]!! }) return reject("added before creation")
        // The consuming loop runs at most once each time K runs.
        if (!isStraight(state, k, iteration.container, allowBranches = false)) return reject("consumed conditionally")

        // P: the outermost loop around the adds below K, or their common ancestor.
        val addsAncestor = state.lowestCommonAncestor(uses.adds)!!
        val addsPath = state.path(addsAncestor)
        val kDepth = addsPath.indexOf(k)
        val producer = addsPath.drop(kDepth + 1).firstOrNull { it is IrLoop } ?: addsAncestor
        if (!isStraight(state, k, producer, allowBranches = true)) return reject("produced conditionally")
        // The blocks around the producer, below K: returning from them ends the producer early.
        val producerAncestors: Set<IrElement> = state.path(producer).drop(kDepth + 1).toSet() - producer

        // X: what runs after the producer and before the consumer.
        val between = ArrayList<IrElement>()
        val pathDeclared = HashSet<IrVariable>()
        run {
            var current: IrElement = producer
            while (true) {
                val parent = state.parents[current]!!
                if (parent === k) break
                if (parent is IrContainerExpression) {
                    val statements = parent.statements
                    between.addAll(statements.subList(statements.indexOf(current) + 1, statements.size))
                }
                if (parent is IrVariable) pathDeclared.add(parent)
                current = parent
            }
        }
        val hoistable = LinkedHashSet<IrVariable>()
        k.statements.subList(kpIndex + 1, kcIndex).forEach {
            between.add(it)
            if (it is IrVariable) hoistable.add(it)
        }
        run {
            var current: IrElement = iteration.iterator
            while (true) {
                val parent = state.parents[current]!!
                if (parent === k) break
                if (parent is IrContainerExpression) {
                    val statements = parent.statements
                    statements.subList(0, statements.indexOf(current)).forEach {
                        between.add(it)
                        if (it is IrVariable) hoistable.add(it)
                    }
                }
                current = parent
            }
        }
        val sizeHints = uses.sizeHints.toSet()
        val betweenEffects = effects(between, constants = sizeHints)
        if (betweenEffects.heapWrite || betweenEffects.throws || betweenEffects.mayNotTerminate || betweenEffects.hasTry)
            return reject("impure code between the loops")
        if (betweenEffects.outward.any { it !in producerAncestors }) return reject("jump between the loops")

        // C: the consumer's body.
        val consumerStatements = iteration.body.statements.drop(1)
        val consumerEffects = effects(consumerStatements)
        consumerEffects.declared.add(iteration.element)
        if (consumerEffects.outward.isNotEmpty()) return reject("jump out of the consumer")

        val producerEffects = effects(listOf(producer), adds = uses.adds.toSet())
        val producerJumpsOut = producerEffects.outward.any { it !in producerAncestors }

        val declaredInK = declaredIn(k)
        val declaredFromKp = declaredIn(kp) + betweenEffects.declared + pathDeclared
        val producerPure = !producerEffects.heapWrite && !producerEffects.throws && !producerEffects.mayNotTerminate && !producerJumpsOut &&
                producerEffects.writes.all { it in declaredInK }
        val consumerPure = !consumerEffects.heapWrite && !consumerEffects.throws && !consumerEffects.mayNotTerminate &&
                consumerEffects.writes.all { it in declaredInK }
        if (!producerPure && !consumerPure) return reject("both sides have effects")
        if (producerEffects.heapWrite && consumerEffects.heapRead) return reject("consumer reads memory the producer writes")
        if (consumerEffects.heapWrite && producerEffects.heapRead) return reject("producer reads memory the consumer writes")
        if (consumerEffects.heapWrite && betweenEffects.heapRead) return reject("code between reads memory the consumer writes")
        if (producerEffects.writes.any { it in consumerEffects.reads || it in consumerEffects.writes }) return reject("producer writes what the consumer uses")
        if (consumerEffects.writes.any { it in producerEffects.reads || it in betweenEffects.reads }) return reject("consumer writes what the producer uses")
        if (betweenEffects.writes.any { it !in betweenEffects.declared || it in consumerEffects.reads }) return reject("code between writes variables")
        if (!consumerPure && uses.adds.any { add -> state.path(add).any { it is IrTry && state.isAncestor(producer, it) } })
            return reject("adds in a try")

        // The variables the consumer uses that are declared between the loops are declared before the producer instead.
        val free = (consumerEffects.reads + consumerEffects.writes).filterIsInstance<IrVariable>().filter { it !in consumerEffects.declared }
        val hoisted = LinkedHashSet<IrVariable>()
        val worklist = ArrayDeque<IrVariable>()
        fun require(variable: IrVariable): Boolean {
            if (variable in hoisted) return true
            if (variable in uses.vars) return false
            if (variable in hoistable) {
                hoisted.add(variable)
                worklist.add(variable)
                return true
            }
            return variable !in declaredFromKp
        }
        for (variable in free) if (!require(variable)) return reject("consumer uses ${variable.name}, computed after the producer")
        while (worklist.isNotEmpty()) {
            val variable = worklist.removeFirst()
            val initializer = variable.initializer ?: continue
            val initializerEffects = effects(listOf(initializer), constants = sizeHints)
            if (!initializerEffects.isPure || initializerEffects.heapRead || initializerEffects.writes.any { it !in initializerEffects.declared })
                return reject("impure initializer of ${variable.name}")
            for (read in initializerEffects.reads) {
                if (read in initializerEffects.declared) continue
                if (read is IrVariable && read.isVar) return reject("initializer of ${variable.name} reads a var")
                if (read is IrVariable && !require(read)) return reject("initializer of ${variable.name} reads ${read.name}")
            }
        }

        // Transform.
        if (log) println("CollectionFusion: ${function.fqNameWhenAvailable}: fused ${creation.name} (producer pure: $producerPure, consumer pure: $consumerPure)")
        // Copies of the creation, to throw the same exception as the original for a negative capacity.
        val creationCopy = creation.deepCopyWithSymbols(function)
        val initCopy = init.deepCopyWithSymbols(function)

        for (hint in uses.sizeHints) replaceChild(state.parents[hint]!!, hint, hint.arguments[1]!!.deepCopyWithSymbols(function))
        val hoistedInOrder = hoisted.sortedBy { state.order[it]!! }
        for (variable in hoistedInOrder) (state.parents[variable] as IrContainerExpression).statements.remove(variable)
        k.statements.addAll(k.statements.indexOf(kp), hoistedInOrder)

        iteration.container.statements.remove(iteration.iterator)
        iteration.container.statements.remove(iteration.loop)
        val element = iteration.element
        iteration.body.statements.removeAt(0)
        element.initializer = null
        val consumerBody = IrBlockImpl(iteration.body.startOffset, iteration.body.endOffset, context.irBuiltIns.unitType)
        consumerBody.statements.add(element)
        consumerBody.statements.addAll(iteration.body.statements)
        iteration.body.statements.clear()
        uses.adds.forEachIndexed { index, add ->
            val replacement = if (index == uses.adds.lastIndex) consumerBody else consumerBody.deepCopyWithSymbols(function)
            (replacement.statements[0] as IrVariable).initializer = add.arguments[1]!!
            val parent = state.parents[add]!!
            if (parent is IrTypeOperatorCall && parent.operator == IrTypeOperator.IMPLICIT_COERCION_TO_UNIT)
                replaceChild(state.parents[parent]!!, parent, replacement)
            else
                replaceChild(parent, add, replacement)
        }

        // The list is no longer built. Its capacity is still evaluated, and checked.
        for (variable in uses.vars) variable.type = variable.type.makeNullable()
        creation.initializer = IrConstImpl.constNull(creation.startOffset, creation.endOffset, creation.type)
        val capacity = init.arguments.getOrNull(1)
        val initReplacement: IrExpression = if (capacity == null || (((capacity as? IrConst)?.value as? Int) ?: -1) >= 0) {
            IrBlockImpl(init.startOffset, init.endOffset, context.irBuiltIns.unitType)
        } else context.createIrBuilder(function.symbol, init.startOffset, init.endOffset).irBlock(resultType = context.irBuiltIns.unitType) {
            val value = irTemporary(capacity, nameHint = "capacity")
            +irIfThen(context.irBuiltIns.unitType,
                    irCall(context.irBuiltIns.lessFunByOperandType[context.irBuiltIns.intClass]!!).apply {
                        arguments[0] = irGet(value)
                        arguments[1] = irInt(0)
                    },
                    irBlock(resultType = context.irBuiltIns.unitType) {
                        +creationCopy
                        +initCopy.apply {
                            arguments[0] = irGet(creationCopy)
                            arguments[1] = irGet(value)
                        }
                    })
        }
        replaceChild(creationContainer, init, initReplacement)
        return true
    }

    private fun replaceChild(parent: IrElement, old: IrElement, new: IrElement) {
        val newExpression = new as IrExpression
        val replaced = when (parent) {
            is IrContainerExpression -> parent.statements.indexOfFirst { it === old }.let { index ->
                if (index >= 0) parent.statements[index] = newExpression
                index >= 0
            }
            is IrTypeOperatorCall -> (parent.argument === old).also { if (it) parent.argument = newExpression }
            is IrVariable -> (parent.initializer === old).also { if (it) parent.initializer = newExpression }
            is IrReturn -> (parent.value === old).also { if (it) parent.value = newExpression }
            is IrSetValue -> (parent.value === old).also { if (it) parent.value = newExpression }
            is IrBranch -> when {
                parent.condition === old -> { parent.condition = newExpression; true }
                parent.result === old -> { parent.result = newExpression; true }
                else -> false
            }
            is IrLoop -> when {
                parent.condition === old -> { parent.condition = newExpression; true }
                parent.body === old -> { parent.body = newExpression; true }
                else -> false
            }
            is IrMemberAccessExpression<*> -> parent.arguments.indexOfFirst { it === old }.let { index ->
                if (index >= 0) parent.arguments[index] = newExpression
                index >= 0
            }
            else -> false
        }
        check(replaced) { "CollectionFusion: cannot replace a child of ${parent::class.simpleName}" }
    }

    // ---------------------------------------------------------------------------------------------------------------------
    // Boxes.

    /**
     * Removes the boxing of values in `val x = box(v)` whose uses (directly or through `val y = x` copies) are all unboxings
     * to the same type: the variables then hold the primitive value. The identity of such boxes is unobservable.
     */
    private fun foldBoxes(function: IrFunction, body: IrBody) {
        val state = FunctionState(body)
        val candidates = ArrayList<IrVariable>()
        body.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
            override fun visitVariable(declaration: IrVariable) {
                declaration.acceptChildrenVoid(this)
                val initializer = declaration.initializer as? IrCall ?: return
                if (!declaration.isVar && initializer.symbol.owner.isBox()) candidates.add(declaration)
            }
        })
        for (variable in candidates) {
            val box = variable.initializer as IrCall
            val primitiveType = box.symbol.owner.parameters[0].type
            if (!primitiveType.isPrimitiveType()) continue
            val copies = ArrayList<IrVariable>()
            val unboxes = ArrayList<IrCall>()
            fun check(source: IrVariable): Boolean {
                for (get in state.gets[source].orEmpty()) {
                    var current: IrElement = get
                    var parent = state.parents[current]
                    while (parent is IrTypeOperatorCall && parent.operator == IrTypeOperator.IMPLICIT_CAST) {
                        current = parent
                        parent = state.parents[current]
                    }
                    when {
                        parent is IrCall && parent.symbol.owner.isUnbox() && parent.arguments[0] === current
                                && parent.symbol.owner.returnType.isPrimitiveType() && parent.symbol.owner.returnType.classOrNull == primitiveType.classOrNull -> unboxes.add(parent)
                        parent is IrVariable && parent.initializer === current && !parent.isVar -> {
                            copies.add(parent)
                            if (!check(parent)) return false
                        }
                        else -> return false
                    }
                }
                return true
            }
            if (!check(variable)) continue
            val builder = context.createIrBuilder(function.symbol, variable.startOffset, variable.endOffset)
            variable.initializer = box.arguments[0]
            variable.type = primitiveType
            for (copy in copies) {
                var source = copy.initializer!!
                while (source is IrTypeOperatorCall) source = source.argument
                copy.type = primitiveType
                copy.initializer = builder.irGet(primitiveType, (source as IrGetValue).symbol)
            }
            for (unbox in unboxes) {
                var source = unbox.arguments[0]!!
                while (source is IrTypeOperatorCall) source = source.argument
                replaceChild(state.parents[unbox]!!, unbox, builder.irGet(primitiveType, (source as IrGetValue).symbol))
            }
        }
    }
}

private fun isListCreation(variable: IrVariable): Boolean {
    val call = variable.initializer as? IrCall ?: return false
    return call.symbol.owner.name.asString() == "createUninitializedInstance" &&
            call.symbol.owner.fqNameWhenAvailable?.asString() == "kotlin.native.internal.createUninitializedInstance" &&
            call.type.classFqName?.asString() == "kotlin.collections.ArrayList"
}
