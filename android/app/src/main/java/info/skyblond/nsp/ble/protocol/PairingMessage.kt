package info.skyblond.nsp.ble.protocol

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * A 17-byte pairing message used in the Nikon smart-device handshake.
 *
 * Layout (little-endian on the wire):
 *   byte 0       stage
 *   bytes 1-8    timestamp (uint64)
 *   bytes 9-12   device (uint32)
 *   bytes 13-16  nonce (uint32)
 */
data class PairingMessage(
    val stage: Int,
    val timestamp: Long,
    val device: Long,
    val nonce: Long
) {
    fun encode(): ByteArray {
        val buffer = ByteBuffer.allocate(17).order(ByteOrder.LITTLE_ENDIAN)
        buffer.put(stage.toByte())
        buffer.putLong(timestamp)
        // SnapBridge stores the 8-byte DeviceID as a raw byte array in the wire
        // format (camera advertises its first 4 bytes, e.g. 44 5d 4b 24). Our
        // uint32 fields are little-endian, so reverse each 4-byte field to keep
        // the original byte order on the wire.
        buffer.putInt((device and 0xFFFFFFFFL).toInt())
        buffer.putInt((nonce and 0xFFFFFFFFL).toInt())
        return buffer.array()
    }

    companion object {
        fun decode(data: ByteArray): PairingMessage {
            require(data.size == 17) { "Pairing message must be exactly 17 bytes, got ${data.size}" }
            val buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
            val stage = buffer.get().toInt() and 0xFF
            val timestamp = buffer.getLong()
            val device = buffer.getInt().toLong() and 0xFFFFFFFFL
            val nonce = buffer.getInt().toLong() and 0xFFFFFFFFL
            return PairingMessage(stage, timestamp, device, nonce)
        }
    }
}
