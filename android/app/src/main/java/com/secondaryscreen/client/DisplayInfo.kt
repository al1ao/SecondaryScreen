package com.secondaryscreen.client

import android.graphics.Point
import android.os.Build
import android.view.WindowManager

object DisplayInfo {
    @Suppress("DEPRECATION")
    fun read(windowManager: WindowManager): Pair<Int, Int> = DisplayValues.readSize(
        Build.VERSION.SDK_INT,
        {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                val bounds = windowManager.maximumWindowMetrics.bounds
                bounds.width() to bounds.height()
            } else throw IllegalStateException("WindowMetrics requires API 30")
        },
        {
            val size = Point()
            windowManager.defaultDisplay.getRealSize(size)
            size.x to size.y
        }
    )

    @Suppress("DEPRECATION")
    fun refreshRate(windowManager: WindowManager): Int = DisplayValues.readRefresh(
        Build.VERSION.SDK_INT,
        { windowManager.defaultDisplay.mode.refreshRate },
        { windowManager.defaultDisplay.refreshRate }
    )
}
