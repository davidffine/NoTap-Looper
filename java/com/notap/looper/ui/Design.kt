package com.notap.looper.ui

import android.content.Context
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.util.TypedValue

/**
 * Single source of truth for the dark/neon design language.
 * Centralised so every view speaks the same visual dialect.
 */
object Design {

    // --- Surfaces ---------------------------------------------------------
    val bgTop      = Color.parseColor("#0B0E15")   // radial glow centre
    val bgBottom   = Color.parseColor("#04050A")   // deep void edge
    val surface    = Color.parseColor("#111624")   // glass card
    val surfaceHi  = Color.parseColor("#18202F")   // raised glass
    val orbBase    = Color.parseColor("#0A0E17")   // deep sphere base (parsed ONCE, not per-frame)
    val stroke     = Color.parseColor("#33405C")   // hairline border (nudged brighter for camera legibility)
    val strokeSoft = Color.parseColor("#1A2234")   // inactive track

    // --- Text -------------------------------------------------------------
    val textHi  = Color.parseColor("#EAF0FB")
    val textMid = Color.parseColor("#8E9CB6")
    val textLo  = Color.parseColor("#586277")

    // --- State accents ----------------------------------------------------
    val cyan   = Color.parseColor("#25D5F0")   // ready / idle
    val red    = Color.parseColor("#FF3B60")   // recording
    val green  = Color.parseColor("#34E29B")   // looping
    val amber  = Color.parseColor("#FFB020")   // overdubbing
    val violet = Color.parseColor("#9B8CFF")   // processing
    val slate  = Color.parseColor("#5C6B84")   // calibrating

    // --- Spatial rhythm (8dp base grid, Material-3 aligned) ---------------
    // One scale so padding/margins stop being scattered magic numbers.
    object Space { const val xs = 4; const val sm = 8; const val md = 12; const val lg = 16; const val xl = 24; const val xxl = 32 }

    // --- Type scale (sp) --------------------------------------------------
    object Type { const val hero = 40f; const val title = 17f; const val body = 13f; const val label = 11f; const val micro = 9f }

    // Premium-cue glow strength (dp) — one knob for all crown/accent shadows.
    const val glowDp = 6f

    // --- Helpers ----------------------------------------------------------
    fun dp(v: Number, ctx: Context): Int =
        TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP, v.toFloat(), ctx.resources.displayMetrics
        ).toInt()

    fun dpf(v: Number, ctx: Context): Float =
        TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP, v.toFloat(), ctx.resources.displayMetrics
        )

    /** Same color with a new alpha (0..1). */
    fun alpha(color: Int, a: Float): Int {
        val al = (a.coerceIn(0f, 1f) * 255f).toInt()
        return (color and 0x00FFFFFF) or (al shl 24)
    }

    /** Linear blend a→b (t 0..1). */
    fun mix(a: Int, b: Int, t: Float): Int {
        val u = t.coerceIn(0f, 1f)
        val ar = (a shr 16) and 0xFF; val ag = (a shr 8) and 0xFF; val ab = a and 0xFF
        val br = (b shr 16) and 0xFF; val bg = (b shr 8) and 0xFF; val bb = b and 0xFF
        return Color.rgb(
            (ar + (br - ar) * u).toInt(),
            (ag + (bg - ag) * u).toInt(),
            (ab + (bb - ab) * u).toInt()
        )
    }

    /** Rounded glass panel with hairline stroke. */
    fun glass(ctx: Context, radiusDp: Float, fill: Int = surface, border: Int = stroke): GradientDrawable =
        GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = dpf(radiusDp, ctx)
            colors = intArrayOf(surfaceHi, fill)
            gradientType = GradientDrawable.LINEAR_GRADIENT
            orientation = GradientDrawable.Orientation.TOP_BOTTOM
            setStroke(dp(1, ctx), border)
        }

    /** Pill background. */
    fun pill(ctx: Context, fill: Int, border: Int): GradientDrawable =
        GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = dpf(100, ctx)
            setColor(fill)
            setStroke(dp(1, ctx), border)
        }
}
