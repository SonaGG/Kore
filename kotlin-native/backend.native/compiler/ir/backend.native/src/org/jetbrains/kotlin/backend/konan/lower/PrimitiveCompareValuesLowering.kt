/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package org.jetbrains.kotlin.backend.konan.lower

import org.jetbrains.kotlin.backend.common.FileLoweringPass
import org.jetbrains.kotlin.backend.konan.Context
import org.jetbrains.kotlin.ir.declarations.IrFile
import org.jetbrains.kotlin.ir.expressions.IrCall
import org.jetbrains.kotlin.ir.expressions.IrExpression
import org.jetbrains.kotlin.ir.expressions.impl.IrCallImpl
import org.jetbrains.kotlin.ir.types.*
import org.jetbrains.kotlin.ir.util.functions
import org.jetbrains.kotlin.ir.util.parentAsClass
import org.jetbrains.kotlin.ir.util.fqNameWhenAvailable
import org.jetbrains.kotlin.ir.visitors.IrElementTransformerVoid
import org.jetbrains.kotlin.ir.visitors.transformChildrenVoid
import org.jetbrains.kotlin.name.FqName

/**
 * Kore: replaces `kotlin.comparisons.compareValues(a, b)` on two non-null values of the same integral primitive type
 * with the primitive comparison `a.compareTo(b)`.
 *
 * `compareBy { it.x }`, `sortedBy { it.x }` and the like are inlined down to such calls. Otherwise both values are
 * boxed on every comparison (millions of allocations for a sort), which a JIT removes by escape analysis. The result
 * is the same: `compareValues` calls `compareTo` of the boxes, which compares the values. Floating-point types are left
 * alone: their boxed and primitive comparisons may differ.
 */
internal class PrimitiveCompareValuesLowering(val context: Context) : FileLoweringPass {
    private val compareValuesName = FqName("kotlin.comparisons.compareValues")

    private val compareToByType by lazy {
        val builtIns = context.irBuiltIns
        listOf(builtIns.intClass, builtIns.longClass, builtIns.shortClass, builtIns.byteClass, builtIns.charClass).associate { classSymbol ->
            val type = classSymbol.defaultType
            classSymbol to classSymbol.owner.functions.single { function ->
                function.name.asString() == "compareTo" && function.parameters.size == 2 && function.parameters[1].type == type
            }
        }
    }

    override fun lower(irFile: IrFile) {
        irFile.transformChildrenVoid(object : IrElementTransformerVoid() {
            override fun visitCall(expression: IrCall): IrExpression {
                expression.transformChildrenVoid(this)
                val callee = expression.symbol.owner
                if (callee.name.asString() != "compareValues" || callee.parameters.size != 2) return expression
                if (callee.fqNameWhenAvailable != compareValuesName) return expression
                val a = expression.arguments[0] ?: return expression
                val b = expression.arguments[1] ?: return expression
                if (a.type.isMarkedNullable() || b.type.isMarkedNullable()) return expression
                val classSymbol = a.type.classOrNull ?: return expression
                if (b.type.classOrNull != classSymbol) return expression
                val compareTo = compareToByType[classSymbol] ?: return expression
                return IrCallImpl(expression.startOffset, expression.endOffset, context.irBuiltIns.intType, compareTo.symbol).apply {
                    arguments[0] = a
                    arguments[1] = b
                }
            }
        })
    }
}
