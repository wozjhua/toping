package com.example.huilangtoupingV3

data class FrameTimingMeta(
    val frameProducedNs: Long,
    val callbackStartNs: Long,
    val encodeStartNs: Long,
    val encodeEndNs: Long,
    val sendStartNs: Long = 0L,
    val sendEndNs: Long = 0L,
    val sendEndWallMs: Long = 0L,
    val sequence: Long = 0L,
)
