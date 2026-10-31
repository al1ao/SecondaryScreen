package com.secondaryscreen.client

import kotlin.math.ceil

data class StatsSnapshot(val renderFps: Double, val receivedFps: Double, val bitrateKbps: Long,
                         val rttMs: Long?, val latencyMs: Long?, val droppedDecode: Long,
                         val droppedGap: Long, val droppedRenderQueue: Long) {
    val dropped: Long get() = droppedDecode + droppedGap + droppedRenderQueue
}

class SessionStats {
    private data class ClockSample(val rttUs: Long, val offsetUs: Long)
    private data class Received(val atUs: Long, val bytes: Int)
    private val samples = ArrayDeque<ClockSample>()
    private val rendered = ArrayDeque<Long>()
    private val received = ArrayDeque<Received>()
    private var offsetUs: Long? = null
    private var rttUs: Long? = null
    private var lastPongUs = 0L
    private var latencyEmaUs: Double? = null
    private var lastFrameUs = 0L
    private var droppedDecode = 0L
    private var droppedGap = 0L
    private var droppedRenderQueue = 0L

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
        received.addLast(Received(recvUs, payloadBytes))
        while (received.isNotEmpty() && received.first().atUs <= recvUs - 1_000_000) received.removeFirst()
        lastFrameUs = recvUs
        val offset = offsetUs ?: return
        // Capture-to-receive estimate; decoder and display time are not included.
        val latency = (recvUs - captureUs + offset).coerceAtLeast(0).toDouble()
        latencyEmaUs = latencyEmaUs?.let { it * 0.8 + latency * 0.2 } ?: latency
    }
    @Synchronized fun onFrameRendered(nowUs: Long) {
        rendered.addLast(nowUs)
        while (rendered.isNotEmpty() && rendered.first() <= nowUs - 1_000_000) rendered.removeFirst()
    }
    @Synchronized fun onDecodeDropped(count: Long = 1) { droppedDecode += count }
    @Synchronized fun onFrameGap(count: Long) { droppedGap += count }
    @Synchronized fun onRenderQueueOverwrite() { droppedRenderQueue++ }
    @Synchronized fun snapshot(nowUs: Long): StatsSnapshot {
        // UI refresh timing must not alter either measurement window.
        val renderCount = rendered.count { it > nowUs - 1_000_000 && it <= nowUs }
        val recent = received.filter { it.atUs > nowUs - 1_000_000 && it.atUs <= nowUs }
        val kbps = recent.sumOf { it.bytes.toLong() } * 8 / 1000
        val live = lastFrameUs != 0L && nowUs - lastFrameUs <= 2_000_000
        // A stale RTT from a link that stopped answering pings is worse than none.
        val liveRtt = lastPongUs != 0L && nowUs - lastPongUs <= 3_000_000
        return StatsSnapshot(renderCount.toDouble(), recent.size.toDouble(), kbps,
            if (liveRtt) rttUs?.let { (it + 999) / 1000 } else null,
            if (live) latencyEmaUs?.let { ceil(it / 1000).toLong() } else null,
            droppedDecode, droppedGap, droppedRenderQueue)
    }
}
