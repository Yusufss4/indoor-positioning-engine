package io.github.yusufss4.ipe.example

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.View
import io.github.yusufss4.ipe.Estimate
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin

/**
 * The map of one floor with the engine's estimate on it. Drag to move, pinch to zoom, tap to say "I am
 * here" ([onTap], in the venue's metres).
 */
class MapView(context: Context) : View(context) {
    var onTap: ((x: Double, y: Double) -> Unit)? = null

    private var venue: VenueMap? = null
    private var level = 0
    private var estimate: Estimate? = null
    private val trail = ArrayList<FloatArray>()  // x, y, level index
    private val marks = ArrayList<FloatArray>()  // x, y, level index, 1 when the tap also set the position

    // Canvas px = zoom * fitted px + pan, where "fitted" shows the whole venue.
    private var zoom = 1f
    private var panX = 0f
    private var panY = 0f

    private val density = resources.displayMetrics.density
    private val wallPaint = stroke(0xFF1D4E89.toInt(), 1.5f)
    private val connectorPaint = stroke(0xFF8E44AD.toInt(), 1.5f)
    private val trailPaint = stroke(0xFF2CA02C.toInt(), 2.5f)
    private val spreadPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0x332CA02C; style = Paint.Style.FILL }
    private val dotPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF2CA02C.toInt(); style = Paint.Style.FILL }
    private val markPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF222222.toInt(); style = Paint.Style.FILL }
    private val fixPaint = stroke(0xFF1D4E89.toInt(), 2f)
    private val path = Path()

    private fun stroke(color: Int, dp: Float) = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        this.color = color
        style = Paint.Style.STROKE
        strokeWidth = dp * density
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }

    private val scaler = ScaleGestureDetector(context, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
        override fun onScale(d: ScaleGestureDetector): Boolean {
            val next = (zoom * d.scaleFactor).coerceIn(1f, 60f)
            val f = next / zoom
            panX = d.focusX - f * (d.focusX - panX)
            panY = d.focusY - f * (d.focusY - panY)
            zoom = next
            invalidate()
            return true
        }
    })

    private val gestures = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDown(e: MotionEvent) = true

        override fun onScroll(e1: MotionEvent?, e2: MotionEvent, dx: Float, dy: Float): Boolean {
            panX -= dx
            panY -= dy
            invalidate()
            return true
        }

        override fun onSingleTapUp(e: MotionEvent): Boolean {
            val v = venue ?: return false
            val s = fitScale(v) * zoom
            val x = v.bounds[0] + (e.x - originX(v)) / s
            val y = v.bounds[3] - (e.y - originY(v)) / s
            if (x in v.bounds[0]..v.bounds[2] && y in v.bounds[1]..v.bounds[3]) onTap?.invoke(x, y)
            return true
        }
    })

    init {
        setBackgroundColor(0xFFFBFCFE.toInt())
    }

    fun show(venue: VenueMap) {
        this.venue = venue
        level = 0
        zoom = 1f
        panX = 0f
        panY = 0f
        clearWalk()
    }

    fun clearWalk() {
        estimate = null
        trail.clear()
        marks.clear()
        invalidate()
    }

    fun showLevel(index: Int) {
        level = index
        invalidate()
    }

    fun update(e: Estimate) {
        estimate = e
        if (e.valid) {
            val last = trail.lastOrNull()
            val x = e.x.toFloat()
            val y = e.y.toFloat()
            if (last == null || last[2].toInt() != e.level || (last[0] - x) * (last[0] - x) + (last[1] - y) * (last[1] - y) > 0.04f) {
                if (trail.size >= 20000) trail.removeAt(0)
                trail.add(floatArrayOf(x, y, e.level.toFloat()))
            }
        }
        invalidate()
    }

    fun addMark(x: Double, y: Double, level: Int, setsPosition: Boolean) {
        marks.add(floatArrayOf(x.toFloat(), y.toFloat(), level.toFloat(), if (setsPosition) 1f else 0f))
        invalidate()
    }

    @Suppress("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        scaler.onTouchEvent(event)
        if (!scaler.isInProgress) gestures.onTouchEvent(event)
        return true
    }

    private val margin get() = 12f * density

    /** Canvas px per metre when the whole venue is shown. */
    private fun fitScale(v: VenueMap): Float =
        min((width - 2 * margin) / (v.bounds[2] - v.bounds[0]).toFloat(), (height - 2 * margin) / (v.bounds[3] - v.bounds[1]).toFloat())

    /** Canvas x of the venue's left edge, and y of its top edge (the map's largest y). */
    private fun originX(v: VenueMap): Float {
        val fitted = (width - fitScale(v) * (v.bounds[2] - v.bounds[0]).toFloat()) / 2
        return zoom * fitted + panX
    }

    private fun originY(v: VenueMap): Float {
        val fitted = (height - fitScale(v) * (v.bounds[3] - v.bounds[1]).toFloat()) / 2
        return zoom * fitted + panY
    }

    override fun onDraw(canvas: Canvas) {
        val v = venue ?: return
        if (width == 0 || height == 0) return
        val s = fitScale(v) * zoom
        val ox = originX(v)
        val oy = originY(v)
        fun px(x: Float) = ox + (x - v.bounds[0].toFloat()) * s
        fun py(y: Float) = oy + (v.bounds[3].toFloat() - y) * s

        val walls = v.levels[level.coerceIn(0, v.levels.size - 1)].walls
        var i = 0
        while (i + 3 < walls.size) {
            canvas.drawLine(px(walls[i]), py(walls[i + 1]), px(walls[i + 2]), py(walls[i + 3]), wallPaint)
            i += 4
        }
        for (c in v.connectors) {
            if (level !in c.levels || c.area.size < 6) continue
            path.rewind()
            path.moveTo(px(c.area[0]), py(c.area[1]))
            var k = 2
            while (k + 1 < c.area.size) {
                path.lineTo(px(c.area[k]), py(c.area[k + 1]))
                k += 2
            }
            path.close()
            canvas.drawPath(path, connectorPaint)
        }

        path.rewind()
        var drawing = false
        for (p in trail) {
            if (p[2].toInt() != level) {
                drawing = false
                continue
            }
            if (drawing) path.lineTo(px(p[0]), py(p[1])) else path.moveTo(px(p[0]), py(p[1]))
            drawing = true
        }
        canvas.drawPath(path, trailPaint)

        for (m in marks) {
            if (m[2].toInt() != level) continue
            val r = 4f * density
            canvas.drawRect(px(m[0]) - r, py(m[1]) - r, px(m[0]) + r, py(m[1]) + r, markPaint)
            if (m[3] != 0f) canvas.drawCircle(px(m[0]), py(m[1]), 9f * density, fixPaint)
        }

        val e = estimate
        if (e != null && e.valid && e.level == level) {
            val x = px(e.x.toFloat())
            val y = py(e.y.toFloat())
            canvas.drawCircle(x, y, (e.sigma * s).toFloat().coerceAtLeast(6f * density), spreadPaint)
            canvas.drawCircle(x, y, 6f * density, dotPaint)
            val tick = 16f * density
            canvas.drawLine(x, y, x + tick * cos(e.heading).toFloat(), y - tick * sin(e.heading).toFloat(), trailPaint)
        }
        if (e != null && e.valid && e.level != level) {
            // The estimate is on another floor: a hollow dot where it would be.
            canvas.drawCircle(px(e.x.toFloat()), py(e.y.toFloat()), 6f * density, trailPaint)
        }
    }
}
