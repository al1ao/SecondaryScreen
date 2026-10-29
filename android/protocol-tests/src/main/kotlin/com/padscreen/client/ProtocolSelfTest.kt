package com.padscreen.client

import java.io.ByteArrayInputStream
import java.io.EOFException
import java.io.InputStream
import java.net.ProtocolException
import java.nio.ByteBuffer
import java.nio.ByteOrder

private fun hex(text: String): ByteArray = text.trim().split(Regex("\\s+")).map { it.toInt(16).toByte() }.toByteArray()
private inline fun <reified T : Throwable> rejects(block: () -> Unit) {
    try { block() } catch (error: Throwable) { check(error is T) { "Expected ${T::class}, got $error" }; return }
    error("Expected ${T::class}")
}
private class Fragmented(private val bytes: ByteArray) : InputStream() {
    private var position = 0
    override fun read(): Int = if (position == bytes.size) -1 else bytes[position++].toInt() and 255
    override fun read(dst: ByteArray, off: Int, len: Int): Int {
        if (position == bytes.size) return -1
        val count = minOf(3, len, bytes.size - position)
        bytes.copyInto(dst, off, position, position + count); position += count
        return count
    }
}
fun main() {
    displayValuesSelfTest()
    val vector = hex("""
        50 44 53 31 01 00 00 00 20 00 00 00
        01 00 00 00 00 05 00 00 20 03 00 00 3C 00 00 00
        40 01 00 00 00 05 00 00 20 03 00 00 01 00 00 00
    """)
    check(makeHello(1280, 800, 60, 320, Capability.CODEC_JPEG).contentEquals(vector))
    val stage2Hello = makeHello(1280, 800, 60, 320)
    check(ByteBuffer.wrap(stage2Hello).order(ByteOrder.LITTLE_ENDIAN).getInt(40) == 3)
    check(stage2Hello.copyOfRange(0, 40).contentEquals(vector.copyOfRange(0, 40)))
    val reader = MessageReader(Fragmented(vector + vector))
    repeat(2) {
        val header = checkNotNull(reader.readHeader())
        check(header.type == MessageType.HELLO && header.length == 32)
        val payload = reader.readPayload(header.length)
        val b = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
        check(b.short.toInt() == 1 && b.short.toInt() == 0)
        check(b.int == 1280 && b.int == 800 && b.int == 60 && b.int == 320)
        check(b.int == 1280 && b.int == 800 && b.int == 1)
        check(payload === reader.payload)
    }
    check(reader.readHeader() == null)
    val ackBytes = ByteBuffer.allocate(32).order(ByteOrder.LITTLE_ENDIAN).apply {
        putShort(1); putShort(0); put(1); put(0); putShort(15)
        putInt(1280); putInt(800); putInt(0); putInt(66666); putLong(1234567890123L)
    }.array()
    val ack = parseHelloAck(ackBytes, 32)
    val h264Ack = parseHelloAck(ackBytes.copyOf().also { it[4] = 2 }, 32)
    check(h264Ack.codec == Codec.H264 && h264Ack.width == ack.width && h264Ack.height == ack.height)
    val keyframe = hex("00 00 00 01 67 42 00 1E 80 00 00 01 68 CE 38 80 00 00 00 01 65 88 84")
    val nals = H264AccessUnit.validate(keyframe, true)
    check(nals.map { it.type } == listOf(7, 8, 5))
    val config = H264AccessUnit.join(keyframe, nals.filter { it.type == 7 || it.type == 8 })
    val picture = H264AccessUnit.join(keyframe, nals.filter { it.type == 5 })
    check(H264AccessUnit.parse(config).map { it.type } == listOf(7, 8))
    check(H264AccessUnit.parse(picture).single().type == 5)
    rejects<ProtocolException> { H264AccessUnit.validate(picture, true) }
    rejects<ProtocolException> { H264AccessUnit.validate(keyframe, false) }
    rejects<ProtocolException> { H264AccessUnit.parse(hex("00 00 00 01")) }
    check(ack.width == 1280 && ack.height == 800 && ack.codec == Codec.JPEG && ack.serverTimeUs == 1234567890123L)
    val frame = ByteBuffer.allocate(32).order(ByteOrder.LITTLE_ENDIAN).apply {
        putInt(1); put(1); put(0); putShort(0); putInt(1280); putInt(800)
        putLong(9876543210L); putInt(4); put(hex("FF D8 FF D9"))
    }.array()
    check(parseFrameInfo(frame, 32, ack).captureTsUs == 9876543210L)
    rejects<ProtocolException> { parseFrameInfo(frame, 31, ack) }
    rejects<ProtocolException> { parseHelloAck(ackBytes, 31) }
    rejects<ProtocolException> { parseFrameInfo(frame, 27, ack) }
    val badMagic = vector.copyOf().also { it[0] = 0 }
    rejects<ProtocolException> { MessageReader(Fragmented(badMagic)).readHeader() }
    for (length in listOf(-1, Protocol.MAX_PAYLOAD + 1)) {
        val badLength = vector.copyOf().also { ByteBuffer.wrap(it).order(ByteOrder.LITTLE_ENDIAN).putInt(8, length) }
        rejects<ProtocolException> { MessageReader(Fragmented(badLength)).readHeader() }
    }
    rejects<EOFException> { MessageReader(Fragmented(vector.copyOf(11))).readHeader() }
    val truncated = MessageReader(Fragmented(vector.copyOf(30)))
    val truncatedHeader = checkNotNull(truncated.readHeader())
    rejects<EOFException> { truncated.readPayload(truncatedHeader.length) }
    val emptyHeader = vector.copyOf(12).also {
        ByteBuffer.wrap(it).order(ByteOrder.LITTLE_ENDIAN).putShort(4, 0x1234).putInt(8, 0)
    }
    val unknown = MessageReader(ByteArrayInputStream(emptyHeader + vector))
    val unknownHeader = checkNotNull(unknown.readHeader())
    check(unknownHeader.type == 0x1234 && unknownHeader.length == 0)
    unknown.readPayload(0)
    check(unknown.readHeader()?.type == MessageType.HELLO)
    val reconnectDelay = ReconnectDelay()
    check(List(8) { reconnectDelay.take() } == listOf(1000L, 2000L, 4000L, 5000L, 5000L, 5000L, 5000L, 5000L))
    reconnectDelay.reset()
    check(reconnectDelay.take() == 1000L)
    println("PASS: exact HELLO vector, ACK/FrameInfo, fragmented/coalesced TCP, reusable buffer, EOF, invalid lengths/magic, unknown types; stage2 caps=3, H264 ACK, Annex-B keyframe and codec-config splitting; stage5 reconnect backoff cap/reset")
}
