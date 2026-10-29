package com.padscreen.client

fun displayValuesSelfTest() {
    val unavailable: () -> Pair<Int, Int> = { error("not available") }
    check(DisplayValues.readSize(30, { 1840 to 2944 }, unavailable) == (2944 to 1840))
    check(DisplayValues.readSize(29, unavailable, { 1840 to 2944 }) == (2944 to 1840))
    check(DisplayValues.readSize(26, unavailable, { 1920 to 1200 }) == (1920 to 1200))
    check(DisplayValues.readSize(34, unavailable, { 2560 to 1600 }) == (2560 to 1600))
    check(DisplayValues.readSize(34, { 0 to 0 }, { 1920 to 1200 }) == (1920 to 1200))
    check(DisplayValues.readSize(34, unavailable, unavailable) == (1280 to 800))
    check(DisplayValues.readRefresh(30, { 143.85f }, { error("wrong API") }) == 144)
    check(DisplayValues.readRefresh(29, { error("wrong API") }, { 59.94f }) == 60)
    for (value in listOf(Float.NaN, Float.POSITIVE_INFINITY, 0f, -1f))
        check(DisplayValues.readRefresh(34, { value }, { 144f }) == 60)
    check(DisplayValues.readRefresh(34, { 20f }, { 60f }) == 30)
    check(DisplayValues.readRefresh(34, { 240f }, { 60f }) == 165)
    check(DisplayValues.readRefresh(34, { error("display disconnected") }, { 144f }) == 60)
    val hello = makeHello(2944, 1840, 144, 320, Capability.CODEC_JPEG or Capability.CODEC_H264, 4096, 4096)
    val fields = java.nio.ByteBuffer.wrap(hello).order(java.nio.ByteOrder.LITTLE_ENDIAN)
    check(fields.getInt(16) == 2944 && fields.getInt(20) == 1840 && fields.getInt(24) == 144)
    check(fields.getInt(32) == 4096 && fields.getInt(36) == 4096)
    println("PASS stage 6: API 26/29/30+ display policy, fallback, landscape, refresh rounding/clamp and unchanged HELLO fields")
}
