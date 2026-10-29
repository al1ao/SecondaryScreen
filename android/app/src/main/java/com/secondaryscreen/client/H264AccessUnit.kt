package com.secondaryscreen.client

import java.net.ProtocolException

/** Pure bitstream parsing; does not depend on Android so malformed samples can be tested on the JVM. */
object H264AccessUnit {
    data class Nal(val offset: Int, val length: Int, val type: Int)
    fun parse(bytes: ByteArray, offset: Int = 0, length: Int = bytes.size - offset): List<Nal> {
        if (offset < 0 || length <= 0 || offset > bytes.size - length) throw ProtocolException("H264 范围无效")
        val end = offset + length
        fun prefix(at: Int): Int {
            if (at + 3 <= end && bytes[at] == 0.toByte() && bytes[at + 1] == 0.toByte()) {
                if (bytes[at + 2] == 1.toByte()) return 3
                if (at + 4 <= end && bytes[at + 2] == 0.toByte() && bytes[at + 3] == 1.toByte()) return 4
            }
            return 0
        }
        val result = mutableListOf<Nal>()
        var at = offset
        while (at < end) {
            val start = prefix(at)
            if (start == 0) throw ProtocolException("H264 缺少 Annex-B 起始码")
            val begin = at + start
            at = begin
            while (at < end && prefix(at) == 0) at++
            var tail = at
            while (tail > begin && bytes[tail - 1] == 0.toByte()) tail--
            if (tail == begin || (bytes[begin].toInt() and 0x80) != 0) throw ProtocolException("无效 H264 NAL")
            result += Nal(begin, tail - begin, bytes[begin].toInt() and 31)
        }
        return result
    }
    fun validate(bytes: ByteArray, keyframe: Boolean): List<Nal> {
        val nals = parse(bytes)
        if (nals.any { it.type == 5 } != keyframe) throw ProtocolException("关键帧标志与 IDR 不符")
        if (keyframe && (!nals.any { it.type == 7 } || !nals.any { it.type == 8 }))
            throw ProtocolException("关键帧缺少 SPS/PPS")
        return nals
    }
    fun join(bytes: ByteArray, nals: List<Nal>): ByteArray {
        val result = ByteArray(nals.sumOf { it.length + 4 })
        var at = 0
        for (nal in nals) {
            result[at + 3] = 1; at += 4
            bytes.copyInto(result, at, nal.offset, nal.offset + nal.length); at += nal.length
        }
        return result
    }
}
