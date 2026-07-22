package com.notap.looper.ui.views

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.view.View
import com.notap.looper.ui.Design
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.sin

/**
 * Programmatically drawn interface icons.
 *
 * WHY THIS EXISTS: the app used emoji as functional controls (🔊 🎚 ✨ 👑 🗑 ⏸ ▶).
 * Emoji are rendered by the system font in their OWN colours and **ignore
 * setTextColor entirely**, so a control's tint could never carry its state —
 * every lit/unlit signal had to be faked with background and alpha, and the
 * glyphs themselves changed shape between OEM font packs. These are real vector
 * paths that honour [tint], scale with the view, and look identical everywhere.
 *
 * EVERY SHAPE HERE WAS RENDERED AND LOOKED AT at 48/24/15px before it shipped
 * (tooling: cpp-adjacent scratch renderer mirroring this exact geometry). The
 * first cut was drawn blind and four icons were wrong in ways no build catches:
 * FADE was a right-pointing wedge indistinguishable from PLAY — and the two sit
 * in the SAME transport row; GEAR read as a sun because thin radial spokes round
 * a circle is not a gear; CROWN was an outlined zigzag that looked like a jagged
 * "M"; UNDO's arrowhead was a corner bracket that vanished into a broken circle
 * below ~20px. If you change geometry here, render it and look at it.
 *
 * Drawn with the same programmatic-View philosophy as the rest of the UI (no
 * XML drawables, no asset pipeline). Paths are rebuilt only on size change, and
 * onDraw allocates nothing.
 */
class IconView(context: Context) : View(context) {

    enum class Kind {
        PLAY, PAUSE, STOP, MUTE,
        VOLUME_HIGH, VOLUME_LOW, VOLUME_OFF,
        METRONOME, SPARKLE, SLIDERS, CROWN, TRASH, LOCK,
        UNDO, GEAR, MAIL, BULB, CLOSE, DOWNLOAD, UPLOAD, FADE, HEADPHONES, SOLO
    }

    var kind: Kind = Kind.PLAY
        set(v) { field = v; rebuild(); invalidate() }

    var tint: Int = Design.textHi
        set(v) { field = v; invalidate() }

    /** Stroke weight as a fraction of the icon box. */
    var weight: Float = 0.092f
        set(v) { field = v; rebuild(); invalidate() }

    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }

    // `path` is the primary shape, `path2` the secondary; each is painted either
    // filled or stroked according to the flags set in rebuild(). Keeping the
    // decision next to the geometry (instead of in a second when-block inside
    // onDraw) is what stops the two drifting apart as icons get edited.
    private val path = Path()
    private val path2 = Path()
    private var fillPrimary = false
    private var fillSecondary = false

    private val rect = RectF()
    private var s = 0f          // icon box size (square, centred)
    private var ox = 0f
    private var oy = 0f

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
        s = minOf(w, h) * 0.78f
        ox = (w - s) / 2f
        oy = (h - s) / 2f
        rebuild()
    }

    // Icon-box coordinates: x/y in 0..1 mapped into the centred square.
    private fun px(x: Float) = ox + x * s
    private fun py(y: Float) = oy + y * s

    private fun rectOf(l: Float, t: Float, r: Float, b: Float): RectF {
        rect.set(px(l), py(t), px(r), py(b)); return rect
    }

    /** Filled triangle at (x,y) aimed along (dx,dy), in icon-box units. */
    private fun Path.arrowHead(x: Float, y: Float, dx: Float, dy: Float, size: Float) {
        val m = hypot(dx, dy).takeIf { it > 1e-6f } ?: 1f
        val ux = dx / m; val uy = dy / m
        val nx = -uy; val ny = ux
        moveTo(px(x + ux * size), py(y + uy * size))
        lineTo(px(x - ux * size * 0.35f + nx * size * 0.8f),
               py(y - uy * size * 0.35f + ny * size * 0.8f))
        lineTo(px(x - ux * size * 0.35f - nx * size * 0.8f),
               py(y - uy * size * 0.35f - ny * size * 0.8f))
        close()
    }

    private fun rad(deg: Float) = Math.toRadians(deg.toDouble())

    private fun rebuild() {
        if (s <= 0f) return
        stroke.strokeWidth = s * weight
        path.reset(); path2.reset()
        path.fillType = Path.FillType.WINDING
        fillPrimary = false
        fillSecondary = false

        when (kind) {
            Kind.PLAY -> {
                fillPrimary = true
                path.moveTo(px(0.26f), py(0.14f))
                path.lineTo(px(0.84f), py(0.5f))
                path.lineTo(px(0.26f), py(0.86f))
                path.close()
            }
            Kind.PAUSE -> {
                fillPrimary = true
                path.addRoundRect(rectOf(0.26f, 0.15f, 0.41f, 0.85f),
                                  s * 0.05f, s * 0.05f, Path.Direction.CW)
                path.addRoundRect(rectOf(0.59f, 0.15f, 0.74f, 0.85f),
                                  s * 0.05f, s * 0.05f, Path.Direction.CW)
            }
            Kind.STOP -> {
                fillPrimary = true
                path.addRoundRect(rectOf(0.22f, 0.22f, 0.78f, 0.78f),
                                  s * 0.08f, s * 0.08f, Path.Direction.CW)
            }

            // Speaker cone shared by the volume family; only the arcs differ.
            Kind.VOLUME_HIGH, Kind.VOLUME_LOW, Kind.VOLUME_OFF, Kind.MUTE, Kind.METRONOME -> {
                fillPrimary = true
                path.moveTo(px(0.10f), py(0.36f))
                path.lineTo(px(0.26f), py(0.36f))
                path.lineTo(px(0.46f), py(0.16f))
                path.lineTo(px(0.46f), py(0.84f))
                path.lineTo(px(0.26f), py(0.64f))
                path.lineTo(px(0.10f), py(0.64f))
                path.close()
                when (kind) {
                    Kind.VOLUME_HIGH, Kind.METRONOME -> {
                        path2.addArc(rectOf(0.34f, 0.24f, 0.78f, 0.76f), -55f, 110f)
                        path2.addArc(rectOf(0.28f, 0.12f, 0.92f, 0.88f), -52f, 104f)
                    }
                    Kind.VOLUME_LOW -> path2.addArc(rectOf(0.34f, 0.24f, 0.78f, 0.76f), -55f, 110f)
                    else -> {   // VOLUME_OFF / MUTE — a struck-through cone
                        path2.moveTo(px(0.60f), py(0.36f)); path2.lineTo(px(0.88f), py(0.64f))
                        path2.moveTo(px(0.88f), py(0.36f)); path2.lineTo(px(0.60f), py(0.64f))
                    }
                }
            }

            Kind.SPARKLE -> {
                // Four-point star + companion. The waist must be TIGHT (0.028):
                // the first version used 0.07 and rendered as a rounded diamond.
                fillPrimary = true
                fun star(cx: Float, cy: Float, r: Float, w: Float) {
                    path.moveTo(px(cx), py(cy - r))
                    path.quadTo(px(cx + w), py(cy - w), px(cx + r), py(cy))
                    path.quadTo(px(cx + w), py(cy + w), px(cx), py(cy + r))
                    path.quadTo(px(cx - w), py(cy + w), px(cx - r), py(cy))
                    path.quadTo(px(cx - w), py(cy - w), px(cx), py(cy - r))
                    path.close()
                }
                star(0.40f, 0.42f, 0.38f, 0.028f)
                star(0.79f, 0.79f, 0.19f, 0.016f)
            }

            Kind.SLIDERS -> {
                fillSecondary = true
                val ys = floatArrayOf(0.24f, 0.5f, 0.76f)
                val hx = floatArrayOf(0.66f, 0.36f, 0.56f)
                for (i in 0..2) {
                    path.moveTo(px(0.12f), py(ys[i])); path.lineTo(px(0.88f), py(ys[i]))
                    path2.addCircle(px(hx[i]), py(ys[i]), s * 0.085f, Path.Direction.CW)
                }
            }

            Kind.CROWN -> {
                // FILLED, with a solid base band. The outlined zigzag it replaced
                // read as a jagged "M" and turned to mush below 20px.
                fillPrimary = true
                path.moveTo(px(0.09f), py(0.30f))
                path.lineTo(px(0.28f), py(0.56f))
                path.lineTo(px(0.50f), py(0.20f))
                path.lineTo(px(0.72f), py(0.56f))
                path.lineTo(px(0.91f), py(0.30f))
                path.lineTo(px(0.85f), py(0.82f))
                path.lineTo(px(0.15f), py(0.82f))
                path.close()
            }

            Kind.TRASH -> {
                path.moveTo(px(0.14f), py(0.28f)); path.lineTo(px(0.86f), py(0.28f))
                path.moveTo(px(0.38f), py(0.28f)); path.lineTo(px(0.40f), py(0.16f))
                path.lineTo(px(0.60f), py(0.16f)); path.lineTo(px(0.62f), py(0.28f))
                path.moveTo(px(0.22f), py(0.28f)); path.lineTo(px(0.28f), py(0.86f))
                path.lineTo(px(0.72f), py(0.86f)); path.lineTo(px(0.78f), py(0.28f))
                path2.moveTo(px(0.42f), py(0.42f)); path2.lineTo(px(0.44f), py(0.72f))
                path2.moveTo(px(0.58f), py(0.42f)); path2.lineTo(px(0.56f), py(0.72f))
            }

            Kind.LOCK -> {
                path.addRoundRect(rectOf(0.19f, 0.45f, 0.81f, 0.87f),
                                  s * 0.10f, s * 0.10f, Path.Direction.CW)
                path2.addArc(rectOf(0.32f, 0.15f, 0.68f, 0.51f), 180f, 180f)
            }

            Kind.UNDO -> {
                // Arc stops short of the head so the two don't pile up; the head
                // sits at the arc's true end, aimed along its tangent.
                fillSecondary = true
                path.addArc(rectOf(0.24f, 0.26f, 0.84f, 0.86f), 200f, 232f)
                val cx = 0.54f; val cy = 0.56f; val r = 0.30f
                val e = rad(200f + 250f)
                path2.arrowHead(
                    cx + r * cos(e).toFloat(), cy + r * sin(e).toFloat(),
                    -sin(e).toFloat(), cos(e).toFloat(), 0.165f
                )
            }

            Kind.GEAR -> {
                // A real toothed ring with an even-odd hole. Radial spokes around
                // a circle (the first attempt) read unmistakably as a SUN.
                fillPrimary = true
                path.fillType = Path.FillType.EVEN_ODD
                val teeth = 8
                val step = 360f / teeth
                val rOut = 0.47f; val rIn = 0.345f
                val tw = 13f; val bevel = 6f
                for (i in 0 until teeth) {
                    val a = i * step
                    val pts = arrayOf(
                        rIn to (a - step / 2f + bevel), rOut to (a - tw),
                        rOut to (a + tw), rIn to (a + step / 2f - bevel)
                    )
                    for ((idx, p) in pts.withIndex()) {
                        val (r, ang) = p
                        val x = 0.5f + r * cos(rad(ang)).toFloat()
                        val y = 0.5f + r * sin(rad(ang)).toFloat()
                        if (i == 0 && idx == 0) path.moveTo(px(x), py(y)) else path.lineTo(px(x), py(y))
                    }
                }
                path.close()
                path.addCircle(px(0.5f), py(0.5f), s * 0.155f, Path.Direction.CCW)
            }

            Kind.MAIL -> {
                path.addRoundRect(rectOf(0.10f, 0.24f, 0.90f, 0.76f),
                                  s * 0.07f, s * 0.07f, Path.Direction.CW)
                path2.moveTo(px(0.10f), py(0.28f))
                path2.lineTo(px(0.50f), py(0.56f))
                path2.lineTo(px(0.90f), py(0.28f))
            }

            Kind.BULB -> {
                // Round glass + a clean screw base; the old teardrop-and-legs
                // silhouette collapsed into a blob at small sizes.
                path.addCircle(px(0.5f), py(0.38f), s * 0.27f, Path.Direction.CW)
                path2.moveTo(px(0.38f), py(0.65f)); path2.lineTo(px(0.38f), py(0.76f))
                path2.moveTo(px(0.62f), py(0.65f)); path2.lineTo(px(0.62f), py(0.76f))
                path2.moveTo(px(0.38f), py(0.72f)); path2.lineTo(px(0.62f), py(0.72f))
                path2.moveTo(px(0.42f), py(0.85f)); path2.lineTo(px(0.58f), py(0.85f))
            }

            Kind.CLOSE -> {
                path.moveTo(px(0.20f), py(0.20f)); path.lineTo(px(0.80f), py(0.80f))
                path.moveTo(px(0.80f), py(0.20f)); path.lineTo(px(0.20f), py(0.80f))
            }

            Kind.DOWNLOAD, Kind.UPLOAD -> {
                val down = kind == Kind.DOWNLOAD
                val tail = if (down) 0.14f else 0.60f
                val head = if (down) 0.60f else 0.14f
                path.moveTo(px(0.5f), py(tail)); path.lineTo(px(0.5f), py(head))
                path.moveTo(px(0.30f), py(if (down) 0.40f else 0.34f))
                path.lineTo(px(0.5f), py(head)); path.lineTo(px(0.70f), py(if (down) 0.40f else 0.34f))
                path2.moveTo(px(0.16f), py(0.72f)); path2.lineTo(px(0.16f), py(0.86f))
                path2.lineTo(px(0.84f), py(0.86f)); path2.lineTo(px(0.84f), py(0.72f))
            }

            Kind.FADE -> {
                // Descending bars — level running out. The wedge this replaced was
                // a right-pointing triangle, i.e. visually PLAY, and FADE sits in
                // the same transport row as PLAY. Unambiguous beats elegant.
                fillPrimary = true
                val tops = floatArrayOf(0.14f, 0.27f, 0.40f, 0.55f, 0.70f)
                for (i in 0..4) {
                    val l = 0.08f + i * 0.18f
                    path.addRoundRect(rectOf(l, tops[i], l + 0.12f, 0.86f),
                                      s * 0.045f, s * 0.045f, Path.Direction.CW)
                }
            }

            Kind.HEADPHONES -> {
                path.addArc(rectOf(0.14f, 0.16f, 0.86f, 0.80f), 180f, 180f)
                path2.addRoundRect(rectOf(0.10f, 0.46f, 0.30f, 0.84f),
                                   s * 0.07f, s * 0.07f, Path.Direction.CW)
                path2.addRoundRect(rectOf(0.70f, 0.46f, 0.90f, 0.84f),
                                   s * 0.07f, s * 0.07f, Path.Direction.CW)
            }

            Kind.SOLO -> {
                fillPrimary = true
                path.addRoundRect(rectOf(0.42f, 0.14f, 0.58f, 0.86f),
                                  s * 0.05f, s * 0.05f, Path.Direction.CW)
                path2.addRoundRect(rectOf(0.12f, 0.34f, 0.28f, 0.66f),
                                   s * 0.05f, s * 0.05f, Path.Direction.CW)
                path2.addRoundRect(rectOf(0.72f, 0.34f, 0.88f, 0.66f),
                                   s * 0.05f, s * 0.05f, Path.Direction.CW)
            }
        }
    }

    override fun onDraw(canvas: Canvas) {
        if (s <= 0f) return
        stroke.color = tint
        fill.color = tint
        canvas.drawPath(path, if (fillPrimary) fill else stroke)
        if (!path2.isEmpty) canvas.drawPath(path2, if (fillSecondary) fill else stroke)
    }
}
