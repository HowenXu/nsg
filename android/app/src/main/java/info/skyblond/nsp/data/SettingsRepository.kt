package info.skyblond.nsp.data

import android.content.Context
import android.content.SharedPreferences

/**
 * Lightweight persistence for paired cameras. Stores a JSON string per camera.
 */
class SettingsRepository(context: Context) {

    private val prefs: SharedPreferences =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun loadSavedCameras(): List<PairedCamera> {
        return prefs.getStringSet(KEY_CAMERAS, emptySet())?.map {
            PairedCamera.fromJson(it)
        }?.sortedBy { it.name } ?: emptyList()
    }

    fun setLastUsedCamera(name: String) {
        prefs.edit().putString(KEY_LAST_USED, name).apply()
    }

    fun lastUsedCamera(): PairedCamera? {
        val name = prefs.getString(KEY_LAST_USED, null) ?: return null
        return loadSavedCameras().firstOrNull { it.name == name }
    }

    /** Camera name auto-connected at startup; null if none is set. */
    fun defaultConnectCameraName(): String? =
        prefs.getString(KEY_DEFAULT_CONNECT, null)?.takeIf { it.isNotBlank() }

    fun setDefaultConnectCameraName(name: String?) {
        prefs.edit()
            .putString(KEY_DEFAULT_CONNECT, name?.takeIf { it.isNotBlank() })
            .apply()
    }

    fun clearLastUsedCamera() {
        prefs.edit().remove(KEY_LAST_USED).apply()
    }
    fun saveCamera(camera: PairedCamera) {
        val current = loadSavedCameras().toMutableList()
        // The camera name is the stable identifier; the BLE address is random and changes
        // between sessions, so replace any existing entry with the same name.
        current.removeAll { it.name == camera.name }
        current.add(camera)
        setLastUsedCamera(camera.name)
        store(current)
    }

    /**
     * Optional controller name used to disguise this app as another SnapBridge
     * device on the camera. Leave null/blank to use the default name.
     * The camera identifies a paired controller by the 32-byte ASCII name written
     * to the ID characteristic; using the same name as SnapBridge lets the camera
     * treat this app as the same device, so switching apps no longer requires
     * deleting the pairing record on both sides.
     */
    fun spoofControllerName(): String? =
        prefs.getString(KEY_SPOOF_NAME, null)?.takeIf { it.isNotBlank() }

    fun setSpoofControllerName(name: String?) {
        prefs.edit()
            .putString(KEY_SPOOF_NAME, name?.takeIf { it.isNotBlank() })
            .apply()
    }

    /**
     * Optional fixed controller identity (SnapBridge-compatible).
     * Accepts 8 hex digits (device ID only) or 16 hex digits (device + nonce).
     * When set, this identity is used for all connections instead of the
     * camera-advertised or saved ID, so the camera treats this app as the same
     * device as SnapBridge.
     */
    fun fixedIdentity(): FixedIdentity? {
        val raw = prefs.getString(KEY_FIXED_DEVICE_ID, null)?.takeIf { it.isNotBlank() } ?: return null
        val hex = raw.removePrefix("0x").removePrefix("0X")
        if (hex.length == 8) {
            val dev = hex.toLongOrNull(16)?.and(0xFFFFFFFFL) ?: return null
            return FixedIdentity(dev, null)
        }
        if (hex.length == 16) {
            // The 16 hex digits spell out the 8-byte DeviceID byte array in the same order
            // the camera advertises it (e.g. 445D4B24...). Our uint32 fields are stored
            // little-endian on the wire, so reverse each 4-byte half to reproduce those
            // bytes exactly.
            val dev = reverseHex(hex.substring(0, 8)).toLongOrNull(16)?.and(0xFFFFFFFFL) ?: return null
            val nonce = reverseHex(hex.substring(8, 16)).toLongOrNull(16)?.and(0xFFFFFFFFL) ?: return null
            return FixedIdentity(dev, nonce)
        }
        return null
    }

    private fun reverseHex(h: String): String = h.chunked(2).reversed().joinToString("")

    fun fixedDeviceId(): Long? = fixedIdentity()?.device

    fun fixedDeviceIdRaw(): String? =
        prefs.getString(KEY_FIXED_DEVICE_ID, null)?.takeIf { it.isNotBlank() }

    fun setFixedDeviceId(hex: String?) {
        prefs.edit()
            .putString(KEY_FIXED_DEVICE_ID, hex?.takeIf { it.isNotBlank() })
            .apply()
    }

    fun removeCamera(camera: PairedCamera) {
        val current = loadSavedCameras().toMutableList()
        current.removeAll { it.name == camera.name }
        if (defaultConnectCameraName() == camera.name) {
            setDefaultConnectCameraName(null)
        }
        if (lastUsedCamera()?.name == camera.name) {
            clearLastUsedCamera()
        }
        store(current)
    }

    private fun store(cameras: List<PairedCamera>) {
        prefs.edit()
            .putStringSet(KEY_CAMERAS, cameras.map { it.toJson() }.toSet())
            .apply()
    }

    companion object {
        private const val PREFS_NAME = "nsg_camera_prefs"
        private const val KEY_CAMERAS = "paired_cameras"
        private const val KEY_LAST_USED = "last_used_camera"
        private const val KEY_DEFAULT_CONNECT = "default_connect_camera"
        private const val KEY_SPOOF_NAME = "spoof_controller_name"
        private const val KEY_FIXED_DEVICE_ID = "fixed_device_id"
    }
}

/** Fixed controller identity: [device] (4 bytes) and optional [nonce] (4 bytes). */
data class FixedIdentity(val device: Long, val nonce: Long?)
