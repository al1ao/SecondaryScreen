package com.secondaryscreen.client

import kotlin.math.roundToInt

// Pure policy kept separate so both platform paths and failures can be tested on the JVM.
object DisplayValues {
    fun readSize(api: Int, maximum: () -> Pair<Int, Int>, real: () -> Pair<Int, Int>): Pair<Int, Int> {
        fun attempt(read: () -> Pair<Int, Int>): Pair<Int, Int>? = try {
            read().takeIf { it.first in 2..16384 && it.second in 2..16384 }
        } catch (_: RuntimeException) { null }
        val size = (if (api >= 30) attempt(maximum) else null) ?: attempt(real)
        // Preserve the physical orientation; only failed queries use the safe default.
        return size ?: (1280 to 800)
    }

    fun readRefresh(api: Int, mode: () -> Float, legacy: () -> Float): Int = try {
        val value = if (api >= 30) mode() else legacy()
        if (!value.isFinite() || value <= 0) 60 else value.roundToInt().coerceIn(30, 165)
    } catch (_: RuntimeException) { 60 }
}
