/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.backend.konan.lower

import org.jetbrains.kotlin.backend.common.FileLoweringPass
import org.jetbrains.kotlin.backend.konan.Context
import org.jetbrains.kotlin.ir.IrElement
import org.jetbrains.kotlin.ir.declarations.IrFile
import org.jetbrains.kotlin.ir.declarations.IrVariable
import org.jetbrains.kotlin.ir.expressions.*
import org.jetbrains.kotlin.ir.expressions.impl.IrConstImpl
import org.jetbrains.kotlin.ir.expressions.impl.IrConstructorCallImpl
import org.jetbrains.kotlin.ir.types.classOrNull
import org.jetbrains.kotlin.ir.util.constructors
import org.jetbrains.kotlin.ir.visitors.IrVisitorVoid
import org.jetbrains.kotlin.ir.visitors.acceptChildrenVoid
import org.jetbrains.kotlin.ir.visitors.acceptVoid

/**
 * Kore: sizes the `StringBuilder` of a string template (as built by the string concatenation lowering:
 * `val tmp = StringBuilder(); tmp.append(...)...; tmp.toString()`) for the whole result: the lengths of the constant
 * parts plus an estimate per argument. The builder otherwise starts with 16 characters and grows (copying) as the parts
 * are appended. The arguments are still evaluated in the same order; only the initial capacity changes.
 */
internal class StringConcatenationPresizing(private val context: Context) : FileLoweringPass {
    private val irBuiltIns = context.irBuiltIns
    private val stringBuilder by lazy { context.symbols.stringBuilder.owner }
    private val defaultConstructor by lazy { stringBuilder.constructors.single { it.parameters.isEmpty() } }
    private val capacityConstructor by lazy {
        stringBuilder.constructors.single { constructor ->
            constructor.parameters.size == 1 && constructor.parameters[0].type.classOrNull == irBuiltIns.intClass
        }
    }

    override fun lower(irFile: IrFile) {
        irFile.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
            override fun visitBlock(expression: IrBlock) {
                expression.acceptChildrenVoid(this)
                presize(expression)
            }
        })
    }

    private fun estimate(argument: IrExpression): Int {
        if (argument is IrConst && argument.value is String) return (argument.value as String).length
        return when (argument.type.classOrNull) {
            irBuiltIns.intClass, irBuiltIns.shortClass, irBuiltIns.byteClass -> 11
            irBuiltIns.longClass -> 20
            irBuiltIns.doubleClass, irBuiltIns.floatClass -> 24
            irBuiltIns.charClass -> 1
            irBuiltIns.booleanClass -> 5
            else -> 16
        }
    }

    private fun presize(block: IrBlock) {
        val statements = block.statements
        if (statements.size < 3) return
        val builder = statements[0] as? IrVariable ?: return
        val creation = builder.initializer as? IrConstructorCall ?: return
        if (creation.symbol != defaultConstructor.symbol) return
        var capacity = 0
        for (statement in statements.subList(1, statements.size - 1)) {
            val append = statement as? IrCall ?: return
            if (append.symbol.owner.name.asString() != "append" || append.symbol.owner.parent != stringBuilder) return
            if ((append.arguments.getOrNull(0) as? IrGetValue)?.symbol != builder.symbol) return
            capacity += estimate(append.arguments.getOrNull(1) ?: return)
        }
        val last = statements.last() as? IrCall ?: return
        if (last.symbol.owner.name.asString() != "toString" || (last.arguments.getOrNull(0) as? IrGetValue)?.symbol != builder.symbol) return
        if (capacity <= 16) return
        builder.initializer = IrConstructorCallImpl(
                creation.startOffset, creation.endOffset, creation.type, capacityConstructor.symbol,
                typeArgumentsCount = 0, constructorTypeArgumentsCount = 0, origin = creation.origin,
        ).apply {
            arguments[0] = IrConstImpl.int(creation.startOffset, creation.endOffset, irBuiltIns.intType, capacity)
        }
    }
}
