package com.secondaryscreen.client

import android.content.Context

data class DisplaySettings(
    val showFps: Boolean = false, val showLink: Boolean = false,
    val showLatency: Boolean = false, val touch: Boolean = false,
    val widthOverride: Int = 0, val heightOverride: Int = 0, val fpsCap: Int = 0
)

class SettingsStore(context: Context) {
    private val prefs = context.getSharedPreferences("display-settings", Context.MODE_PRIVATE)
    fun read() = DisplaySettings(
        prefs.getBoolean("fps", false), prefs.getBoolean("link", false),
        prefs.getBoolean("latency", false), prefs.getBoolean("touch", false),
        prefs.getInt("width", 0), prefs.getInt("height", 0), prefs.getInt("fpsCap", 0))
    fun write(value: DisplaySettings) {
        prefs.edit().putBoolean("fps", value.showFps).putBoolean("link", value.showLink)
            .putBoolean("latency", value.showLatency).putBoolean("touch", value.touch)
            .putInt("width", value.widthOverride).putInt("height", value.heightOverride)
            .putInt("fpsCap", value.fpsCap).apply()
    }
}
