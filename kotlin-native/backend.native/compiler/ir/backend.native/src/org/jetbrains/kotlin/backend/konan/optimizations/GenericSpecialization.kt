/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.backend.konan.optimizations

import org.jetbrains.kotlin.backend.konan.lower.isMonomorphizedCopy
import org.jetbrains.kotlin.backend.konan.Context
import org.jetbrains.kotlin.backend.common.lower.createIrBuilder
import org.jetbrains.kotlin.descriptors.ClassKind
import org.jetbrains.kotlin.descriptors.Modality
import org.jetbrains.kotlin.ir.IrElement
import org.jetbrains.kotlin.ir.builders.*
import org.jetbrains.kotlin.ir.declarations.*
import org.jetbrains.kotlin.ir.expressions.IrCall
import org.jetbrains.kotlin.ir.expressions.IrExpression
import org.jetbrains.kotlin.ir.expressions.IrGetValue
import org.jetbrains.kotlin.ir.symbols.IrClassSymbol
import org.jetbrains.kotlin.ir.symbols.IrTypeParameterSymbol
import org.jetbrains.kotlin.ir.types.*
import org.jetbrains.kotlin.ir.util.*
import org.jetbrains.kotlin.ir.visitors.IrVisitorVoid
import org.jetbrains.kotlin.ir.visitors.acceptChildrenVoid
import org.jetbrains.kotlin.ir.visitors.acceptVoid
import org.jetbrains.kotlin.ir.visitors.IrElementTransformerVoid
import org.jetbrains.kotlin.ir.visitors.transformChildrenVoid
import org.jetbrains.kotlin.name.Name

/**
 * Kore: specialization of generic member functions for the final classes their class's type parameters are bound to at
 * the call site, like C++ templates (or HotSpot's profile-guided inlining, without a profile).
 *
 * Kotlin generics are erased, so e.g. `HashMap.findKey` calls `equals` and `hashCode` virtually on keys of any type, and
 * the devirtualization analysis, which does not distinguish one map from another, sees every key type of the program
 * there. For a call `map.get(key)` with `map: HashMap<ChunkPos, Chunk>` and `ChunkPos` final, this pass calls a clone
 * of `get`, in which calls on values of type `K` are guarded: `if (k is ChunkPos) (k as ChunkPos).equals(x) else
 * k.equals(x)`. The devirtualization analysis then turns the guarded calls into direct ones, which LLVM can inline.
 * The unguarded fallback keeps the behavior for heap pollution (a key of another type in the map via an unchecked cast).
 *
 * Clones are specialized transitively: calls to other members on `this` (`get` -> `findKey` -> `hash`) and member calls
 * on other generic receivers whose type arguments are bound (`HashSet<E>.contains` -> `HashMap<E, Boolean>.containsKey`)
 * go to their clones for the same bindings.
 */
internal class GenericSpecialization(private val context: Context, private val module: IrModuleFragment) {
    private data class Request(val function: IrSimpleFunction, val bindings: Map<IrTypeParameterSymbol, IrClass>)

    private val clones = mutableMapOf<Request, IrSimpleFunction?>()
    private val sensitivity = mutableMapOf<Request, Boolean>()
    private val inProgress = mutableSetOf<Request>()
    private var cloneCount = 0

    fun run() {
        // Requests at call sites in the original code.
        val sites = mutableListOf<Pair<IrCall, Request>>()
        module.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)

            // Kore: copies made by monomorphization are specialized already; their calls stay as they are.
            override fun visitClass(declaration: IrClass) {
                if (declaration.isMonomorphizedCopy != true) super.visitClass(declaration)
            }

            override fun visitCall(expression: IrCall) {
                expression.acceptChildrenVoid(this)
                requestFor(expression, emptyMap())?.let { sites += expression to it }
            }
        })
        for ([call, request] in sites) {
            val clone = cloneFor(request) ?: continue
            call.symbol = clone.symbol
        }
    }

    // The final class a type is bound to, given the bindings of the type parameters in scope.
    private fun boundClass(type: IrType, bindings: Map<IrTypeParameterSymbol, IrClass>): IrClass? {
        val simpleType = type as? IrSimpleType ?: return null
        return when (val classifier = simpleType.classifier) {
            is IrTypeParameterSymbol -> bindings[classifier]
            is IrClassSymbol -> classifier.owner.takeIf { it.isSpecializable() }
            else -> null
        }
    }

    private fun IrClass.isSpecializable() =
            modality == Modality.FINAL && kind == ClassKind.CLASS && !isValue && typeParameters.isEmpty()

    // A request to specialize the callee of [call] (a member call on a generic receiver with bound type arguments).
    private fun requestFor(call: IrCall, bindings: Map<IrTypeParameterSymbol, IrClass>): Request? {
        val callee = call.symbol.owner
        if (callee.dispatchReceiverParameter == null || call.arguments.isEmpty()) return null
        val receiver = call.arguments[0] ?: return null
        val receiverType = receiver.type as? IrSimpleType ?: return null
        val receiverClass = (receiverType.classifier as? IrClassSymbol)?.owner ?: return null
        if (receiverClass.typeParameters.isEmpty() || receiverType.arguments.size != receiverClass.typeParameters.size) return null
        // The call must be statically bound to a function with a body.
        val target = callee.resolveFakeOverride() ?: return null
        if (target.body == null || target.isExternal || target.parent != receiverClass) return null
        if (call.superQualifierSymbol == null && target.isOverridable && receiverClass.modality != Modality.FINAL) return null
        val classBindings = mutableMapOf<IrTypeParameterSymbol, IrClass>()
        receiverClass.typeParameters.forEachIndexed { index, parameter ->
            val argument = receiverType.arguments[index] as? IrTypeProjection ?: return@forEachIndexed
            boundClass(argument.type, bindings)?.let { classBindings[parameter.symbol] = it }
        }
        if (classBindings.isEmpty()) return null
        return Request(target, classBindings).takeIf { isSensitive(it) }
    }

    // Whether specializing [request] changes anything: its body makes virtual calls on values of a bound type parameter,
    // or calls functions that do.
    private fun isSensitive(request: Request): Boolean {
        sensitivity[request]?.let { return it }
        if (!inProgress.add(request)) return false
        val body = request.function.body
        var sensitive = false
        if (body != null && isCloneable(request.function)) {
            body.acceptVoid(object : IrVisitorVoid() {
                override fun visitElement(element: IrElement) {
                    if (!sensitive) element.acceptChildrenVoid(this)
                }

                override fun visitCall(expression: IrCall) {
                    if (guardedClass(expression, request) != null || innerRequest(expression, request) != null) sensitive = true
                    else super.visitCall(expression)
                }
            })
        }
        inProgress.remove(request)
        sensitivity[request] = sensitive
        return sensitive
    }

    private fun isCloneable(function: IrSimpleFunction): Boolean {
        var size = 0
        var hasLocalDeclarations = false
        function.body?.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) {
                ++size
                if (element is IrClass || element is IrFunction) hasLocalDeclarations = true
                if (!hasLocalDeclarations && size <= MAX_CLONED_FUNCTION_SIZE) element.acceptChildrenVoid(this)
            }
        })
        return !hasLocalDeclarations && size <= MAX_CLONED_FUNCTION_SIZE &&
                function.typeParameters.isEmpty() && !function.isSuspend && function.parent is IrClass
    }

    // The class to guard a virtual call in a clone for [request] with: its receiver is of a bound type parameter.
    private fun guardedClass(call: IrCall, request: Request): IrClass? {
        val callee = call.symbol.owner
        if (call.superQualifierSymbol != null || !callee.isOverridable || callee.dispatchReceiverParameter == null) return null
        val receiver = call.arguments.getOrNull(0) ?: return null
        val classifier = (receiver.type as? IrSimpleType)?.classifier as? IrTypeParameterSymbol ?: return null
        val bound = request.bindings[classifier] ?: return null
        // The bound class must actually implement the callee.
        if (bound.functions.none { it.overrides(callee) || it.symbol == callee.symbol }) return null
        return bound
    }

    // A member call in a clone for [request] that has a specialization itself.
    private fun innerRequest(call: IrCall, request: Request): Request? = requestFor(call, request.bindings)

    private fun IrSimpleFunction.overrides(other: IrSimpleFunction): Boolean =
            overriddenSymbols.any { it == other.symbol || it.owner.overrides(other) }

    private fun cloneFor(request: Request): IrSimpleFunction? {
        if (clones.containsKey(request)) return clones[request]
        if (cloneCount >= MAX_CLONES) return null
        val original = request.function
        val parentClass = original.parent as IrClass
        val clone = original.deepCopyWithSymbols(parentClass)
        clone.name = Name.identifier("${original.name.asString()}\$kore\$${cloneCount++}")
        clone.overriddenSymbols = emptyList()
        clone.modality = Modality.FINAL
        clone.visibility = org.jetbrains.kotlin.descriptors.DescriptorVisibilities.PRIVATE
        clone.annotations = emptyList()
        clones[request] = clone
        parentClass.declarations += clone
        specializeBody(clone, request)
        return clone
    }

    private fun specializeBody(clone: IrSimpleFunction, request: Request) {
        val body = clone.body ?: return
        body.transformChildrenVoid(object : IrElementTransformerVoid() {
            override fun visitCall(expression: IrCall): IrExpression {
                expression.transformChildrenVoid(this)
                innerRequest(expression, request)?.let { inner ->
                    cloneFor(inner)?.let { expression.symbol = it.symbol }
                    return expression
                }
                val guard = guardedClass(expression, request) ?: return expression
                return context.createIrBuilder(clone.symbol, expression.startOffset, expression.endOffset).irBlock(resultType = expression.type) {
                    val receiver = irTemporary(expression.arguments[0]!!, nameHint = "receiver")
                    val fallback = expression.deepCopyWithSymbols(clone).also { it.arguments[0] = irGet(receiver) }
                    expression.arguments[0] = irImplicitCast(irGet(receiver), guard.defaultType)
                    +irIfThenElse(expression.type, irIs(irGet(receiver), guard.defaultType), expression, fallback)
                }
            }
        })
    }

    private companion object {
        const val MAX_CLONES = 4000
        const val MAX_CLONED_FUNCTION_SIZE = 1500
    }
}
