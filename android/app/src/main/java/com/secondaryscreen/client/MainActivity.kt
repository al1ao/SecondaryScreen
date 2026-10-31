package com.secondaryscreen.client

import android.app.Activity
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.content.res.Configuration
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.CheckBox
import android.widget.Spinner
import android.widget.ArrayAdapter
import android.widget.AdapterView

class MainActivity : Activity(), SurfaceHolder.Callback {
    private lateinit var surface: SurfaceView
    private lateinit var statsLabel: TextView
    private lateinit var container: FrameLayout
    private var renderer: JpegRenderer? = null
    private var h264Renderer: H264Renderer? = null
    private var client: NetClient? = null
    private var resumed = false
    private var surfaceReady = false
    private var generation = 0
    private lateinit var store: SettingsStore
    private var settings = DisplaySettings()
    private lateinit var panel: LinearLayout
    private lateinit var settingsButton: ImageView
    private lateinit var touchToggle: CheckBox
    private val ui = Handler(Looper.getMainLooper())
    private var stats = SessionStats()
    private var ack: HelloAck? = null
    private var lastMoveUs = 0L
    private var touchDown = false
    private var deviceSize: Pair<Int, Int>? = null
    private val debugStats by lazy { intent.getBooleanExtra("show-stats", false) }
    private val updateOverlay = object : Runnable {
        override fun run() {
            val current = ack
            if (current == null) {
                statsLabel.visibility = View.GONE
                ui.postDelayed(this, 500)
                return
            }
            val sample = stats.snapshot(SystemClock.elapsedRealtimeNanos() / 1000)
            val text = OverlayText.format(sample.fps, sample.rttMs, sample.latencyMs,
                if (current.codec == Codec.H264) "H264" else "JPEG",
                current.width, current.height, sample.bitrateKbps, sample.dropped,
                settings.showFps || debugStats, settings.showLink || debugStats,
                settings.showLatency || debugStats)
            statsLabel.text = text
            statsLabel.visibility = if (text.isEmpty()) View.GONE else View.VISIBLE
            ui.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        store = SettingsStore(this); settings = store.read()
        ScreenService.startIfRequested(this)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        surface = SurfaceView(this).apply { holder.addCallback(this@MainActivity) }
        statsLabel = TextView(this).apply {
            textSize = 14f; setTextColor(Color.WHITE)
            setBackgroundColor(0x99000000.toInt()); setPadding(12, 8, 12, 8)
            // TODO(design): no new wire flags. Android's adb launch extra controls
            // the local overlay; host --show-stats controls local host diagnostics.
            visibility = View.GONE
        }
        container = FrameLayout(this).apply {
            setBackgroundColor(Color.BLACK)
            addView(surface, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
            addView(statsLabel, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.END))
        }
        setContentView(container)
        buildSettings()
        surface.setOnTouchListener { view, event ->
            if (!settings.touch || client?.supportsTouch() != true) return@setOnTouchListener false
            val action = when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> 0
                MotionEvent.ACTION_MOVE -> 1
                MotionEvent.ACTION_UP -> 2
                MotionEvent.ACTION_CANCEL -> 3
                else -> return@setOnTouchListener true
            }
            val now = SystemClock.elapsedRealtimeNanos() / 1000
            if (action == 1 && !InputValues.sendMove(lastMoveUs, now)) return@setOnTouchListener true
            if (action == 1) lastMoveUs = now
            val (x, y) = InputValues.rotatePpm(InputValues.ppm(event.x, view.width),
                InputValues.ppm(event.y, view.height), ack?.rotationDegrees ?: 0)
            client?.sendTouch(action, x, y)
            touchDown = action == 0 || action == 1
            true
        }
        fullscreen()
    }
    override fun onStart() { super.onStart(); resumed = true; ui.post(updateOverlay); startIfReady() }
    override fun onStop() { resumed = false; ui.removeCallbacks(updateOverlay); stopClient(); super.onStop() }
    @Deprecated("Back callback")
    override fun onBackPressed() { if (panel.visibility == View.VISIBLE) panel.visibility = View.GONE else super.onBackPressed() }
    override fun surfaceCreated(holder: SurfaceHolder) { surfaceReady = true; startIfReady() }
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = Unit
    override fun surfaceDestroyed(holder: SurfaceHolder) { surfaceReady = false; stopClient() }
    override fun onWindowFocusChanged(hasFocus: Boolean) { super.onWindowFocusChanged(hasFocus); if (hasFocus) fullscreen() }
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        val current = DisplayInfo.read(windowManager)
        if (current != deviceSize) {
            stopClient()
            buildSettings()
            startIfReady()
        }
    }
    private fun startIfReady() {
        if (!resumed || !surfaceReady || client != null) return
        val currentGeneration = ++generation
        val callback: (String) -> Unit = { _ -> }
        stats = SessionStats(); ack = null
        val newRenderer = JpegRenderer(surface, { }, stats)
        renderer = newRenderer
        val hardwareRenderer = H264Renderer(surface, { },
            { reason -> runOnUiThread { if (generation == currentGeneration) client?.decoderFailed(reason) } }, stats)
        h264Renderer = hardwareRenderer
        val metrics = resources.displayMetrics
        val (realWidth, realHeight) = DisplayInfo.read(windowManager)
        deviceSize = realWidth to realHeight
        normalizeResolution(realWidth, realHeight)
        val refreshRate = DisplayInfo.refreshRate(windowManager)
        // TODO(design): stage 1 has no settings UI; an adb integer extra optionally overrides the TCP port.
        val port = intent.getIntExtra("port", Protocol.DEFAULT_PORT).takeIf { it in 1..65535 } ?: Protocol.DEFAULT_PORT
        client = NetClient(settings.widthOverride.takeIf { it > 0 } ?: realWidth,
            settings.heightOverride.takeIf { it > 0 } ?: realHeight,
            settings.fpsCap.takeIf { it > 0 } ?: refreshRate,
            metrics.densityDpi, port, newRenderer, hardwareRenderer, stats, { ack ->
                runOnUiThread {
                    if (generation == currentGeneration && resumed) {
                        this.ack = ack
                        touchToggle.isEnabled = ack.capabilities and Protocol.ACK_CAP_TOUCH_INPUT != 0
                        if (!touchToggle.isEnabled) touchToggle.isChecked = false
                        touchToggle.text = getString(if (touchToggle.isEnabled) R.string.touch_mode else R.string.touch_unsupported)
                        fitSurface(ack.width, ack.height, ack.rotationDegrees, currentGeneration)
                    }
                }
            }, callback).also { it.start() }
    }
    private fun stopClient() {
        generation++
        if (touchDown) { client?.sendTouch(3, 0, 0); touchDown = false }
        ack = null
        statsLabel.visibility = View.GONE
        surface.rotation = 0f
        touchToggle.isEnabled = false
        touchToggle.text = getString(R.string.touch_mode)
        h264Renderer?.close(); h264Renderer = null
        client?.stop(); client = null
        renderer?.close(); renderer = null
    }
    private fun dp(value: Int) = (value * resources.displayMetrics.density + 0.5f).toInt()
    private fun normalizeResolution(nativeW: Int, nativeH: Int) {
        val (w, h) = ResolutionOptions.normalizedOverride(
            settings.widthOverride, settings.heightOverride, nativeW, nativeH)
        if (w != settings.widthOverride || h != settings.heightOverride) {
            settings = settings.copy(widthOverride = w, heightOverride = h)
            store.write(settings)
        }
    }
    private fun buildSettings() {
        val panelWasVisible = ::panel.isInitialized && panel.visibility == View.VISIBLE
        if (::panel.isInitialized) container.removeView(panel)
        if (::settingsButton.isInitialized) container.removeView(settingsButton)
        fun background(color: Int, radius: Int) = GradientDrawable().apply {
            setColor(color); cornerRadius = dp(radius).toFloat()
        }
        panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL; setPadding(dp(16), dp(12), dp(16), dp(12))
            background = background(0xCC1E1E1E.toInt(), 12)
            visibility = if (panelWasVisible) View.VISIBLE else View.GONE
        }
        val panelWidth = minOf(dp(300), resources.displayMetrics.widthPixels - dp(64)).coerceAtLeast(dp(160))
        val params = FrameLayout.LayoutParams(panelWidth, FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.END or Gravity.CENTER_VERTICAL).apply { marginEnd = dp(52) }
        container.addView(panel, params)
        val heading = TextView(this).apply { text = getString(R.string.display_settings); textSize = 20f; setTextColor(Color.WHITE)
            setOnClickListener { panel.visibility = View.GONE } }
        panel.addView(heading)
        fun toggle(title: String, initial: Boolean, changed: (Boolean) -> Unit): CheckBox =
            CheckBox(this).apply {
                buttonDrawable = null
                setCompoundDrawablesWithIntrinsicBounds(null, null, getDrawable(R.drawable.selection_dot), null)
                text = title; textSize = 16f; setTextColor(Color.WHITE); isChecked = initial
                minHeight = dp(44); gravity = Gravity.CENTER_VERTICAL
                compoundDrawablePadding = dp(12)
                setOnCheckedChangeListener { _, checked -> changed(checked); store.write(settings) }
                panel.addView(this, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT))
            }
        toggle(getString(R.string.show_fps), settings.showFps) { settings = settings.copy(showFps = it) }
        toggle(getString(R.string.show_link), settings.showLink) { settings = settings.copy(showLink = it) }
        toggle(getString(R.string.show_latency), settings.showLatency) { settings = settings.copy(showLatency = it) }
        val (nativeW, nativeH) = DisplayInfo.read(windowManager)
        deviceSize = nativeW to nativeH
        val sizes = ResolutionOptions.candidates(nativeW, nativeH)
        normalizeResolution(nativeW, nativeH)
        fun spinner(labels: List<String>, selection: Int, changed: (Int) -> Unit) {
            val view = Spinner(this)
            view.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, labels)
            panel.addView(view)
            view.setSelection(selection.coerceIn(labels.indices))
            view.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                var first = true
                override fun onNothingSelected(parent: AdapterView<*>?) = Unit
                override fun onItemSelected(parent: AdapterView<*>?, selected: View?, position: Int, id: Long) {
                    if (first) { first = false; return }
                    changed(position); store.write(settings); stopClient(); startIfReady()
                }
            }
        }
        spinner(sizes.map { (w, h) -> if (w == 0) getString(R.string.follow_tablet) else if (w == nativeW && h == nativeH)
            getString(R.string.native_resolution, w, h) else "${w}×${h}" + if (if (nativeW < nativeH) w * 16 != h * 10 else w * 10 != h * 16)
                getString(R.string.black_bars_hint) else "" },
            sizes.indexOf(settings.widthOverride to settings.heightOverride).coerceAtLeast(0)) {
            val (w, h) = sizes[it]; settings = settings.copy(widthOverride = w, heightOverride = h)
        }
        val rates = listOf(0, 30, 60, 90, 120, 144)
        spinner(rates.map { if (it == 0) getString(R.string.rate_follow) else getString(R.string.rate_cap, it) },
            rates.indexOf(settings.fpsCap).coerceAtLeast(0)) { settings = settings.copy(fpsCap = rates[it]) }
        touchToggle = toggle(getString(R.string.touch_mode), settings.touch) { checked ->
            if (!checked && touchDown) { client?.sendTouch(3, 0, 0); touchDown = false }
            settings = settings.copy(touch = checked)
        }
        touchToggle.isEnabled = false
        panel.addView(TextView(this).apply {
            text = getString(R.string.reconnect_hint); textSize = 12f; setTextColor(0xFFBBBBBB.toInt())
        })
        settingsButton = ImageView(this).apply {
            setImageResource(R.drawable.ic_settings); contentDescription = getString(R.string.settings)
            background = background(0x66000000, 20); alpha = 0.55f
            setOnClickListener { panel.visibility = if (panel.visibility == View.VISIBLE) View.GONE else View.VISIBLE }
        }
        container.addView(settingsButton, FrameLayout.LayoutParams(dp(40), dp(72), Gravity.END or Gravity.CENTER_VERTICAL))
    }
    private fun fitSurface(width: Int, height: Int, rotationDegrees: Int, sessionGeneration: Int) {
        container.post {
            if (!resumed || !surfaceReady || generation != sessionGeneration) return@post
            val swap = rotationDegrees == 90 || rotationDegrees == 270
            val effectiveWidth = if (swap) height else width
            val effectiveHeight = if (swap) width else height
            val scale = minOf(container.width.toFloat() / effectiveWidth,
                container.height.toFloat() / effectiveHeight)
            surface.layoutParams = FrameLayout.LayoutParams(maxOf(1, (width * scale).toInt()),
                maxOf(1, (height * scale).toInt()), Gravity.CENTER)
            surface.rotation = rotationDegrees.toFloat()
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
