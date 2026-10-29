package com.secondaryscreen.client

internal class ReconnectDelay {
    private var nextMs = 1000L
    fun reset() { nextMs = 1000L }
    fun take(): Long = nextMs.also { nextMs = minOf(nextMs * 2, 5000L) }
}
