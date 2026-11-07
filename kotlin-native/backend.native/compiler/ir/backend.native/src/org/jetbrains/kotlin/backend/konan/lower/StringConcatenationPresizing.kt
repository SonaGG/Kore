/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.backend.konan.lower

import org.jetbrains.kotlin.backend.common.FileLoweringPass
import org.jetbrains.kotlin.backend.common.lower.createIrBuilder
import org.jetbrains.kotlin.backend.konan.Context
import org.jetbrains.kotlin.ir.IrElement
import org.jetbrains.kotlin.ir.builders.*
import org.jetbrains.kotlin.ir.declarations.IrFile
import org.jetbrains.kotlin.ir.declarations.IrFunction
import org.jetbrains.kotlin.ir.declarations.IrVariable
import org.jetbrains.kotlin.ir.expressions.*
import org.jetbrains.kotlin.ir.expressions.impl.IrConstImpl
import org.jetbrains.kotlin.ir.expressions.impl.IrConstructorCallImpl
import org.jetbrains.kotlin.ir.types.classOrNull
import org.jetbrains.kotlin.ir.types.isMarkedNullable
import org.jetbrains.kotlin.ir.util.constructors
import org.jetbrains.kotlin.ir.util.deepCopyWithSymbols
import org.jetbrains.kotlin.ir.util.functions
import org.jetbrains.kotlin.ir.util.properties
import org.jetbrains.kotlin.ir.visitors.IrVisitorVoid
import org.jetbrains.kotlin.ir.visitors.acceptChildrenVoid
import org.jetbrains.kotlin.ir.visitors.acceptVoid

/**
 * Kore: string templates, as built by the string concatenation lowering
 * (`val tmp = StringBuilder(); tmp.append(...)...; tmp.toString()`).
 *
 * When every part is a `String`, an `Int`, a `Long` or a `Char`, the template is built in place instead (as the JVM's
 * `StringConcatFactory` does): the parts are evaluated in order into temporaries, measured, one string of the total
 * length is allocated, and the parts are written into it. One allocation and one copy of each part, instead of the
 * builder's array, its growth, and the copy into the result.
 *
 * Otherwise the `StringBuilder` is sized for the whole result: the lengths of the constant parts plus an estimate per
 * argument. The builder otherwise starts with 16 characters and grows (copying) as the parts are appended.
 */
internal class StringConcatenationPresizing(private val context: Context) : FileLoweringPass {
    private val irBuiltIns = context.irBuiltIns
    private val symbols = context.symbols
    private val stringBuilder by lazy { symbols.stringBuilder.owner }
    private val defaultConstructor by lazy { stringBuilder.constructors.single { it.parameters.isEmpty() } }
    private val capacityConstructor by lazy {
        stringBuilder.constructors.single { constructor ->
            constructor.parameters.size == 1 && constructor.parameters[0].type.classOrNull == irBuiltIns.intClass
        }
    }
    private val stringLength by lazy { irBuiltIns.stringClass.owner.properties.single { it.name.asString() == "length" }.getter!!.symbol }
    private val intToLong by lazy { irBuiltIns.intClass.owner.functions.single { it.name.asString() == "toLong" && it.parameters.size == 1 }.symbol }
    private val intPlus by lazy {
        irBuiltIns.intClass.owner.functions.single {
            it.name.asString() == "plus" && it.parameters.size == 2 && it.parameters[1].type.classOrNull == irBuiltIns.intClass
        }.symbol
    }

    override fun lower(irFile: IrFile) {
        val functions = ArrayList<IrFunction>()
        irFile.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)
            override fun visitFunction(declaration: IrFunction) {
                functions.add(declaration)
                declaration.acceptChildrenVoid(this)
                functions.removeAt(functions.lastIndex)
            }
            override fun visitBlock(expression: IrBlock) {
                expression.acceptChildrenVoid(this)
                transform(expression, functions.lastOrNull())
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

    private enum class Kind { STRING, INT, LONG, CHAR }

    private class Part(val argument: IrExpression, val kind: Kind)

    private fun kindOf(append: IrCall, argument: IrExpression): Kind? {
        val parameter = append.symbol.owner.parameters.getOrNull(1)?.type ?: return null
        return when (parameter.classOrNull) {
            irBuiltIns.stringClass -> if (argument.type.classOrNull == irBuiltIns.stringClass && !argument.type.isMarkedNullable()) Kind.STRING else null
            irBuiltIns.intClass -> if (argument.type.classOrNull == irBuiltIns.intClass) Kind.INT else null
            irBuiltIns.longClass -> if (argument.type.classOrNull == irBuiltIns.longClass) Kind.LONG else null
            irBuiltIns.charClass -> if (argument.type.classOrNull == irBuiltIns.charClass) Kind.CHAR else null
            else -> null
        }
    }

    // `x.toString()` of an Int, Long or Char (as the type narrowing writes appended primitives): `x` itself.
    private fun part(append: IrCall, argument: IrExpression): Part? {
        val kind = kindOf(append, argument) ?: return null
        if (kind == Kind.STRING && argument is IrCall && argument.symbol.owner.name.asString() == "toString" && argument.arguments.size == 1) {
            val receiver = argument.arguments[0]
            val owner = argument.symbol.owner.parent
            if (receiver != null && !receiver.type.isMarkedNullable()) {
                when {
                    owner == irBuiltIns.intClass.owner && receiver.type.classOrNull == irBuiltIns.intClass -> return Part(receiver, Kind.INT)
                    owner == irBuiltIns.longClass.owner && receiver.type.classOrNull == irBuiltIns.longClass -> return Part(receiver, Kind.LONG)
                    owner == irBuiltIns.charClass.owner && receiver.type.classOrNull == irBuiltIns.charClass -> return Part(receiver, Kind.CHAR)
                }
            }
        }
        return Part(argument, kind)
    }

    private fun transform(block: IrBlock, function: IrFunction?) {
        val statements = block.statements
        if (statements.size < 3) return
        val builder = statements[0] as? IrVariable ?: return
        val creation = builder.initializer as? IrConstructorCall ?: return
        if (creation.symbol != defaultConstructor.symbol) return
        val appends = ArrayList<IrCall>()
        for (statement in statements.subList(1, statements.size - 1)) {
            val append = statement as? IrCall ?: return
            if (append.symbol.owner.name.asString() != "append" || append.symbol.owner.parent != stringBuilder) return
            if ((append.arguments.getOrNull(0) as? IrGetValue)?.symbol != builder.symbol) return
            if (append.arguments.size != 2 || append.arguments[1] == null) return
            appends += append
        }
        val last = statements.last() as? IrCall ?: return
        if (last.symbol.owner.name.asString() != "toString" || (last.arguments.getOrNull(0) as? IrGetValue)?.symbol != builder.symbol) return

        if (function != null && !context.config.latin1Strings) {
            val parts = appends.mapNotNull { append -> part(append, append.arguments[1]!!) }
            if (parts.size == appends.size) {
                buildInPlace(block, function, parts)
                return
            }
        }

        val capacity = appends.sumOf { estimate(it.arguments[1]!!) }
        if (capacity <= 16) return
        builder.initializer = IrConstructorCallImpl(
                creation.startOffset, creation.endOffset, creation.type, capacityConstructor.symbol,
                typeArgumentsCount = 0, constructorTypeArgumentsCount = 0, origin = creation.origin,
        ).apply {
            arguments[0] = IrConstImpl.int(creation.startOffset, creation.endOffset, irBuiltIns.intType, capacity)
        }
    }

    private fun buildInPlace(block: IrBlock, function: IrFunction, parts: List<Part>) {
        val builder = context.createIrBuilder(function.symbol, block.startOffset, block.endOffset)
        val built = builder.irBlock(resultType = irBuiltIns.stringType) {
            // The parts in order (constants need no temporary), then their lengths.
            var constantLength = 0
            val values = parts.map { part ->
                val argument = part.argument
                when {
                    argument is IrConst && (argument.value is String || argument.value is Char) -> {
                        constantLength += (argument.value as? String)?.length ?: 1
                        null
                    }
                    part.kind == Kind.CHAR -> {
                        constantLength += 1
                        irTemporary(argument, nameHint = "templatePart")
                    }
                    else -> irTemporary(argument, nameHint = "templatePart")
                }
            }
            var length: IrExpression = irInt(constantLength)
            parts.forEachIndexed { index, part ->
                val value = values[index] ?: return@forEachIndexed
                val partLength = when (part.kind) {
                    Kind.STRING -> irCall(stringLength).apply { arguments[0] = irGet(value) }
                    Kind.INT -> irCall(symbols.stringTemplateLongLength).apply {
                        arguments[0] = irCall(intToLong).apply { arguments[0] = irGet(value) }
                    }
                    Kind.LONG -> irCall(symbols.stringTemplateLongLength).apply { arguments[0] = irGet(value) }
                    Kind.CHAR -> return@forEachIndexed
                }
                length = irCall(intPlus).apply {
                    arguments[0] = length
                    arguments[1] = partLength
                }
            }
            val result = irTemporary(irCall(symbols.stringTemplateAllocate).apply { arguments[0] = length }, nameHint = "template")
            var position: IrExpression = irInt(0)
            parts.forEachIndexed { index, part ->
                val value: IrExpression = values[index]?.let { irGet(it) } ?: part.argument.deepCopyWithSymbols(function)
                val write = when (part.kind) {
                    Kind.STRING -> symbols.stringTemplateWriteString
                    Kind.INT, Kind.LONG -> symbols.stringTemplateWriteLong
                    Kind.CHAR -> symbols.stringTemplateWriteChar
                }
                val written = if (part.kind == Kind.INT) irCall(intToLong).apply { arguments[0] = value } else value
                position = irCall(write).apply {
                    arguments[0] = irGet(result)
                    arguments[1] = position
                    arguments[2] = written
                }
            }
            +position
            +irGet(result)
        }
        block.statements.clear()
        block.statements.addAll(built.statements)
    }
}
