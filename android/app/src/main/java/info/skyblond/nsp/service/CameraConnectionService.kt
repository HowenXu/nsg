package info.skyblond.nsp.service

import android.Manifest
import android.annotation.SuppressLint
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.os.Looper
import android.util.Log
import info.skyblond.nsp.ble.GeoPayloadGenerator
import info.skyblond.nsp.data.DiscoveredCamera
import info.skyblond.nsp.data.PairedCamera
import info.skyblond.nsp.data.SettingsRepository
import info.skyblond.nsp.ui.L10n
import java.time.ZoneOffset
import java.time.ZonedDateTime
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

private const val TAG = "CameraConnectionService"

/**
 * Foreground service shell: owns the lifecycle, the observable state, location
 * tracking and the startup watchdog. All Bluetooth pairing logic lives in
 * [NikonPairingSession].
 */
@SuppressLint("MissingPermission")
class CameraConnectionService : Service(), NikonPairingSession.Host {

    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    private val binder = LocalBinder()

    private lateinit var settingsRepository: SettingsRepository
    private lateinit var pairingSession: NikonPairingSession

    private var startupReconnectTimeoutJob: kotlinx.coroutines.Job? = null
    private var keepAliveJob: kotlinx.coroutines.Job? = null
    private var geoTimeoutJob: kotlinx.coroutines.Job? = null
    private var lastActivityTime = System.currentTimeMillis()

    private var locationManager: LocationManager? = null
    private var lastLocation: Location? = null
    private var lastSentLocation: Location? = null
    private var lastSentTime: Long = 0L

    private val _state = MutableStateFlow<ConnectionState>(ConnectionState.Idle)
    val state: StateFlow<ConnectionState> = _state.asStateFlow()

    private val _events = MutableSharedFlow<String>(extraBufferCapacity = 10)
    val events: MutableSharedFlow<String> = _events

    private val _gpsState = MutableStateFlow(GpsState())
    val gpsState: StateFlow<GpsState> = _gpsState.asStateFlow()

    private val _savedCameras = MutableStateFlow<List<PairedCamera>>(emptyList())
    val savedCameras: StateFlow<List<PairedCamera>> = _savedCameras.asStateFlow()

    private val _defaultCameraName = MutableStateFlow<String?>(null)
    val defaultCameraName: StateFlow<String?> = _defaultCameraName.asStateFlow()

    val discoveredCameras: StateFlow<List<DiscoveredCamera>>
        get() = pairingSession.discoveredCameras

    private val locationListener = object : LocationListener {
        override fun onLocationChanged(location: Location) {
            handleLocation(location)
        }
    }

    override fun onCreate() {
        super.onCreate()
        settingsRepository = SettingsRepository(this)
        pairingSession = NikonPairingSession(this, serviceScope, settingsRepository, this)
        pairingSession.registerReceivers()
        _defaultCameraName.value = settingsRepository.defaultConnectCameraName()
        refreshSavedCameras()
        NotificationHelper.createChannel(this)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == NotificationHelper.ACTION_DISCONNECT) {
            disconnect()
            stopSelf()
            return START_NOT_STICKY
        }
        startForegroundCompat()
        autoReconnectLastCamera()
        return START_STICKY
    }

    override fun onBind(intent: Intent): IBinder = binder

    override fun onTaskRemoved(rootIntent: Intent?) {
        super.onTaskRemoved(rootIntent)
        try {
            startService(Intent(this, CameraConnectionService::class.java))
        } catch (_: Exception) {
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        pairingSession.dispose()
        keepAliveJob?.cancel()
        stopLocationUpdates()
        serviceScope.cancel()
    }

    // -------------------------------------------------------------------------
    // NikonPairingSession.Host
    // -------------------------------------------------------------------------

    override fun currentState(): ConnectionState = state.value

    override fun updateState(state: ConnectionState) {
        updateServiceState(state)
    }

    override fun log(message: String) {
        logEvent(message)
    }

    override fun markActivity() {
        lastActivityTime = System.currentTimeMillis()
    }

    override fun refreshSavedCameras() {
        _savedCameras.value = settingsRepository.loadSavedCameras()
    }

    override fun cancelStartupReconnectTimeout() {
        startupReconnectTimeoutJob?.cancel()
        startupReconnectTimeoutJob = null
    }

    override fun onSessionReady() {
        startLocationUpdates()
        startKeepAlive()
    }

    // -------------------------------------------------------------------------
    // Public commands (delegated to the pairing session)
    // -------------------------------------------------------------------------

    fun startPairingScan() = pairingSession.startPairingScan()

    fun stopScan() = pairingSession.stopScan()

    fun selectDiscoveredCamera(camera: DiscoveredCamera) = pairingSession.selectDiscoveredCamera(camera)

    fun connectToSavedCamera(camera: PairedCamera) = pairingSession.connectToSavedCamera(camera)

    fun startAutoExtract(camera: PairedCamera) = pairingSession.startAutoExtract(camera)

    fun disconnect() {
        serviceScope.launch {
            pairingSession.disconnect()
            startupReconnectTimeoutJob?.cancel()
            startupReconnectTimeoutJob = null
            geoTimeoutJob?.cancel()
            updateServiceState(ConnectionState.Idle)
            keepAliveJob?.cancel()
            stopLocationUpdates()
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
        }
    }

    fun sendGeoOnce() {
        serviceScope.launch {
            if (state.value != ConnectionState.Ready) {
                logEvent(L10n.t("未就绪 - 无法发送 GPS", "Not ready - cannot send GPS"))
                return@launch
            }
            val loc = lastLocation
            if (loc != null) {
                sendGeo(loc)
                logEvent(L10n.t("已手动发送 GPS", "GPS sent manually"))
            } else {
                val payload = GeoPayloadGenerator.buildFake()
                pairingSession.writeGeo(payload)
                updateServiceState(ConnectionState.Busy)
                logEvent(L10n.t("暂无定位，发送兜底数据", "No fix yet; sending fallback data"))
            }
        }
    }

    /** Set or clear the startup default camera. */
    fun setDefaultCamera(camera: PairedCamera) {
        serviceScope.launch {
            val isDefault = settingsRepository.defaultConnectCameraName() == camera.name
            if (isDefault) {
                settingsRepository.setDefaultConnectCameraName(null)
                logEvent(L10n.t("已取消 ${camera.name} 的启动默认连接", "Removed ${camera.name} as the startup default"))
            } else {
                settingsRepository.setDefaultConnectCameraName(camera.name)
                logEvent(L10n.t("已将 ${camera.name} 设为启动时默认连接", "Set ${camera.name} as the startup default"))
            }
            _defaultCameraName.value = settingsRepository.defaultConnectCameraName()
        }
    }

    /** Delete a camera from the saved list. */
    fun deleteCamera(camera: PairedCamera) {
        serviceScope.launch {
            settingsRepository.removeCamera(camera)
            _defaultCameraName.value = settingsRepository.defaultConnectCameraName()
            refreshSavedCameras()
            logEvent(L10n.t("已删除 ${camera.name}", "Deleted ${camera.name}"))
        }
    }

    // -------------------------------------------------------------------------
    // Location / GPS
    // -------------------------------------------------------------------------

    private fun handleLocation(location: Location) {
        lastLocation = location
        _gpsState.update {
            it.copy(
                hasFix = true,
                latitude = location.latitude,
                longitude = location.longitude,
                accuracyMeters = location.accuracy,
                lastFixTime = System.currentTimeMillis()
            )
        }
        maybeSendGeo(location)
    }

    private fun maybeSendGeo(location: Location) {
        if (state.value != ConnectionState.Ready) return
        val now = System.currentTimeMillis()
        val last = lastSentLocation
        val distance = if (last != null) location.distanceTo(last) else Float.MAX_VALUE
        // Skip sending when the position barely moved and we sent recently (low-power).
        if (last == null || distance > MIN_SEND_DISTANCE_M || now - lastSentTime > MAX_SEND_INTERVAL_MS) {
            sendGeo(location)
        }
    }

    private fun sendGeo(location: Location) {
        if (state.value != ConnectionState.Ready) return
        val payload = GeoPayloadGenerator.build(
            latitude = location.latitude,
            longitude = location.longitude,
            altitude = location.altitude,
            timestamp = ZonedDateTime.now(ZoneOffset.UTC)
        )
        pairingSession.writeGeo(payload)
        lastSentLocation = location
        lastSentTime = System.currentTimeMillis()
        _gpsState.update { it.copy(lastSentTime = lastSentTime) }
        updateServiceState(ConnectionState.Busy)
        logEvent(L10n.t("已发送 GPS (%.5f, %.5f)", "GPS sent (%.5f, %.5f)").format(location.latitude, location.longitude))
        geoTimeoutJob?.cancel()
        geoTimeoutJob = serviceScope.launch {
            delay(10_000)
            if (state.value is ConnectionState.Busy) {
                updateServiceState(ConnectionState.Ready)
                logEvent(L10n.t("GPS 写入超时，已恢复就绪", "GPS write timed out; back to ready"))
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun startLocationUpdates() {
        val lm = getSystemService(Context.LOCATION_SERVICE) as? LocationManager ?: return
        locationManager = lm
        val hasGps = lm.getProvider(LocationManager.GPS_PROVIDER) != null
        val hasNetwork = lm.getProvider(LocationManager.NETWORK_PROVIDER) != null
        if (!hasGps && !hasNetwork) {
            _gpsState.value = GpsState()
            logEvent(L10n.t("没有可用的定位源", "No location source available"))
            return
        }
        _gpsState.update { it.copy(enabled = true) }
        try {
            if (hasGps) {
                lm.requestLocationUpdates(
                    LocationManager.GPS_PROVIDER,
                    GPS_UPDATE_INTERVAL_MS,
                    GPS_UPDATE_MIN_DISTANCE_M,
                    locationListener,
                    Looper.getMainLooper()
                )
            }
            if (hasNetwork) {
                lm.requestLocationUpdates(
                    LocationManager.NETWORK_PROVIDER,
                    NETWORK_UPDATE_INTERVAL_MS,
                    GPS_UPDATE_MIN_DISTANCE_M,
                    locationListener,
                    Looper.getMainLooper()
                )
            }
            logEvent(L10n.t("已开启定位追踪（省电模式）", "Location tracking enabled (battery saver)"))
        } catch (e: SecurityException) {
            logEvent(L10n.t("缺少定位权限，无法获取 GPS", "Missing location permission; cannot get GPS"))
        }
    }

    private fun stopLocationUpdates() {
        try {
            locationManager?.removeUpdates(locationListener)
        } catch (_: Exception) {
        }
        locationManager = null
        _gpsState.value = GpsState()
    }

    private fun startKeepAlive() {
        keepAliveJob?.cancel()
        keepAliveJob = serviceScope.launch {
            while (isActive) {
                delay(30_000)
                val loc = lastLocation
                if (loc != null &&
                    state.value == ConnectionState.Ready &&
                    System.currentTimeMillis() - lastSentTime > 25_000
                ) {
                    sendGeo(loc)
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Startup auto-reconnect
    // -------------------------------------------------------------------------

    private fun autoReconnectLastCamera() {
        if (state.value != ConnectionState.Idle) return
        if (!hasBluetoothPermission()) return
        val cameras = settingsRepository.loadSavedCameras()
        val defaultName = settingsRepository.defaultConnectCameraName()
        val camera = defaultName?.let { name -> cameras.firstOrNull { it.name == name } }
            ?: if (cameras.size == 1) cameras.first() else null
            ?: run {
                if (cameras.size > 1) {
                    logEvent(L10n.t("已保存多台相机且未设置启动默认连接，跳过自动连接", "Multiple cameras saved with no startup default; skipping auto-connect"))
                }
                return
            }
        logEvent(L10n.t("服务重启，正在重连 ${camera.name}（10 秒超时）", "Service restarted; reconnecting ${camera.name} (10s timeout)"))
        startupReconnectTimeoutJob?.cancel()
        startupReconnectTimeoutJob = serviceScope.launch {
            while (true) {
                delay(1_000)
                val current = state.value
                if (current is ConnectionState.Ready ||
                    current is ConnectionState.Busy ||
                    current is ConnectionState.Error
                ) {
                    break
                }
                // Kill only when there has been no progress for 10s (the watchdog resets on
                // every state change or reconnect-scan round), so slow scans are not cut short.
                if (System.currentTimeMillis() - lastActivityTime > 10_000) {
                    Log.w(TAG, "Startup reconnect timed out (state=$current), stopping")
                    logEvent(L10n.t("10 秒内无进展，已停止自动连接", "No progress for 10s; auto-connect stopped"))
                    disconnect()
                    break
                }
            }
        }
        connectToSavedCamera(camera)
    }

    private fun hasBluetoothPermission(): Boolean {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
    }

    // -------------------------------------------------------------------------
    // Foreground service
    // -------------------------------------------------------------------------

    /**
     * Start the foreground service without crashing on Android 14+ when the
     * runtime permission for a declared type has not been granted yet: the
     * location type is only attached when the location permission is held, and
     * any SecurityException falls back to the bare connectedDevice type before
     * giving up.
     */
    private fun startForegroundCompat() {
        val notification = NotificationHelper.build(this, state.value.label)
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            startForeground(NotificationHelper.NOTIFICATION_ID, notification)
            return
        }
        var type = ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
        if (hasLocationPermission()) {
            type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION
        }
        try {
            startForeground(NotificationHelper.NOTIFICATION_ID, notification, type)
        } catch (e: SecurityException) {
            Log.w(TAG, "startForeground(type=$type) failed: ${e.message}")
            try {
                startForeground(
                    NotificationHelper.NOTIFICATION_ID,
                    notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
                )
            } catch (e2: SecurityException) {
                Log.w(TAG, "startForeground(connectedDevice) failed too: ${e2.message}")
                stopSelf()
            }
        }
    }

    private fun hasLocationPermission(): Boolean {
        val fine = checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION)
        val coarse = checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION)
        return fine == PackageManager.PERMISSION_GRANTED ||
            coarse == PackageManager.PERMISSION_GRANTED
    }

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    private fun updateServiceState(newState: ConnectionState) {
        _state.value = newState
        lastActivityTime = System.currentTimeMillis()
        NotificationHelper.update(this, newState.label)
    }

    private fun logEvent(message: String) {
        _events.tryEmit(message)
    }

    inner class LocalBinder : Binder() {
        val service: CameraConnectionService
            get() = this@CameraConnectionService
    }

    companion object {
        private const val GPS_UPDATE_INTERVAL_MS = 5_000L
        private const val NETWORK_UPDATE_INTERVAL_MS = 15_000L
        private const val GPS_UPDATE_MIN_DISTANCE_M = 1f
        private const val MIN_SEND_DISTANCE_M = 3f
        private const val MAX_SEND_INTERVAL_MS = 20_000L
    }
}
