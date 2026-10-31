package com.secondaryscreen.client

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Log
import android.view.SurfaceView
import java.net.ProtocolException
import java.util.ArrayDeque
import java.util.Locale

class H264Renderer(
    private val surfaceView: SurfaceView,
    private val stats: (String) -> Unit,
    private val failed: (String) -> Unit,
    private val sessionStats: SessionStats? = null
) {
    data class Support(val name: String?, val maxWidth: Int, val maxHeight: Int)
    private data class Packet(val bytes: ByteArray, val keyframe: Boolean, val timestampUs: Long, val generation: Int)
    private val thread = HandlerThread("SecondaryScreen-H264").apply { start() }
    private val handler = Handler(thread.looper)
    private val gate = Any()
    private val pending = ArrayDeque<Packet>()
    private var active = true
    private var generation = 0
    private var posted = false
    private var width = 0
    private var height = 0
    private var fps = 60
    private var codec: MediaCodec? = null
    private var codecStarted = false
    private var gotFirstKeyframe = false
    private var resetRequired = false
    private var rendered = 0
    private var lastStats = 0L
    private var rawClockDeltaMs = 0.0
    private var dropped = 0L
    private var queueDrop = 0L
    private var firstKeyframeDrop = 0L
    private var resetCount = 0L
    private var decodeMaxUs = 0L
    private var pendingPeak = 0

    fun startSession(newWidth: Int, newHeight: Int, newFps: Int) {
        synchronized(gate) {
            if (!active) return
            generation++
            pending.clear()
            handler.removeCallbacks(consume); posted = false
            width = newWidth; height = newHeight; fps = newFps
            handler.post {
                synchronized(gate) {
                    if (!active) return@post
                    // Reconnects and changed HELLO_ACK dimensions always get a new codec.
                    releaseCodec()
                    gotFirstKeyframe = false; resetRequired = false
                    rendered = 0; queueDrop = 0; firstKeyframeDrop = 0; resetCount = 0
                    decodeMaxUs = 0; pendingPeak = 0
                    lastStats = SystemClock.elapsedRealtime()
                }
            }
        }
    }
    fun submit(bytes: ByteArray, offset: Int, length: Int, isKeyframe: Boolean, timestampUs: Long) {
        synchronized(gate) {
            if (!active) return
            if (pending.size >= 4) {
                dropped += pending.size
                queueDrop += pending.size
                sessionStats?.onDecodeDropped(pending.size.toLong())
                pending.clear()
                resetRequired = true // Losing a reference frame requires a new IDR, not just a newer P-frame.
            }
            pending.addLast(Packet(bytes.copyOfRange(offset, offset + length), isKeyframe, timestampUs, generation))
            pendingPeak = maxOf(pendingPeak, pending.size)
            if (!posted) { posted = true; handler.post(consume) }
        }
    }
    fun referenceGap() { synchronized(gate) { resetRequired = true } }
    fun disconnected() {
        synchronized(gate) {
            if (!active) return
            generation++; pending.clear(); handler.removeCallbacks(consume); posted = false
            handler.post { synchronized(gate) { releaseCodec(); gotFirstKeyframe = false } }
        }
    }
    private val consume = object : Runnable {
        override fun run() {
            try {
                synchronized(gate) {
                    posted = false
                    if (!active) return
                    val packet = pending.pollFirst() ?: return
                    if (packet.generation == generation) {
                        val began = SystemClock.elapsedRealtimeNanos()
                        decode(packet)
                        decodeMaxUs = maxOf(decodeMaxUs, (SystemClock.elapsedRealtimeNanos() - began) / 1000)
                    }
                    if (pending.isNotEmpty() && !posted) { posted = true; handler.post(this) }
                }
            } catch (error: Exception) { fail(error) }
        }
    }
    private fun decode(packet: Packet) {
        if (!surfaceView.holder.surface.isValid) return
        if (resetRequired) {
            resetCount++
            // A new codec avoids stale decoder references and re-submits CSD with the next IDR.
            releaseCodec(); gotFirstKeyframe = false; resetRequired = false
        }
        if (!gotFirstKeyframe && !packet.keyframe) {
            dropped++; firstKeyframeDrop++; sessionStats?.onDecodeDropped(); return
        }
        val nals = H264AccessUnit.validate(packet.bytes, packet.keyframe)
        if (codec == null) createCodec()
        val decoder = codec ?: return
        drainOutput()
        if (packet.keyframe) {
            val config = H264AccessUnit.join(packet.bytes, nals.filter { it.type == 7 || it.type == 8 })
            if (!queue(decoder, config, packet.timestampUs, MediaCodec.BUFFER_FLAG_CODEC_CONFIG)) {
                resetRequired = true; dropped++; sessionStats?.onDecodeDropped(); return
            }
            val picture = H264AccessUnit.join(packet.bytes, nals.filter { it.type != 7 && it.type != 8 })
            if (!queue(decoder, picture, packet.timestampUs, MediaCodec.BUFFER_FLAG_KEY_FRAME)) {
                resetRequired = true; dropped++; sessionStats?.onDecodeDropped(); return
            }
            gotFirstKeyframe = true // Only after the complete IDR has actually been accepted.
        } else if (!queue(decoder, packet.bytes, packet.timestampUs, 0)) {
            resetRequired = true; dropped++; sessionStats?.onDecodeDropped(); return
        }
        rawClockDeltaMs = (SystemClock.elapsedRealtimeNanos() / 1000 - packet.timestampUs) / 1000.0
        drainOutput()
    }
    private fun createCodec() {
        val support = probe(width, height)
        val name = support.name ?: throw ProtocolException("无可用 H264 硬件解码器")
        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            setInteger(MediaFormat.KEY_PRIORITY, 0)
            setInteger(MediaFormat.KEY_OPERATING_RATE, fps)
            setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709)
            setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
            setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO)
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, minOf(Protocol.MAX_PAYLOAD,
                maxOf(1024 * 1024, width * height * 3 / 2)))
        }
        val created = MediaCodec.createByCodecName(name)
        codec = created // Publish immediately so configure/start exceptions still release the decoder.
        created.configure(format, surfaceView.holder.surface, null, 0)
        created.start(); codecStarted = true
        created.setVideoScalingMode(MediaCodec.VIDEO_SCALING_MODE_SCALE_TO_FIT)
        Log.i(TAG, "MediaCodec hardware decoder=$name ${width}x$height")
        handler.removeCallbacks(drain)
        handler.post(drain)
    }
    private fun queue(decoder: MediaCodec, data: ByteArray, pts: Long, flags: Int): Boolean {
        val index = decoder.dequeueInputBuffer(10_000) // Bounded 10ms; never wait indefinitely.
        if (index < 0) return false
        val input = decoder.getInputBuffer(index) ?: throw ProtocolException("MediaCodec 输入缓冲区不可用")
        input.clear()
        if (data.size > input.remaining()) throw ProtocolException("H264 帧超出解码器输入缓冲区")
        input.put(data)
        decoder.queueInputBuffer(index, 0, data.size, pts, flags)
        return true
    }
    private val drain = object : Runnable {
        override fun run() {
            try {
                synchronized(gate) {
                    if (!active || codec == null) return
                    drainOutput()
                    if (active && codec != null) handler.postDelayed(this, 5)
                }
            } catch (error: Exception) { fail(error) }
        }
    }
    private fun drainOutput() {
        val decoder = codec ?: return
        val info = MediaCodec.BufferInfo()
        while (active) {
            val index = decoder.dequeueOutputBuffer(info, 0)
            if (index >= 0) {
                if (!surfaceView.holder.surface.isValid) { releaseCodec(); gotFirstKeyframe = false; return }
                decoder.releaseOutputBuffer(index, true)
                rendered++
                sessionStats?.onFrameRendered(SystemClock.elapsedRealtimeNanos() / 1000)
            } else if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                Log.i(TAG, "H264 output format: ${decoder.outputFormat}")
            } else break
        }
        val now = SystemClock.elapsedRealtime()
        if (now - lastStats >= 1000) {
            // TODO(design): server/client monotonic clocks are not aligned in stage 2.
            // Show their raw difference, explicitly marked unsynchronized; never label it RTT.
            stats(String.format(Locale.US, "Δts %.1fms（未同步） | %.1f fps | drop %d", rawClockDeltaMs,
                rendered * 1000.0 / (now - lastStats), dropped))
            Log.d(TAG, String.format(Locale.US,
                "h264 render_fps=%.1f queue_drop=%d first_keyframe_drop=%d reset_required=%d pending_peak=%d decode_max_ms=%.1f",
                rendered * 1000.0 / (now - lastStats), queueDrop, firstKeyframeDrop, resetCount,
                pendingPeak, decodeMaxUs / 1000.0))
            rendered = 0; queueDrop = 0; firstKeyframeDrop = 0; resetCount = 0
            pendingPeak = 0; decodeMaxUs = 0; lastStats = now
        }
    }
    private fun fail(error: Exception) {
        Log.e(TAG, "H264 renderer", error)
        synchronized(gate) { releaseCodec(); gotFirstKeyframe = false; pending.clear() }
        failed(error.message ?: "H264 解码失败")
    }
    private fun releaseCodec() {
        handler.removeCallbacks(drain)
        val old = codec ?: return
        codec = null
        try { if (codecStarted) old.stop() } catch (error: RuntimeException) { Log.w(TAG, "MediaCodec.stop", error) }
        finally {
            codecStarted = false
            try { old.release() } catch (error: RuntimeException) { Log.w(TAG, "MediaCodec.release", error) }
        }
    }
    fun close() {
        synchronized(gate) {
            if (!active) return
            // Surface destruction waits for current guarded configure/render calls;
            // no subsequent releaseOutputBuffer(true) or configure can use this Surface.
            active = false; generation++; pending.clear()
            handler.removeCallbacksAndMessages(null)
            handler.post { synchronized(gate) { releaseCodec() }; thread.quitSafely() }
        }
    }
    companion object {
        private const val TAG = "SecondaryScreen"
        fun probe(width: Int, height: Int): Support {
            return try {
                // This app uses a regular Surface, so exclude secure/tunneled-only components.
                for (info in MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos) {
                    if (info.isEncoder || !info.supportedTypes.any { it.equals(MediaFormat.MIMETYPE_VIDEO_AVC, true) }) continue
                    val hardware = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) info.isHardwareAccelerated && !info.isSoftwareOnly
                        else !info.name.startsWith("OMX.google.", true) && !info.name.startsWith("c2.android.", true)
                    if (!hardware) continue
                    val capabilities = info.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC).videoCapabilities ?: continue
                    if (!capabilities.isSizeSupported(width and -2, height and -2)) continue
                    return Support(info.name, capabilities.supportedWidths.upper, capabilities.supportedHeights.upper)
                }
                Support(null, width, height)
            } catch (error: RuntimeException) { Log.w(TAG, "H264 decoder probe", error); Support(null, width, height) }
        }
    }
}
