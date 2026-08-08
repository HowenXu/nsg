package info.skyblond.nsp.ble.protocol

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class SnapBridgeIdSolverTest {

    @Test
    fun nextBytesMatchesVerifiedJavaOutput() {
        // Verified on a JVM: new Random(1785587115763L).nextBytes(8)
        assertEquals(
            "48ee2fc1f03fcf17",
            SnapBridgeIdSolver.nextBytes(1785587115763L, 8).toHexString()
        )
    }

    @Test
    fun solvedSeedsContainTheRealPairingSeed() {
        // The camera advertises 44 5d 4b 24 (LE uint32 = 0x244B5D44). The seed that
        // actually produced the working DeviceID was the first SnapBridge launch on
        // the user's phone: 2026-04-25 20:37:21 = 1777120641934.
        val seeds = SnapBridgeIdSolver.solveSeeds(0x244B5D44L)
        assertTrue(seeds.contains(1777120641934L))
        assertTrue(seeds.contains(1782432516591L))
    }

    @Test
    fun candidatesFilteredByTimeWindow() {
        val candidates = SnapBridgeIdSolver.candidatesFor(
            targetDevice = 0x244B5D44L,
            windowStartMs = 1777120641934L - 1_000,
            windowEndMs = 1777120641934L + 1_000
        )
        assertEquals(listOf(1777120641934L), candidates.map { it.seed })
        assertEquals("445d4b24981064f7", candidates.single().deviceIdHex)
        assertEquals("445D4B24981064F7", candidates.single().fixedIdentityHex)
    }

    @Test
    fun candidateOrderIsChronological() {
        val candidates = SnapBridgeIdSolver.candidatesFor(
            targetDevice = 0x244B5D44L,
            windowStartMs = 1_700_000_000_000L,
            windowEndMs = 1_800_000_000_000L
        )
        val seeds = candidates.map { it.seed }
        assertEquals(seeds.sorted(), seeds)
        assertTrue(seeds.isNotEmpty())
    }

    private fun ByteArray.toHexString(): String = joinToString("") { "%02x".format(it) }
}
