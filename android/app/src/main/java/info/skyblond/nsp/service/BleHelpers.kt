package info.skyblond.nsp.service

import android.bluetooth.le.ScanRecord
import android.os.Build
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale
import java.util.Random

/** Small, dependency-free helpers shared by the pairing session and the service. */
object BleHelpers {
    const val DEFAULT_CONTROLLER_NAME = "nsg-poc"

    /**
     * Case-insensitive exact match, or one name being a prefix of the other (some ROMs
     * truncate device names over classic discovery). A bare contains() check could match
     * an unrelated device whose name merely embeds the target string.
     */
    fun namesMatch(foundName: String?, targetName: String?): Boolean {
        if (foundName.isNullOrBlank() || targetName.isNullOrBlank()) return false
        val a = foundName.trim().lowercase(Locale.ROOT)
        val b = targetName.trim().lowercase(Locale.ROOT)
        val (shorter, longer) = if (a.length <= b.length) a to b else b to a
        return a == b || (shorter.length >= 4 && longer.startsWith(shorter))
    }

    /**
     * SnapBridge-style controller name: "Android_" + sanitized Build.MODEL + "_%04d".
     */
    fun generateSnapBridgeControllerName(): String {
        val model = Build.MODEL.replace(Regex("[^\\x21-\\x7e]"), "_")
        val suffix = String.format(Locale.US, "_%04d", Random().nextInt(10000))
        val clientName = if (model.length > 31 - suffix.length) {
            model.substring(0, 31 - suffix.length) + suffix
        } else {
            model + suffix
        }
        return "Android_" + clientName
    }

    fun formatTimestamp(ms: Long): String {
        return try {
            Instant.ofEpochMilli(ms)
                .atZone(ZoneId.systemDefault())
                .format(DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss"))
        } catch (_: Exception) {
            ms.toString()
        }
    }

    fun formatManufacturerData(record: ScanRecord?): String {
        val data = record?.manufacturerSpecificData ?: return ""
        if (data.size() == 0) return "none"
        val sb = StringBuilder()
        for (i in 0 until data.size()) {
            val key = data.keyAt(i)
            val bytes = data.get(key) ?: continue
            sb.append("company=0x%04X ".format(key))
            sb.append(bytes.joinToString(" ") { "%02x".format(it) })
            if (i < data.size() - 1) sb.append("; ")
        }
        return sb.toString()
    }

    /**
     * Nikon cameras advertise the controller device ID they expect in manufacturer data:
     * company 0x0399 (Nikon) + 4-byte little-endian device ID. Returns null when absent.
     */
    fun extractAdvertisedDeviceId(record: ScanRecord?): Long? {
        val data = record?.manufacturerSpecificData ?: return null
        for (i in 0 until data.size()) {
            if (data.keyAt(i) != 0x0399) continue
            val bytes = data.get(data.keyAt(i)) ?: continue
            if (bytes.size < 4) return null
            return ((bytes[0].toLong() and 0xFF) or
                    ((bytes[1].toLong() and 0xFF) shl 8) or
                    ((bytes[2].toLong() and 0xFF) shl 16) or
                    ((bytes[3].toLong() and 0xFF) shl 24)) and 0xFFFFFFFFL
        }
        return null
    }

    /** Parses the flattened manufacturer data held by [info.skyblond.nsp.data.DiscoveredCamera]. */
    fun extractAdvertisedDeviceId(data: ByteArray?): Long? {
        if (data == null || data.size < 6) return null
        val company = (data[0].toLong() and 0xFF) or ((data[1].toLong() and 0xFF) shl 8)
        if (company != 0x0399L) return null
        return ((data[2].toLong() and 0xFF) or
                ((data[3].toLong() and 0xFF) shl 8) or
                ((data[4].toLong() and 0xFF) shl 16) or
                ((data[5].toLong() and 0xFF) shl 24)) and 0xFFFFFFFFL
    }
}

fun ByteArray.toHex(): String =
    joinToString(" ") { "%02x".format(it) }

fun Long.toHexString(): String =
    "0x%016x".format(this)
