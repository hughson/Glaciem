package com.hughson.rime

/**
 * JNI bridge to the native Lattice miner (librime_jni.so, built from
 * lattice_jni.c, which links the canonical pow/lattice_ref.c).
 */
object MinerNative {
    init { System.loadLibrary("rime_jni") }

    /** Lattice hash of a frozen test vector == consensus. Run once at startup. */
    external fun selfTest(): Boolean

    /** (Re)build the 4 MiB epoch dataset. Call while no worker is hashing. */
    external fun setEpochSeed(seed: ByteArray)

    /**
     * Hash `count` nonces from `startNonce`, patched at `nonceOffset` in `blob`.
     * Returns the first nonce whose hash meets `difficulty`, or -1.
     * `outLastHash` (32 bytes) gets the last hash; `outBestBits[0]` is updated
     * with the best leading-zero-bit count. Safe to call from many threads.
     */
    external fun hash(
        blob: ByteArray,
        nonceOffset: Int,
        startNonce: Long,
        count: Int,
        difficulty: Long,
        outLastHash: ByteArray,
        outBestBits: IntArray,
    ): Long

    /**
     * v1.1.9: like [hash], but fills [outWinners] with EVERY nonce that
     * meets `difficulty` (up to outWinners.size). Returns the count
     * actually written. Closes the miner/pool hashrate gap caused by
     * under-reporting batches with 2+ winning nonces.
     */
    external fun hashMulti(
        blob: ByteArray,
        nonceOffset: Int,
        startNonce: Long,
        count: Int,
        difficulty: Long,
        outLastHash: ByteArray,
        outBestBits: IntArray,
        outWinners: LongArray,
    ): Int

    /**
     * Generate a fresh Rime wallet using Rime's own crypto (the keygen
     * library). Returns [address, 25-word recovery seed], or null on failure.
     */
    external fun generateAddress(): Array<String>?
}
