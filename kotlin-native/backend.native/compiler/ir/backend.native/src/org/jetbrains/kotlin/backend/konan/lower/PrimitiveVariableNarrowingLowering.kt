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
import org.jetbrains.kotlin.ir.expressions.IrExpression
import org.jetbrains.kotlin.ir.expressions.IrGetValue
import org.jetbrains.kotlin.ir.expressions.IrSetValue
import org.jetbrains.kotlin.ir.types.IrType
import org.jetbrains.kotlin.ir.types.classOrNull
import org.jetbrains.kotlin.ir.types.isMarkedNullable
import org.jetbrains.kotlin.ir.visitors.IrVisitorVoid
import org.jetbrains.kotlin.ir.visitors.acceptChildrenVoid
import org.jetbrains.kotlin.ir.visitors.acceptVoid

/**
 * Kore: gives a local variable a primitive type when every value assigned to it has that type.
 *
 * Inline generic functions such as `fold`, `reduce` or `runningFold` keep their accumulators in variables of the
 * (erased) type parameter, so after inlining `intArray.fold(0L) { acc, v -> acc + v }` boxes the `Long` accumulator on
 * every iteration, which a JIT removes by escape analysis. Every value stored in such a variable is a `Long` here, so the
 * variable can hold the primitive itself: reads that need an object box it there (as for any primitive variable).
 */
internal class PrimitiveVariableNarrowingLowering(val context: Context) : FileLoweringPass {
    private val primitiveClasses by lazy {
        val builtIns = context.irBuiltIns
        setOf(builtIns.intClass, builtIns.longClass, builtIns.doubleClass, builtIns.floatClass, builtIns.shortClass,
                builtIns.byteClass, builtIns.charClass, builtIns.booleanClass)
    }

    private fun primitiveTypeOf(type: IrType): IrType? =
            type.takeIf { !it.isMarkedNullable() && it.classOrNull in primitiveClasses }

    override fun lower(irFile: IrFile) {
        // The primitive type of every value stored into each variable, or null when one of them is not that primitive.
        val storedTypes = mutableMapOf<IrVariable, IrType?>()
        val gets = mutableListOf<IrGetValue>()

        fun record(variable: IrVariable, value: IrExpression) {
            val type = primitiveTypeOf(value.type)
            if (!storedTypes.containsKey(variable)) {
                storedTypes[variable] = type
            } else {
                val previous = storedTypes[variable]
                if (previous == null || type == null || previous.classOrNull != type.classOrNull) storedTypes[variable] = null
            }
        }

        irFile.acceptVoid(object : IrVisitorVoid() {
            override fun visitElement(element: IrElement) = element.acceptChildrenVoid(this)

            override fun visitVariable(declaration: IrVariable) {
                declaration.acceptChildrenVoid(this)
                if (primitiveTypeOf(declaration.type) != null) return // Already primitive.
                // Without an initializer, the assignments decide (and a variable that is never assigned is left alone).
                declaration.initializer?.let { record(declaration, it) }
            }

            override fun visitSetValue(expression: IrSetValue) {
                expression.acceptChildrenVoid(this)
                val variable = expression.symbol.owner as? IrVariable ?: return
                if (primitiveTypeOf(variable.type) != null) return
                record(variable, expression.value)
            }

            override fun visitGetValue(expression: IrGetValue) {
                if (expression.symbol.owner is IrVariable) gets.add(expression)
            }
        })

        val narrowed = storedTypes.filterValues { it != null }
        if (narrowed.isEmpty()) return
        for (entry in narrowed) entry.key.type = entry.value!!
        for (get in gets) {
            val type = narrowed[get.symbol.owner as IrVariable] ?: continue
            get.type = type
        }
    }
}
