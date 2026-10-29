package com.padscreen.client

import kotlin.math.roundToInt

// Pure policy kept separate so both platform paths and failures can be tested on the JVM.
object DisplayValues {
    fun readSize(api: Int, maximum: () -> Pair<Int, Int>, real: () -> Pair<Int, Int>): Pair<Int, Int> {
        fun attempt(read: () -> Pair<Int, Int>): Pair<Int, Int>? = try {
            read().takeIf { it.first in 2..16384 && it.second in 2..16384 }
        } catch (_: RuntimeException) { null }
        val size = (if (api >= 30) attempt(maximum) else null) ?: attempt(real)
        // TODO(design): both platform queries failed; retain the protocol's safe landscape default.
        return size?.let { maxOf(it.first, it.second) to minOf(it.first, it.second) } ?: (1280 to 800)
    }

    fun readRefresh(api: Int, mode: () -> Float, legacy: () -> Float): Int = try {
        val value = if (api >= 30) mode() else legacy()
        if (!value.isFinite() || value <= 0) 60 else value.roundToInt().coerceIn(30, 165)
    } catch (_: RuntimeException) { 60 }
}
