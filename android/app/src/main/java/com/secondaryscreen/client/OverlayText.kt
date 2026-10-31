package com.secondaryscreen.client

import java.util.Locale

object OverlayText {
    fun format(fps: Double?, rttMs: Long?, latencyMs: Long?, codecName: String?, width: Int,
               height: Int, bitrateKbps: Long, dropped: Long, showFps: Boolean,
               showLink: Boolean, showLatency: Boolean, debug: StatsSnapshot? = null): String {
        val parts = mutableListOf<String>()
        if (showFps) parts += fps?.let {
            val digits = if (it == it.toLong().toDouble()) "%.0f" else "%.1f"
            String.format(Locale.US, digits, it) + " fps"
        } ?: "— fps"
        if (showLink) parts += "${codecName ?: "—"} ${width}×${height} · " +
            String.format(Locale.US, "%.1f Mbps", bitrateKbps / 1000.0) +
            " · RTT ${rttMs?.let { "$it ms" } ?: "—"} · drop $dropped"
        if (showLatency) parts += "延迟 ${latencyMs?.let { "$it ms" } ?: "—"}"
        if (debug != null) parts += String.format(Locale.US,
            "rx %.0f / render %.0f · drop decode/gap/queue %d/%d/%d",
            debug.receivedFps, debug.renderFps, debug.droppedDecode, debug.droppedGap,
            debug.droppedRenderQueue)
        return parts.joinToString(" · ")
    }
}
