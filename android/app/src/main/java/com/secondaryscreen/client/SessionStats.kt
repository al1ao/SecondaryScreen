package com.secondaryscreen.client

import kotlin.math.ceil

data class StatsSnapshot(val fps: Double?, val bitrateKbps: Long, val rttMs: Long?,
                         val latencyMs: Long?, val dropped: Long)

class SessionStats {
    private data class ClockSample(val rttUs: Long, val offsetUs: Long)
    private val samples = ArrayDeque<ClockSample>()
    private var offsetUs: Long? = null
    private var rttUs: Long? = null
    private var lastPongUs = 0L
    private var latencyEmaUs: Double? = null
    private var lastFrameUs = 0L
    private var windowStartUs = 0L
    private var frames = 0
    private var bytes = 0L
    private var fps: Double? = null
    private var kbps = 0L
    private var dropped = 0L

    @Synchronized fun onPong(pong: Pong, recvUs: Long) {
        val rtt = (recvUs - pong.clientSendUs) - (pong.hostSendUs - pong.hostRecvUs)
        if (rtt < 0 || rtt > 5_000_000) return
        val offset = ((pong.hostRecvUs - pong.clientSendUs) + (pong.hostSendUs - recvUs)) / 2
        samples.addLast(ClockSample(rtt, offset))
        if (samples.size > 8) samples.removeFirst()
        val best = samples.minBy { it.rttUs }
        rttUs = best.rttUs; offsetUs = best.offsetUs
        lastPongUs = recvUs
    }
    @Synchronized fun onFrameReceived(captureUs: Long, recvUs: Long, payloadBytes: Int) {
        tick(recvUs)
        bytes += payloadBytes
        val offset = offsetUs ?: return
        // Capture-to-receive estimate; decoder and display time are not included.
        val latency = (recvUs - captureUs + offset).coerceAtLeast(0).toDouble()
        latencyEmaUs = latencyEmaUs?.let { it * 0.8 + latency * 0.2 } ?: latency
        lastFrameUs = recvUs
    }
    @Synchronized fun onFrameRendered(nowUs: Long) { tick(nowUs); frames++ }
    @Synchronized fun onFrameDropped(count: Long = 1) { dropped += count }
    private fun tick(nowUs: Long) {
        if (windowStartUs == 0L) windowStartUs = nowUs
        val elapsed = nowUs - windowStartUs
        if (elapsed >= 1_000_000) {
            fps = frames * 1_000_000.0 / elapsed
            kbps = bytes * 8_000L / elapsed
            frames = 0; bytes = 0; windowStartUs = nowUs
        }
    }
    @Synchronized fun snapshot(nowUs: Long): StatsSnapshot {
        tick(nowUs)
        val live = lastFrameUs != 0L && nowUs - lastFrameUs <= 2_000_000
        // A stale RTT from a link that stopped answering pings is worse than none.
        val liveRtt = lastPongUs != 0L && nowUs - lastPongUs <= 3_000_000
        return StatsSnapshot(fps, kbps, if (liveRtt) rttUs?.let { (it + 999) / 1000 } else null,
            if (live) latencyEmaUs?.let { ceil(it / 1000).toLong() } else null, dropped)
    }
}
