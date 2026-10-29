package com.padscreen.client

import android.app.Activity
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView

class MainActivity : Activity(), SurfaceHolder.Callback {
    private lateinit var surface: SurfaceView
    private lateinit var label: TextView
    private lateinit var statsLabel: TextView
    private lateinit var container: FrameLayout
    private var renderer: JpegRenderer? = null
    private var h264Renderer: H264Renderer? = null
    private var client: NetClient? = null
    private var resumed = false
    private var surfaceReady = false
    private var generation = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        ScreenService.startIfRequested(this)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        surface = SurfaceView(this).apply { holder.addCallback(this@MainActivity) }
        label = TextView(this).apply {
            text = "连接中…"; textSize = 16f; setTextColor(Color.WHITE)
            setBackgroundColor(0x99000000.toInt()); setPadding(16, 8, 16, 8)
        }
        statsLabel = TextView(this).apply {
            textSize = 14f; setTextColor(Color.WHITE)
            setBackgroundColor(0x99000000.toInt()); setPadding(12, 8, 12, 8)
            // TODO(design): no new wire flags. Android's adb launch extra controls
            // the local overlay; host --show-stats controls local host diagnostics.
            visibility = if (intent.getBooleanExtra("show-stats", false)) View.VISIBLE else View.GONE
        }
        container = FrameLayout(this).apply {
            setBackgroundColor(Color.BLACK)
            addView(surface, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
            addView(label, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.START))
            addView(statsLabel, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.END))
        }
        setContentView(container)
        fullscreen()
    }
    override fun onStart() { super.onStart(); resumed = true; startIfReady() }
    override fun onStop() { resumed = false; stopClient(); super.onStop() }
    override fun surfaceCreated(holder: SurfaceHolder) { surfaceReady = true; startIfReady() }
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = Unit
    override fun surfaceDestroyed(holder: SurfaceHolder) { surfaceReady = false; stopClient() }
    override fun onWindowFocusChanged(hasFocus: Boolean) { super.onWindowFocusChanged(hasFocus); if (hasFocus) fullscreen() }
    private fun startIfReady() {
        if (!resumed || !surfaceReady || client != null) return
        val currentGeneration = ++generation
        val callback: (String) -> Unit = { text ->
            runOnUiThread { if (generation == currentGeneration && resumed) label.text = text }
        }
        val newRenderer = JpegRenderer(surface, callback)
        renderer = newRenderer
        val hardwareRenderer = H264Renderer(surface, { text ->
            runOnUiThread { if (generation == currentGeneration && resumed) statsLabel.text = text }
        }, { reason -> runOnUiThread { if (generation == currentGeneration) client?.decoderFailed(reason) } })
        h264Renderer = hardwareRenderer
        val metrics = resources.displayMetrics
        // TODO(design): stage 1 has no settings UI; an adb integer extra optionally overrides the TCP port.
        val port = intent.getIntExtra("port", Protocol.DEFAULT_PORT).takeIf { it in 1..65535 } ?: Protocol.DEFAULT_PORT
        client = NetClient(maxOf(metrics.widthPixels, metrics.heightPixels), minOf(metrics.widthPixels, metrics.heightPixels),
            metrics.densityDpi, port, newRenderer, hardwareRenderer, { ack ->
                runOnUiThread {
                    if (generation == currentGeneration && resumed) fitSurface(ack.width, ack.height, currentGeneration)
                }
            }, callback).also { it.start() }
    }
    private fun stopClient() {
        generation++
        h264Renderer?.close(); h264Renderer = null
        client?.stop(); client = null
        renderer?.close(); renderer = null
    }
    private fun fitSurface(width: Int, height: Int, sessionGeneration: Int) {
        container.post {
            if (!resumed || !surfaceReady || generation != sessionGeneration) return@post
            val scale = minOf(container.width.toFloat() / width, container.height.toFloat() / height)
            surface.layoutParams = FrameLayout.LayoutParams(maxOf(1, (width * scale).toInt()),
                maxOf(1, (height * scale).toInt()), Gravity.CENTER)
        }
    }
    @Suppress("DEPRECATION")
    private fun fullscreen() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false)
            window.insetsController?.apply {
                hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        }
    }
}
