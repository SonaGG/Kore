/*
 * Copyright 2010-2024 JetBrains s.r.o. and Kotlin Programming Language contributors.
 * Use of this source code is governed by the Apache 2.0 license that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.backend.konan.optimizations

import org.jetbrains.kotlin.backend.common.lower.optimizations.LivenessAnalysis
import org.jetbrains.kotlin.backend.common.peek
import org.jetbrains.kotlin.backend.common.pop
import org.jetbrains.kotlin.backend.common.push
import org.jetbrains.kotlin.backend.konan.NativeGenerationState
import org.jetbrains.kotlin.backend.konan.binaryTypeIsReference
import org.jetbrains.kotlin.backend.konan.ir.isArray
import org.jetbrains.kotlin.backend.konan.ir.isBoxOrUnbox
import org.jetbrains.kotlin.backend.konan.ir.isInlineClass
import org.jetbrains.kotlin.backend.konan.ir.konanLibrary
import org.jetbrains.kotlin.backend.konan.lower.*
import org.jetbrains.kotlin.ir.declarations.IrClass
import org.jetbrains.kotlin.ir.declarations.IrField
import org.jetbrains.kotlin.ir.declarations.IrFunction
import org.jetbrains.kotlin.ir.declarations.IrProperty
import org.jetbrains.kotlin.ir.expressions.IrSuspensionPoint
import org.jetbrains.kotlin.ir.types.classOrNull
import org.jetbrains.kotlin.ir.types.isAny
import org.jetbrains.kotlin.ir.util.*
import org.jetbrains.kotlin.library.metadata.isCInteropLibrary

/*
 * A simple greedy inliner. Traverses the call graph and inlines all the callees with size less than a specified threshold.
 * The call graph traversal is done using the tricolor DFS algorithm to be able to detect cycles.
 * The DFS itself is unwrapped from a recursion to a loop to circumvent the SO exception on particular big graphs.
 */
internal class PreCodegenInliner(
        val generationState: NativeGenerationState,
        val moduleDFG: ModuleDFG,
        val callGraph: CallGraph,
) {
    private val context = generationState.context
    private val symbols = context.symbols
    private val noInline = symbols.noInline
    private val string = context.irBuiltIns.stringClass
    private val throwable = context.irBuiltIns.throwableClass
    private val kFunctionImpl = symbols.kFunctionImpl
    private val kSuspendFunctionImpl = symbols.kSuspendFunctionImpl
    private val invokeSuspendFunction = symbols.invokeSuspendFunction

    private val inlineThreshold = context.config.preCodegenInlineThreshold.toInt()
    private val targeted = context.config.preCodegenInlineTargeted

    // Kore: whether [body] returns an object it allocates.
    private fun returnsNewObject(body: DataFlowIR.FunctionBody): Boolean {
        val visited = mutableSetOf<DataFlowIR.Node>()
        fun reaches(node: DataFlowIR.Node): Boolean {
            if (!visited.add(node)) return false
            return when (node) {
                is DataFlowIR.Node.AllocInstance -> true
                is DataFlowIR.Node.Variable -> node.values.any { reaches(it.node) }
                else -> false
            }
        }
        return reaches(body.returns)
    }

    // Kore: whether [node] is an object created for the call: allocated by the caller, or returned by a function that
    // allocates it (and that is inlined). Inlining the callee lets the object be replaced by its fields.
    // Only objects of classes without references, which the object scalar replacement LLVM pass can replace.
    private fun isFreshObject(node: DataFlowIR.Node, depth: Int = 0): Boolean = when (node) {
        is DataFlowIR.Node.AllocInstance -> isReplaceable(node.type.irClass)
        is DataFlowIR.Node.Call -> isReplaceable(node.returnType.irClass) && (node.callee as? DataFlowIR.FunctionSymbol.Declared)
                ?.let { moduleDFG.functions[it] }?.let { returnsNewObject(it.body) } == true
        is DataFlowIR.Node.Variable -> depth < 4 && node.values.isNotEmpty() && node.values.all { isFreshObject(it.node, depth + 1) }
        else -> false
    }

    private val replaceableClasses = mutableMapOf<IrClass, Boolean>()

    private fun isReplaceable(irClass: IrClass?): Boolean = irClass != null && replaceableClasses.getOrPut(irClass) {
        irClass.isFinalClass && !irClass.isInterface
                && irClass.superTypes.all { it.classOrNull?.owner?.isInterface == true || it.isAny() }
                && irClass.declarations.all { declaration ->
                    val field = (declaration as? IrField) ?: (declaration as? IrProperty)?.backingField
                    field == null || field.isStatic || !field.type.binaryTypeIsReference()
                }
    }

    private companion object {
        // Kore: the size up to which functions are inlined into callers that pass them fresh objects.
        const val FRESH_ARGUMENT_INLINE_THRESHOLD = 150
        const val INLINING_ROUNDS = 3
    }

    private val rootSet = callGraph.rootSet

    /**
     * Node color (same as in the tricolor DFS algorithm).
     */
    private enum class NodeColor {
        /**
         * The node is not processed yet.
         */
        WHITE,

        /**
         * The node is being processed (DFS has started but not finished yet).
         */
        GRAY,

        /**
         * The node and all its children are processed.
         */
        BLACK
    }

    fun run() {
        val nodeColors = mutableMapOf<DataFlowIR.FunctionSymbol.Declared, NodeColor>()
        val stack = rootSet.toMutableList()
        for (root in stack)
            nodeColors[root] = NodeColor.WHITE

        while (stack.isNotEmpty()) {
            val functionSymbol = stack.peek()!!
            val color = nodeColors[functionSymbol]!!
            val callSites = callGraph.directEdges[functionSymbol]!!.callSites.filter {
                !it.isVirtual && callGraph.directEdges.containsKey(it.actualCallee)
            }
            when (color) {
                NodeColor.WHITE -> {
                    nodeColors[functionSymbol] = NodeColor.GRAY
                    for (callSite in callSites) {
                        val calleeSymbol = callSite.actualCallee as DataFlowIR.FunctionSymbol.Declared
                        if (nodeColors[calleeSymbol] == null || nodeColors[calleeSymbol] == NodeColor.WHITE) {
                            nodeColors[calleeSymbol] = NodeColor.WHITE
                            stack.push(calleeSymbol)
                        }
                    }
                }

                NodeColor.GRAY -> {
                    stack.pop()
                    nodeColors[functionSymbol] = NodeColor.BLACK

                    val irFunction = functionSymbol.irFunction ?: continue
                    val irBody = irFunction.body ?: continue
                    if (irFunction.bridgeTarget != null
                            || irFunction.konanLibrary?.isCInteropLibrary() == true
                            || irFunction.originalConstructor?.let { constructor ->
                                // To support IR pattern recognition in IrToBitcode.kt on IrConstantObject generation.
                                constructor.parameters.isEmpty()
                                        && constructor.constructedClass.superTypes.any {
                                    it.classOrNull == kFunctionImpl || it.classOrNull == kSuspendFunctionImpl
                                }
                            } == true
                    ) {
                        continue
                    }

                    // The calls in the function, and then (Kore) the calls exposed by inlining: functions inlined into
                    // it may pass fresh objects on.
                    var calls: List<Pair<DataFlowIR.FunctionSymbol.Declared, List<DataFlowIR.Edge>>> =
                            callSites.map { (it.actualCallee as DataFlowIR.FunctionSymbol.Declared) to it.call.arguments }
                    for (round in 0 until INLINING_ROUNDS) {
                        val functionsToInline = mutableSetOf<IrFunction>()
                        for ([calleeSymbol, arguments] in calls) {
                            if (calleeSymbol == functionSymbol || nodeColors[calleeSymbol] != NodeColor.BLACK) continue
                            functionToInline(calleeSymbol, arguments, firstRound = round == 0)?.let { functionsToInline.add(it) }
                        }
                        if (functionsToInline.isEmpty()) break

                        PreCodegenFunctionInlining(context, functionsToInline).run(irFunction)

                        // KT-72336: This is not entirely correct since coroutinesLivenessAnalysisPhase could be turned off.
                        LivenessAnalysis.run(irBody) { it is IrSuspensionPoint }
                                .forEach { [irElement, liveVariables] ->
                                    (irElement as IrSuspensionPoint).liveVariablesAtSuspensionPoint = liveVariables
                                }

                        val rebuiltFunction = FunctionDFGBuilder(generationState, moduleDFG.symbolTable).build(irFunction)
                        moduleDFG.functions[functionSymbol] = rebuiltFunction
                        if (!targeted) break
                        val exposed = mutableListOf<Pair<DataFlowIR.FunctionSymbol.Declared, List<DataFlowIR.Edge>>>()
                        rebuiltFunction.body.forEachNonScopeNode { node ->
                            if (node is DataFlowIR.Node.StaticCall) {
                                val callee = node.callee as? DataFlowIR.FunctionSymbol.Declared
                                if (callee != null && moduleDFG.functions.containsKey(callee))
                                    exposed += callee to node.arguments
                            }
                        }
                        calls = exposed
                    }
                }

                NodeColor.BLACK -> {
                    stack.pop()
                }
            }
        }
    }

    /**
     * The function to inline for a call to [calleeSymbol] with [arguments], if any. Kore: in the targeted mode, small
     * functions returning new objects are inlined (in the first round), and larger ones taking fresh objects.
     */
    private fun functionToInline(
            calleeSymbol: DataFlowIR.FunctionSymbol.Declared,
            arguments: List<DataFlowIR.Edge>,
            firstRound: Boolean,
    ): IrFunction? {
        val calleeIrFunction = calleeSymbol.irFunction ?: return null
        val callee = moduleDFG.functions[calleeSymbol] ?: return null

        var isALoop = false
        callee.body.forEachNonScopeNode { node ->
            if (node is DataFlowIR.Node.Call && node.callee == calleeSymbol)
                isALoop = true
        }

        val calleeSize = callee.body.allScopes.sumOf { it.nodes.size }
        val shouldInline = !isALoop // As FunctionInlining doesn't work with recursive functions.
                && (calleeSize <= inlineThreshold && (!targeted || returnsNewObject(callee.body)) && (firstRound || targeted)
                        || targeted && calleeSize <= FRESH_ARGUMENT_INLINE_THRESHOLD
                        && arguments.any { isFreshObject(it.node) })
                && calleeIrFunction.symbol != context.symbols.entryPoint // Might be unexpected to not see [main] in stacktraces.
                && !calleeIrFunction.isBoxOrUnbox()
                && !calleeIrFunction.isLazyStaticInitializer
                && !calleeIrFunction.isEagerStaticInitializer
                && calleeIrFunction.konanLibrary?.isCInteropLibrary() != true
                && !calleeIrFunction.hasAnnotation(noInline)
                && calleeIrFunction.correspondingPropertySymbol?.owner?.hasAnnotation(noInline) != true
                && !calleeIrFunction.overrides(invokeSuspendFunction.owner)
                && calleeIrFunction.correspondingPropertySymbol?.owner?.let {
            it.parentClassOrNull?.isInlineClass == true && it.backingField != null
        } != true
                && calleeIrFunction.originalConstructor?.constructedClass?.let { it.isArray || it.symbol == string } != true
                && calleeIrFunction.originalConstructor?.constructedClass?.getAllSuperclasses()?.contains(throwable.owner) != true
        return calleeIrFunction.takeIf { shouldInline }
    }
}
