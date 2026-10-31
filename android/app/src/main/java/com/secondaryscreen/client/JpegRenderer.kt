package com.secondaryscreen.client

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.SurfaceView
import java.net.ProtocolException
import java.util.Locale

class JpegRenderer(private val surfaceView: SurfaceView, private val status: (String) -> Unit,
                   private val sessionStats: SessionStats? = null) {
    private val main = Handler(Looper.getMainLooper())
    private val lock = Any()
    private val paint = Paint(Paint.FILTER_BITMAP_FLAG)
    private var pending: Bitmap? = null
    private var posted = false
    private var active = true
    private var session = 0
    private var connectedText = ""
    private var frameCount = 0
    private var lastFpsMs = 0L

    fun resetFps(text: String) {
        main.post {
            synchronized(lock) {
                session++
                pending?.recycle(); pending = null
                connectedText = text; frameCount = 0; lastFpsMs = SystemClock.elapsedRealtime()
            }
        }
    }
    fun disconnected() {
        synchronized(lock) { session++; pending?.recycle(); pending = null }
    }
    fun render(jpeg: ByteArray, offset: Int, length: Int, width: Int, height: Int) {
        val generation = synchronized(lock) { if (!active) return else session }
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(jpeg, offset, length, bounds)
        if (bounds.outWidth != width || bounds.outHeight != height)
            throw ProtocolException("JPEG 尺寸与 HELLO_ACK 不符")
        val bitmap = BitmapFactory.decodeByteArray(jpeg, offset, length)
            ?: throw ProtocolException("JPEG 解码失败")
        synchronized(lock) {
            if (!active || generation != session) { bitmap.recycle(); return }
            // TODO(design): keep one pending decoded bitmap so a paused/slow UI cannot accumulate frames.
            if (pending != null) sessionStats?.onFrameDropped()
            pending?.recycle()
            pending = bitmap
            if (!posted) {
                posted = true
                main.post(draw)
            }
        }
    }
    private val draw = Runnable {
        val bitmap = synchronized(lock) {
            posted = false
            pending.also { pending = null }
        }
        if (bitmap != null) {
            try {
                val holder = surfaceView.holder
                if (holder.surface.isValid) {
                    val canvas = holder.lockCanvas()
                    if (canvas != null) {
                        try {
                            canvas.drawColor(Color.BLACK)
                            canvas.drawBitmap(bitmap, null, fitCenter(bitmap.width, bitmap.height, canvas.width, canvas.height), paint)
                            frameCount++
                            sessionStats?.onFrameRendered(SystemClock.elapsedRealtimeNanos() / 1000)
                        } finally { holder.unlockCanvasAndPost(canvas) }
                    }
                }
                val now = SystemClock.elapsedRealtime()
                if (now - lastFpsMs >= 1000) {
                    val fps = frameCount * 1000.0 / (now - lastFpsMs)
                    status(String.format(Locale.US, "%s  %.1f fps", connectedText, fps))
                    frameCount = 0; lastFpsMs = now
                }
            } catch (error: RuntimeException) {
                // Surface may disappear between isValid and lockCanvas while the Activity stops.
                Log.w("SecondaryScreen", "Surface unavailable", error)
            } finally { bitmap.recycle() }
        }
    }
    fun close() {
        synchronized(lock) {
            active = false
            pending?.recycle(); pending = null
            main.removeCallbacksAndMessages(null)
            posted = false
        }
    }
    private fun fitCenter(sw: Int, sh: Int, dw: Int, dh: Int): RectF {
        val scale = minOf(dw.toFloat() / sw, dh.toFloat() / sh)
        val w = sw * scale; val h = sh * scale
        return RectF((dw - w) / 2, (dh - h) / 2, (dw + w) / 2, (dh + h) / 2)
    }
}
