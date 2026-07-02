package com.notap.looper.ui.views

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.view.View

class VuMeterView(context: Context) : View(context) {
    var level: Float = 0f
        set(value) {
            field = value.coerceIn(0f, 1f)
            invalidate()
        }

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val rect = RectF()
    private val blockCount = 14
    private val blockSpacing = 6f

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val blockHeight = (height - (blockSpacing * (blockCount - 1))) / blockCount
        val activeBlocks = (level * blockCount).toInt()

        for (i in 0 until blockCount) {
            val isLit = (blockCount - 1 - i) < activeBlocks
            rect.left = 0f
            rect.right = width.toFloat()
            rect.top = i * (blockHeight + blockSpacing)
            rect.bottom = rect.top + blockHeight

            paint.color = when {
                !isLit -> Color.parseColor("#15171A") // LED כבוי
                i < 2 -> Color.parseColor("#FF1744")  // Clipping (אדום)
                i < 5 -> Color.parseColor("#FFD600")  // Warning (צהוב)
                else -> Color.parseColor("#00E676")   // Signal (ירוק)
            }

            if (isLit) {
                paint.setShadowLayer(12f, 0f, 0f, paint.color)
            } else {
                paint.clearShadowLayer()
            }

            canvas.drawRoundRect(rect, 4f, 4f, paint)
        }
    }
}