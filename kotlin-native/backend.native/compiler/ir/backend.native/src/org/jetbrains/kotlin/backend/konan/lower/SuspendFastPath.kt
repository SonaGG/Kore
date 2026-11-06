/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.backend.konan.lower

import org.jetbrains.kotlin.backend.common.lower.createIrBuilder
import org.jetbrains.kotlin.backend.konan.Context
import org.jetbrains.kotlin.descriptors.Modality
import org.jetbrains.kotlin.ir.IrElement
import org.jetbrains.kotlin.ir.builders.irBlockBody
import org.jetbrains.kotlin.ir.builders.irCall
import org.jetbrains.kotlin.ir.builders.irGet
import org.jetbrains.kotlin.ir.builders.irReturn
import org.jetbrains.kotlin.ir.declarations.*
import org.jetbrains.kotlin.ir.expressions.IrCall
import org.jetbrains.kotlin.ir.expressions.IrExpression
import org.jetbrains.kotlin.ir.types.defaultType
import org.jetbrains.kotlin.ir.util.deepCopyWithSymbols
import org.jetbrains.kotlin.ir.util.isInterface
import org.jetbrains.kotlin.ir.visitors.IrElementTransformerVoid
import org.jetbrains.kotlin.ir.visitors.IrVisitorVoid
import org.jetbrains.kotlin.ir.visitors.acceptChildrenVoid
import org.jetbrains.kotlin.ir.visitors.transformChildrenVoid
import org.jetbrains.kotlin.name.Name

/**
 * Kore: suspend functions that can never suspend are called as plain functions.
 *
 * A suspend function that calls other suspend functions (other than in tail position) becomes a state machine, which
 * allocates its continuation on every call, and its callers box its result through `Any?` and check for
 * `COROUTINE_SUSPENDED`, even when nothing ever suspends (e.g. `suspend fun leaf(x: Int) = x * 2`).
 *
 * A suspend function never suspends when it is final, has a body here, and every suspend call in it (including in
 * local functions and lambdas, conservatively) is a direct call to a function that never suspends. (Calls of suspend
 * lambdas, of open or abstract functions, of functions without a body (intrinsics such as `coroutineContext` or
 * `suspendCoroutineUninterceptedOrReturn`) and recursion make a function possibly suspending.)
 *
 * Each such function gets a non-suspend twin with the same body; every direct call of the function becomes a call of
 * its twin, and the function itself (still used by references and calls through overridden declarations) calls its
 * twin. The coroutine lowering then sees no suspension points in these calls.
 *
 * Runs after all inline functions have been inlined, before the coroutine lowering, over all the modules compiled
 * here at once (the lowerings before it are in the same state in all of them).
 */
internal class SuspendFastPath(private val context: Context, private val modules: List<IrModuleFragment>) {
    private enum class State { IN_PROGRESS, NEVER_SUSPENDS, MAY_SUSPEND }

    private val candidates = mutableSetOf<IrSimpleFunction>()
    private val states = mutableMapOf<IrSimpleFunction, State>()

    fun run() {
        for (module in modules) for (file in module.files) collectCandidates(file.declarations)
        val neverSuspending = candidates.filter { neverSuspends(it) }
        if (neverSuspending.isEmpty()) return

        val twins = neverSuspending.associateWith { createTwin(it) }
        for (entry in twins) {
            val function = entry.key
            val twin = entry.value
            when (val parent = function.parent) {
                is IrClass -> parent.declarations += twin
                is IrFile -> parent.declarations += twin
                else -> error("Unexpected parent of ${function.name}")
            }
        }
        // Calls of the functions become calls of their twins (also in the twins themselves).
        val retarget = object : IrElementTransformerVoid() {
            override fun visitCall(expression: IrCall): IrExpression {
                expression.transformChildrenVoid(this)
                twins[expression.symbol.owner]?.let { expression.symbol = it.symbol }
                return expression
            }
        }
        for (module in modules) for (file in module.files) file.transformChildrenVoid(retarget)
        // The functions call their twins.
        for (entry in twins) delegateTo(entry.key, entry.value)
    }

    private fun collectCandidates(declarations: List<IrDeclaration>) {
        for (declaration in declarations) {
            when (declaration) {
                is IrClass -> collectCandidates(declaration.declarations)
                is IrSimpleFunction -> if (isCandidate(declaration)) candidates += declaration
                else -> {}
            }
        }
    }

    private fun isCandidate(function: IrSimpleFunction): Boolean {
        if (!function.isSuspend || function.isInline || function.isExternal || function.isExpect || function.isFakeOverride)
            return false
        if (function.body == null || function.modality != Modality.FINAL) return false
        val parent = function.parent
        return parent is IrFile || (parent is IrClass && !parent.isInterface)
    }

    private fun neverSuspends(function: IrSimpleFunction): Boolean {
        when (states[function]) {
            State.NEVER_SUSPENDS -> return true
            State.MAY_SUSPEND, State.IN_PROGRESS -> return false // Recursion counts as possibly suspending.
            null -> {}
        }
        if (function !in candidates) return false
        states[function] = State.IN_PROGRESS
        var result = true
        function.acceptChildrenVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) {
                if (result) element.acceptChildrenVoid(this)
            }

            override fun visitCall(expression: IrCall) {
                val callee = expression.symbol.owner
                if (callee.isSuspend && !neverSuspends(callee)) result = false
                // The continuation of the calling function is available to intrinsics only in suspend functions.
                if (expression.symbol == context.symbols.getContinuation) result = false
                if (result) expression.acceptChildrenVoid(this)
            }
        })
        states[function] = if (result) State.NEVER_SUSPENDS else State.MAY_SUSPEND
        return result
    }

    private fun createTwin(function: IrSimpleFunction): IrSimpleFunction {
        val twin = function.deepCopyWithSymbols(function.parent)
        twin.name = Name.identifier("${function.name.asString()}\$nosuspend")
        twin.isSuspend = false
        twin.overriddenSymbols = emptyList()
        twin.annotations = emptyList()
        return twin
    }

    private fun delegateTo(function: IrSimpleFunction, twin: IrSimpleFunction) {
        val builder = context.createIrBuilder(function.symbol, function.startOffset, function.endOffset)
        function.body = builder.irBlockBody {
            val call = irCall(twin.symbol, function.returnType).apply {
                function.typeParameters.forEachIndexed { index, typeParameter -> typeArguments[index] = typeParameter.defaultType }
                function.parameters.forEachIndexed { index, parameter -> arguments[index] = irGet(parameter) }
            }
            +irReturn(call)
        }
    }
}
