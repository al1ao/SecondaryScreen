package com.padscreen.client

import android.util.Log
import java.io.EOFException
import java.io.IOException
import java.net.InetSocketAddress
import java.net.ProtocolException
import java.net.Socket
import java.net.SocketException
import java.util.concurrent.atomic.AtomicBoolean

class NetClient(
    private val width: Int,
    private val height: Int,
    private val dpi: Int,
    private val port: Int,
    private val renderer: JpegRenderer,
    private val h264Renderer: H264Renderer,
    private val session: (HelloAck) -> Unit,
    private val status: (String) -> Unit
) {
    private val running = AtomicBoolean(false)
    private val socketLock = Any()
    private var socket: Socket? = null
    private var worker: Thread? = null
    private val forceJpeg = AtomicBoolean(false)
    private val support = H264Renderer.probe(minOf(width, 1280), minOf(height, 800))

    fun start() {
        if (!running.compareAndSet(false, true)) return
        worker = Thread({ runLoop() }, "PadScreen-Net").also { it.start() }
    }
    fun stop() {
        running.set(false)
        synchronized(socketLock) { try { socket?.close() } catch (error: IOException) { Log.w(TAG, "Close stopped session", error) }; socket = null }
        worker?.interrupt()
        // The worker holds only its renderer/status callbacks, both invalidated by Activity onStop.
        // It exits after socket.close/interrupt; avoid blocking the UI on a network thread join.
    }
    fun decoderFailed(reason: String) {
        forceJpeg.set(true)
        status("H264 解码失败: $reason；重连并回退 JPEG…")
        synchronized(socketLock) { try { socket?.close() } catch (error: IOException) { Log.w(TAG, "Close failed decoder session", error) } }
    }
    private fun runLoop() {
        // One 16 MiB payload buffer is reused even across reconnect attempts.
        val payloadReaderFactory = ReusableInputStream()
        val reader = MessageReader(payloadReaderFactory)
        val reconnectDelay = ReconnectDelay()
        while (running.get()) {
            status("连接中…")
            try {
                Socket().use { connection ->
                    synchronized(socketLock) {
                        if (!running.get()) return
                        socket = connection // Publish before connect so stop can cancel it.
                    }
                    connection.tcpNoDelay = true
                    connection.soTimeout = 5000
                    connection.connect(InetSocketAddress("127.0.0.1", port), 1000)
                    Log.i(TAG, "connected")
                    val hardware = !forceJpeg.get() && support.name != null
                    val capabilities = Capability.CODEC_JPEG or if (hardware) Capability.CODEC_H264 else 0
                    sendHello(connection.getOutputStream(), width, height, 60, dpi, capabilities,
                        if (hardware) support.maxWidth else width, if (hardware) support.maxHeight else height)
                    payloadReaderFactory.input = connection.getInputStream()
                    val ack = readAck(reader)
                    if (ack.codec == Codec.H264 && !hardware) throw ProtocolException("服务端选择了未声明的 H264 能力")
                    val codecName = if (ack.codec == Codec.H264) "H264" else "JPEG"
                    val connectedText = "已连接 ${ack.width}x${ack.height} $codecName"
                    status(connectedText)
                    Log.i(TAG, "HELLO_ACK ${ack.width}x${ack.height} codec=$codecName")
                    session(ack)
                    if (ack.codec == Codec.H264) {
                        renderer.disconnected()
                        h264Renderer.startSession(ack.width, ack.height, ack.fps)
                    } else renderer.resetFps(connectedText)
                    var lastFrameId = 0L
                    while (running.get()) {
                        val header = reader.readHeader() ?: throw EOFException("服务端已断开")
                        val payload = reader.readPayload(header.length)
                        if (header.type == MessageType.VIDEO_FRAME) {
                            val frame = parseFrameInfo(payload, header.length, ack)
                            reconnectDelay.reset()
                            if (frame.frameId <= lastFrameId) throw ProtocolException("frame_id 未递增")
                            if (ack.codec == Codec.H264 && lastFrameId != 0L && frame.frameId != lastFrameId + 1)
                                h264Renderer.referenceGap()
                            lastFrameId = frame.frameId
                            Log.d(TAG, "frame=${frame.frameId} payload_size=${frame.payloadSize} length=${header.length}")
                            if (ack.codec == Codec.JPEG)
                                renderer.render(payload, Protocol.FRAME_INFO_SIZE, frame.payloadSize, ack.width, ack.height)
                            else h264Renderer.submit(payload, Protocol.FRAME_INFO_SIZE, frame.payloadSize,
                                (header.flags and Protocol.FLAG_KEYFRAME) != 0, frame.captureTsUs)
                        } else Log.w(TAG, "忽略未知消息 type=${header.type} length=${header.length}")
                    }
                }
            } catch (_: InterruptedException) {
                break
            } catch (error: IOException) {
                if (running.get()) {
                    val reason = if (error is java.net.ConnectException) "连接被拒绝，请检查 host 和 adb reverse" else error.message
                    status("错误: $reason；重连中…")
                    Log.w(TAG, "Disconnected: $reason")
                }
            } catch (error: RuntimeException) {
                if (running.get()) { status("错误: ${error.message}；重连中…"); Log.e(TAG, "Client error", error) }
            } finally {
                synchronized(socketLock) { try { socket?.close() } catch (error: IOException) { Log.w(TAG, "Close disconnected session", error) }; socket = null }
                payloadReaderFactory.input = null
                renderer.disconnected()
                h264Renderer.disconnected()
            }
            if (!running.get()) break
            try { Thread.sleep(reconnectDelay.take()) } catch (_: InterruptedException) { break }
        }
    }
    private fun readAck(reader: MessageReader): HelloAck {
        while (running.get()) {
            val header = reader.readHeader() ?: throw EOFException("握手时连接关闭")
            val payload = reader.readPayload(header.length)
            if (header.type == MessageType.HELLO_ACK) return parseHelloAck(payload, header.length)
            if (header.type == MessageType.VIDEO_FRAME) throw ProtocolException("HELLO_ACK 前收到视频帧")
            Log.w(TAG, "握手时忽略未知消息 type=${header.type}")
        }
        throw SocketException("已停止")
    }
    // TODO(design): retain the protocol reader's fixed receive buffer while replacing its socket input.
    private class ReusableInputStream : java.io.InputStream() {
        var input: java.io.InputStream? = null
        override fun read(): Int = input?.read() ?: -1
        override fun read(bytes: ByteArray, off: Int, len: Int): Int = input?.read(bytes, off, len) ?: -1
    }
    companion object { private const val TAG = "PadScreen" }
}
