package com.secondaryscreen.client

object ResolutionOptions {
    fun candidates(nativeW: Int, nativeH: Int): List<Pair<Int, Int>> {
        val sizes = mutableListOf(0 to 0, nativeW to nativeH)
        val portrait = nativeH > nativeW
        for ((w, h) in listOf(2944 to 1840, 2560 to 1600, 1920 to 1200,
            1600 to 1000, 1280 to 800, 1920 to 1080, 1280 to 720)) {
            val candidate = if (portrait) h to w else w to h
            if (candidate.first <= nativeW && candidate.second <= nativeH && candidate !in sizes)
                sizes.add(candidate)
        }
        return sizes
    }

    fun normalizedOverride(width: Int, height: Int, nativeW: Int, nativeH: Int): Pair<Int, Int> =
        (width to height).takeIf { it in candidates(nativeW, nativeH) } ?: (0 to 0)
}
