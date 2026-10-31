package com.secondaryscreen.client

import kotlin.math.roundToInt

object InputValues {
    fun ppm(position: Float, size: Int): Int =
        if (size <= 0) 0 else ((position / size).coerceIn(0f, 1f) * 1_000_000).roundToInt()
    fun sendMove(lastMoveUs: Long, nowUs: Long): Boolean = nowUs - lastMoveUs >= 8_000
    fun rotatePpm(xPpm: Int, yPpm: Int, degrees: Int): Pair<Int, Int> {
        val x = xPpm.coerceIn(0, 1_000_000)
        val y = yPpm.coerceIn(0, 1_000_000)
        return when (degrees) {
            0 -> x to y
            90 -> (1_000_000 - y) to x
            180 -> (1_000_000 - x) to (1_000_000 - y)
            270 -> y to (1_000_000 - x)
            else -> throw IllegalArgumentException("rotation must be 0, 90, 180 or 270")
        }
    }
}
