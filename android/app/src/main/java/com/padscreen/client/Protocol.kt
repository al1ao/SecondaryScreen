package com.padscreen.client

import java.io.EOFException
import java.io.InputStream
import java.io.OutputStream
import java.net.ProtocolException
import java.nio.ByteBuffer
import java.nio.ByteOrder

object Protocol {
    const val MAGIC = 0x31534450
    const val VERSION = 1
    const val MAX_PAYLOAD = 16 * 1024 * 1024
    const val DEFAULT_PORT = 51900
    const val HEADER_SIZE = 12
    const val HELLO_SIZE = 32
    const val ACK_SIZE = 32
    const val FRAME_INFO_SIZE = 28
    const val FLAG_KEYFRAME = 1
    const val FLAG_END_OF_STREAM = 2
}
object MessageType {
    const val HELLO = 0x0001
    const val HELLO_ACK = 0x0002
    const val VIDEO_FRAME = 0x0010
    const val STATS = 0x0020
    const val PING = 0x0030
    const val PONG = 0x0031
    const val BYE = 0x00FF
}
object Capability {
    const val CODEC_JPEG = 1 shl 0
    const val CODEC_H264 = 1 shl 1
    const val CODEC_HEVC = 1 shl 2
}
object Codec {
    const val JPEG = 1
    const val H264 = 2
    const val HEVC = 3
}
data class HelloAck(
    val version: Int, val result: Int, val codec: Int, val fps: Int,
    val width: Int, val height: Int, val bitrateKbps: Long,
    val frameIntervalUs: Long, val serverTimeUs: Long
)
data class FrameInfo(
    val frameId: Long, val codec: Int, val frameFlags: Int,
    val width: Int, val height: Int, val captureTsUs: Long, val payloadSize: Int
)
private fun ByteBuffer.u16(): Int = short.toInt() and 0xFFFF
private fun ByteBuffer.u8(): Int = get().toInt() and 0xFF
private fun ByteBuffer.u32(): Long = int.toLong() and 0xFFFFFFFFL
private fun little(bytes: ByteArray, length: Int): ByteBuffer =
    ByteBuffer.wrap(bytes, 0, length).order(ByteOrder.LITTLE_ENDIAN)

fun makeHello(width: Int, height: Int, fps: Int, dpi: Int,
              capabilities: Int = Capability.CODEC_JPEG or Capability.CODEC_H264,
              decoderMaxWidth: Int = width, decoderMaxHeight: Int = height): ByteArray =
    ByteBuffer.allocate(Protocol.HEADER_SIZE + Protocol.HELLO_SIZE).order(ByteOrder.LITTLE_ENDIAN).apply {
        putInt(Protocol.MAGIC); putShort(MessageType.HELLO.toShort()); putShort(0)
        putInt(Protocol.HELLO_SIZE)
        putShort(Protocol.VERSION.toShort()); putShort(0)
        putInt(width); putInt(height); putInt(fps); putInt(dpi)
        putInt(decoderMaxWidth); putInt(decoderMaxHeight); putInt(capabilities)
    }.array()

fun sendHello(out: OutputStream, width: Int, height: Int, fps: Int, dpi: Int,
              capabilities: Int = Capability.CODEC_JPEG or Capability.CODEC_H264,
              decoderMaxWidth: Int = width, decoderMaxHeight: Int = height) {
    out.write(makeHello(width, height, fps, dpi, capabilities, decoderMaxWidth, decoderMaxHeight))
    out.flush()
}
fun parseHelloAck(bytes: ByteArray, length: Int): HelloAck {
    if (length != Protocol.ACK_SIZE) throw ProtocolException("HELLO_ACK 长度必须为 32")
    val b = little(bytes, length)
    val version = b.u16(); val result = b.u16(); val codec = b.u8()
    b.get()
    val fps = b.u16(); val width = b.int; val height = b.int
    val ack = HelloAck(version, result, codec, fps, width, height, b.u32(), b.u32(), b.long)
    if (version != Protocol.VERSION) throw ProtocolException("协议版本不支持，请升级客户端")
    if (result != 0) throw ProtocolException(when (result) {
        1 -> "协议版本不支持，请升级客户端"
        2 -> "服务端无共同编码格式"
        3 -> "分辨率超出服务端能力"
        else -> "服务端拒绝连接: $result"
    })
    if ((codec != Codec.JPEG && codec != Codec.H264) || fps == 0 || width <= 0 || height <= 0)
        throw ProtocolException("无效的会话参数")
    return ack
}
fun parseFrameInfo(bytes: ByteArray, length: Int, ack: HelloAck): FrameInfo {
    if (length < Protocol.FRAME_INFO_SIZE || length > Protocol.MAX_PAYLOAD)
        throw ProtocolException("VIDEO_FRAME 长度无效")
    val b = little(bytes, length)
    val id = b.u32(); val codec = b.u8(); val flags = b.u8()
    b.short
    val width = b.int; val height = b.int; val timestamp = b.long; val size = b.int
    if (size <= 0 || size != length - Protocol.FRAME_INFO_SIZE)
        throw ProtocolException("payload_size != header.length - 28")
    if (codec != ack.codec || width != ack.width || height != ack.height)
        throw ProtocolException("帧参数与 HELLO_ACK 不符")
    if (id == 0L) throw ProtocolException("frame_id 必须从 1 开始")
    return FrameInfo(id, codec, flags, width, height, timestamp, size)
}

class MessageReader(private val input: InputStream) {
    private val header = ByteBuffer.allocate(Protocol.HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
    // Reused across all messages; the decoder consumes it synchronously before the next read.
    val payload = ByteArray(Protocol.MAX_PAYLOAD)
    data class Header(val type: Int, val flags: Int, val length: Int)

    fun readHeader(): Header? {
        header.clear()
        if (!readFully(header.array(), Protocol.HEADER_SIZE, allowCleanEof = true)) return null
        if (header.int != Protocol.MAGIC) throw ProtocolException("非法 magic")
        val type = header.u16(); val flags = header.u16(); val length = header.int
        if (length < 0 || length > Protocol.MAX_PAYLOAD) throw ProtocolException("非法载荷长度: $length")
        return Header(type, flags, length)
    }
    fun readPayload(length: Int): ByteArray {
        if (length < 0 || length > Protocol.MAX_PAYLOAD) throw ProtocolException("非法载荷长度: $length")
        readFully(payload, length)
        return payload
    }
    private fun readFully(dst: ByteArray, length: Int, allowCleanEof: Boolean = false): Boolean {
        var got = 0
        while (got < length) {
            val count = input.read(dst, got, length - got)
            if (count < 0) {
                if (allowCleanEof && got == 0) return false
                throw EOFException("消息中途断开 ($got/$length)")
            }
            if (count == 0) {
                // InputStream implementations should not return zero for nonempty reads.
                val one = input.read()
                if (one < 0) throw EOFException("消息中途断开")
                dst[got++] = one.toByte()
            } else got += count
        }
        return true
    }
}
