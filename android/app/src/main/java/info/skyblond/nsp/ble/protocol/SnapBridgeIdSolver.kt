package info.skyblond.nsp.ble.protocol

import java.math.BigInteger

/**
 * Recovers the 8-byte controller DeviceID that a Nikon camera has stored.
 *
 * Nikon SnapBridge generates its controller DeviceID exactly once per install:
 * ```
 * if (!prefs.contains("DeviceID")) {
 *     byte[] id = new byte[8];
 *     new Random(new Date().getTime()).nextBytes(id);   // seed = Unix ms timestamp
 *     prefs.put("DeviceID", id);
 * }
 * ```
 * `java.util.Random.nextBytes()` fills little-endian, and the camera advertises the
 * first 4 bytes of the DeviceID after the Nikon company id (0x0399) in its BLE
 * manufacturer data.
 *
 * Given the advertised 4 bytes (little-endian uint32) we invert the LCG: a seed
 * whose first `nextInt()` equals that value must satisfy a 48-bit modular equation,
 * which has exactly 2^16 solutions. Filtering those solutions to plausible
 * timestamps (SnapBridge install time .. now) leaves a handful of candidates that
 * can be verified by connecting to the camera.
 */
object SnapBridgeIdSolver {

    /** One candidate seed (a Unix millisecond timestamp) with its 8-byte DeviceID. */
    data class Candidate(val seed: Long, val deviceIdHex: String) {
        /** 16-hex-digit identity accepted by the app's fixed-identity field. */
        val fixedIdentityHex: String get() = deviceIdHex.uppercase()
    }

    private const val MASK48 = (1L shl 48) - 1L
    private const val MULTIPLIER = 0x5DEECE66DL
    private const val ADDEND = 0xBL

    /**
     * All 48-bit seeds whose first `nextInt()` equals [targetDevice]
     * (the little-endian uint32 advertised by the camera).
     */
    fun solveSeeds(targetDevice: Long): List<Long> {
        val target = targetDevice and 0xFFFFFFFFL
        val modulus = 1L shl 48
        val inverse = BigInteger.valueOf(MULTIPLIER)
            .modInverse(BigInteger.valueOf(modulus))
            .toLong()
        val seeds = ArrayList<Long>(1 shl 16)
        for (low16 in 0 until (1 shl 16)) {
            // first nextInt() == (state >> 16) & 0xFFFFFFFF where
            // state = ((seed xor MULT) * MULT + ADD) mod 2^48
            val state = (target shl 16) or low16.toLong()
            val x = (((state - ADDEND) and MASK48) * inverse) and MASK48
            seeds.add(x xor MULTIPLIER)
        }
        return seeds
    }

    /** Emulates `java.util.Random.nextBytes(n)` for the given seed. */
    fun nextBytes(seed: Long, n: Int): ByteArray {
        var state = (seed xor MULTIPLIER) and MASK48
        val out = ByteArray(n)
        var i = 0
        while (i < n) {
            state = (state * MULTIPLIER + ADDEND) and MASK48
            val rnd = (state shr 16) and 0xFFFFFFFFL
            val count = minOf(4, n - i)
            for (j in 0 until count) {
                out[i] = ((rnd shr (8 * j)) and 0xFF).toByte()
                i++
            }
        }
        return out
    }

    /**
     * Candidates whose seed lies in `[windowStartMs, windowEndMs]`, sorted ascending.
     * [targetDevice] is the little-endian uint32 advertised by the camera.
     */
    fun candidatesFor(
        targetDevice: Long,
        windowStartMs: Long,
        windowEndMs: Long
    ): List<Candidate> =
        solveSeeds(targetDevice)
            .asSequence()
            .filter { it in windowStartMs..windowEndMs }
            .sorted()
            .map { Candidate(it, nextBytes(it, 8).toHexString()) }
            .toList()

    private fun ByteArray.toHexString(): String = joinToString("") { "%02x".format(it) }
}
