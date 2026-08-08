package info.skyblond.nsp.service

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanRecord
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.Manifest
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Looper
import java.time.ZoneOffset
import java.time.ZonedDateTime
import java.util.Locale
import java.util.Random
import kotlinx.coroutines.isActive
import android.os.ParcelUuid
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import info.skyblond.nsp.MainActivity
import info.skyblond.nsp.R
import info.skyblond.nsp.ble.BleEvent
import info.skyblond.nsp.ble.CameraBleManager
import info.skyblond.nsp.ble.GeoPayloadGenerator
import info.skyblond.nsp.ble.protocol.NikonPairingEngine
import info.skyblond.nsp.ble.protocol.PairingMessage
import info.skyblond.nsp.ble.protocol.SnapBridgeIdSolver
import info.skyblond.nsp.data.DiscoveredCamera
import info.skyblond.nsp.data.PairedCamera
import info.skyblond.nsp.data.SettingsRepository
import info.skyblond.nsp.ui.L10n
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
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.launch

private const val TAG = "CameraConnectionService"

@SuppressLint("MissingPermission")
class CameraConnectionService : Service(), CameraBleManager.BleListener {

    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    private val binder = LocalBinder()

    private lateinit var settingsRepository: SettingsRepository
    private val pairingEngine = NikonPairingEngine()

    private val bluetoothManager by lazy { getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager }
    private val bluetoothAdapter by lazy { bluetoothManager.adapter }
    private val scanner by lazy { bluetoothAdapter.bluetoothLeScanner }

    private var bleManager: CameraBleManager? = null
    private var currentDevice: BluetoothDevice? = null
    private var savedCamera: PairedCamera? = null
    private var controllerName: String = DEFAULT_CONTROLLER_NAME

    private var currentStage1: PairingMessage? = null
    private var pairingStep = 0
    private var pairingMode = PairingMode.NEW
    private var idWriteTimeoutJob: kotlinx.coroutines.Job? = null
    private var bondingTimeoutJob: kotlinx.coroutines.Job? = null
    private var discoveryReceiver: BroadcastReceiver? = null
    private var discoveryRestartJob: kotlinx.coroutines.Job? = null
    private var isAwaitingBond: Boolean = false
    private var classicDevice: BluetoothDevice? = null
    private var classicBondComplete: Boolean = false
    private var reBondAfterRemoval: Boolean = false
    private var reconnectScanCallback: ScanCallback? = null
    private var reconnectScanTimeoutJob: kotlinx.coroutines.Job? = null
    private var adoptedDeviceId: Long? = null
    private var lastAdvertisedDevice: Long? = null
    private var stage1RetryCount = 0
    private var reconnectRetryCount = 0
    private var reconnectScanRound = 0
    private var autoExtractActive = false
    private var autoExtractCamera: PairedCamera? = null
    private val autoExtractQueue = ArrayDeque<SnapBridgeIdSolver.Candidate>()
    private var locationManager: LocationManager? = null
    private var lastLocation: Location? = null
    private var lastSentLocation: Location? = null
    private var lastSentTime: Long = 0L
    private var keepAliveJob: kotlinx.coroutines.Job? = null
    private var geoTimeoutJob: kotlinx.coroutines.Job? = null
    private var startupReconnectTimeoutJob: kotlinx.coroutines.Job? = null

    private val _state = MutableStateFlow<ConnectionState>(ConnectionState.Idle)
    val state: StateFlow<ConnectionState> = _state.asStateFlow()

    private val _events = MutableSharedFlow<String>(extraBufferCapacity = 10)
    val events: MutableSharedFlow<String> = _events
    private val _gpsState = MutableStateFlow(GpsState())
    val gpsState: StateFlow<GpsState> = _gpsState.asStateFlow()

    private val _discoveredCameras = MutableStateFlow<List<DiscoveredCamera>>(emptyList())
    val discoveredCameras: StateFlow<List<DiscoveredCamera>> = _discoveredCameras.asStateFlow()

    private val _savedCameras = MutableStateFlow<List<PairedCamera>>(emptyList())
    val savedCameras: StateFlow<List<PairedCamera>> = _savedCameras.asStateFlow()

    private val _defaultCameraName = MutableStateFlow<String?>(null)
    val defaultCameraName: StateFlow<String?> = _defaultCameraName.asStateFlow()

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult?) {
            result ?: return
            logScanResult(result, "ScanResult")
            handleScanResult(result)
        }

        override fun onBatchScanResults(results: MutableList<ScanResult>?) {
            results?.forEach {
                logScanResult(it, "BatchScanResult")
                handleScanResult(it)
            }
        }

        override fun onScanFailed(errorCode: Int) {
            Log.w(TAG, "Scan failed: errorCode=$errorCode")
            logEvent(L10n.t("扫描失败: $errorCode", "Scan failed: $errorCode"))
            updateState(ConnectionState.Error("Scan failed: $errorCode"))
        }
    }

    private val locationListener = object : LocationListener {
        override fun onLocationChanged(location: Location) {
            handleLocation(location)
        }
    }

    private val bondReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                BluetoothDevice.ACTION_BOND_STATE_CHANGED -> {
                    val device = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
                    } else {
                        @Suppress("DEPRECATION")
                        intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
                    }
                    if (device == null) return
                    val isRelevant = device.address == currentDevice?.address || device.address == classicDevice?.address
                    if (!isRelevant) return
                    val bondState = intent.getIntExtra(
                        BluetoothDevice.EXTRA_BOND_STATE,
                        BluetoothDevice.BOND_NONE
                    )
                    val previousBondState = intent.getIntExtra(
                        BluetoothDevice.EXTRA_PREVIOUS_BOND_STATE,
                        BluetoothDevice.BOND_NONE
                    )
                    Log.d(TAG, "Bond state changed for ${device.address}: $previousBondState -> $bondState")
                    when (bondState) {
                        BluetoothDevice.BOND_BONDING -> updateState(ConnectionState.Bonding)
                        BluetoothDevice.BOND_BONDED -> {
                            bondingTimeoutJob?.cancel()
                            if (device.address == classicDevice?.address) {
                                Log.d(TAG, "Classic Bluetooth device bonded; will use it for the BLE connection")
                                classicBondComplete = true
                            }
                            if (isAwaitingBond) {
                                Log.d(TAG, "Bonded while disconnected; reconnecting to complete handshake")
                                isAwaitingBond = false
                                stopClassicDiscovery()
                                reconnectAfterBonding()
                            } else if (pairingStep >= 5) {
                                onBonded()
                            } else {
                                Log.d(TAG, "Bonded before handshake completed, waiting for handshake")
                            }
                        }
                        BluetoothDevice.BOND_NONE -> {
                            if (reBondAfterRemoval) {
                                reBondAfterRemoval = false
                                Log.d(TAG, "Stale bond removed; creating fresh bond now")
                                @Suppress("MissingPermission")
                                val created = device.createBond()
                                Log.d(TAG, "createBond() after removal returned $created")
                                if (!created) {
                                    logEvent(L10n.t("重新配对失败，请到系统 设置->蓝牙 手动点击相机完成配对", "Re-pairing failed. Pair the camera manually in System Settings -> Bluetooth."))
                                }
                                return
                            }
                            Log.w(TAG, "Bonding failed for ${device.address}; waiting for discovery to find camera again")
                            logEvent(L10n.t("配对已移除/失败", "Pairing removed/failed"))
                        }
                    }
                }

                BluetoothDevice.ACTION_PAIRING_REQUEST -> {
                    val device = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
                    } else {
                        @Suppress("DEPRECATION")
                        intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
                    }
                    val isRelevant = device != null && (device.address == currentDevice?.address || device.address == classicDevice?.address)
                    if (!isRelevant) return
                    val variant = intent.getIntExtra(BluetoothDevice.EXTRA_PAIRING_VARIANT, 0)
                    val key = intent.getIntExtra(BluetoothDevice.EXTRA_PAIRING_KEY, 0)
                    Log.d(TAG, "Pairing request from ${device?.address}: variant=$variant key=$key")
                    val passkey = String.format("%06d", key)
                    logEvent(L10n.t("相机配对请求（配对码 $passkey）- 请在系统弹窗确认", "Camera pairing request (code $passkey) - confirm in the system dialog"))
                }
            }
        }
    }

    override fun onCreate() {
        super.onCreate()
        settingsRepository = SettingsRepository(this)
        _defaultCameraName.value = settingsRepository.defaultConnectCameraName()
        refreshSavedCameras()
        createNotificationChannel()
        val filter = IntentFilter().apply {
            addAction(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
            addAction(BluetoothDevice.ACTION_PAIRING_REQUEST)
        }
        ContextCompat.registerReceiver(this, bondReceiver, filter, ContextCompat.RECEIVER_EXPORTED)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_DISCONNECT) {
            disconnect()
            stopSelf()
            return START_NOT_STICKY
        }
        startForeground(NOTIFICATION_ID, buildNotification())
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
        try {
            unregisterReceiver(bondReceiver)
        } catch (_: IllegalArgumentException) {
            // already unregistered
        }
        bleManager?.close()
        keepAliveJob?.cancel()
        stopLocationUpdates()
        serviceScope.cancel()
    }

    // -------------------------------------------------------------------------
    // Public commands
    // -------------------------------------------------------------------------

    fun startPairingScan() {
        serviceScope.launch {
            pairingMode = PairingMode.NEW
            savedCamera = null
            adoptedDeviceId = null
            _discoveredCameras.value = emptyList()
            updateState(ConnectionState.Scanning)
            try {
                Log.d(TAG, "Starting BLE scan with Nikon service filter")
                val filter = ScanFilter.Builder()
                    .setServiceUuid(ParcelUuid(CameraBleManager.SERVICE_UUID))
                    .build()
                val settings = ScanSettings.Builder()
                    .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                    .build()
                scanner.startScan(listOf(filter), settings, scanCallback)
                // Stop scan automatically after 15 seconds.
                serviceScope.launch {
                    delay(15_000)
                    if (state.value is ConnectionState.Scanning) {
                        stopScan()
                        logEvent("Scan timed out")
                    }
                }
            } catch (e: SecurityException) {
                updateState(ConnectionState.Error("Missing permission: ${e.message}"))
            } catch (e: Exception) {
                updateState(ConnectionState.Error("Scan start failed: ${e.message}"))
            }
        }
    }

    fun stopScan() {
        serviceScope.launch {
            try {
                scanner.stopScan(scanCallback)
            } catch (_: Exception) {
            }
            if (state.value is ConnectionState.Scanning) {
                updateState(ConnectionState.Idle)
            }
        }
    }

    fun selectDiscoveredCamera(camera: DiscoveredCamera) {
        serviceScope.launch {
            Log.d(TAG, "selectDiscoveredCamera: ${camera.name} [${camera.address}]")
            stopScan()
            pairingMode = PairingMode.NEW
            savedCamera = null
            reconnectRetryCount = 0
            reconnectScanRound = 0
            ensureSpoofName()
            val advertised = camera.manufacturerData?.let { extractAdvertisedDeviceId(it) }
            if (advertised != null && settingsRepository.fixedDeviceId() == null) {
                // The camera already has a pairing record (it advertises the device ID it
                // expects) - most likely SnapBridge's. Auto-crack the full SnapBridge
                // DeviceID and connect with it, so no record deletion is needed.
                logEvent(
                    L10n.t("检测到相机已有配对记录（期望设备ID=0x%08X），自动破解 SnapBridge 设备标识...", "Camera already has a pairing record (expects device ID=0x%08X). Auto-cracking the SnapBridge device ID...").format(advertised)
                )
                onAutoExtractCameraFound(
                    camera = PairedCamera(
                        name = camera.name,
                        address = camera.address,
                        addressType = BluetoothDevice.DEVICE_TYPE_LE,
                        device = advertised,
                        nonce = 0L,
                        controllerName = settingsRepository.spoofControllerName() ?: DEFAULT_CONTROLLER_NAME
                    ),
                    advertisedDevice = advertised
                )
                return@launch
            }
            adoptedDeviceId = advertised
            if (advertised != null) {
                logEvent(L10n.t("相机已有配对记录（设备ID=0x%08X），直接采用该ID连接", "Camera already paired (device ID=0x%08X); connecting with that ID directly").format(advertised))
            }
            currentDevice = bluetoothAdapter.getRemoteDevice(camera.address)
            controllerName = settingsRepository.spoofControllerName() ?: DEFAULT_CONTROLLER_NAME
            connectCurrentDevice()
        }
    }

    fun connectToSavedCamera(camera: PairedCamera) {
        serviceScope.launch {
            Log.d(TAG, "connectToSavedCamera: ${camera.name} [${camera.address}]")
            pairingMode = PairingMode.RECONNECT
            savedCamera = camera
            adoptedDeviceId = null
            reconnectRetryCount = 0
            reconnectScanRound = 0
            ensureSpoofName()
            controllerName = camera.controllerName.ifBlank {
                settingsRepository.spoofControllerName() ?: DEFAULT_CONTROLLER_NAME
            }
            // The camera changes its BLE (random) address between sessions. Scan for the
            // current advertisement by name before connecting; if the scan fails or times
            // out, fall back to the saved address.
            startReconnectScan(camera)
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

    /**
     * Automatically fill the controller name the camera will display, using the exact
     * SnapBridge algorithm: "Android_" + sanitized Build.MODEL + "_%04d" random suffix.
     * Only generates once (persists), so it stays stable across sessions like SnapBridge.
     */
    private fun ensureSpoofName() {
        if (!settingsRepository.spoofControllerName().isNullOrBlank()) return
        val name = generateSnapBridgeControllerName()
        settingsRepository.setSpoofControllerName(name)
        logEvent(L10n.t("已自动生成控制器名称：$name", "Auto-generated controller name: $name"))
    }

    private fun generateSnapBridgeControllerName(): String {
        val model = Build.MODEL.replace(Regex("[^\\x21-\\x7e]"), "_")
        val suffix = String.format(Locale.US, "_%04d", Random().nextInt(10000))
        val clientName = if (model.length > 31 - suffix.length) {
            model.substring(0, 31 - suffix.length) + suffix
        } else {
            model + suffix
        }
        return "Android_" + clientName
    }

    /**
     * Automatically recover the SnapBridge DeviceID that the camera has stored.
     *
     * SnapBridge seeds its 8-byte DeviceID with `new Random(System.currentTimeMillis())`
     * at first launch (see SnapBridgeIdSolver). The camera advertises the first 4 bytes,
     * which lets us invert the LCG and enumerate all possible seed timestamps. We then
     * connect to the camera with each candidate identity until the camera accepts one.
     */
    fun startAutoExtract(camera: PairedCamera) {
        serviceScope.launch {
            if (autoExtractActive) {
                logEvent(L10n.t("自动提取正在进行中，请稍候...", "Auto-extraction in progress, please wait..."))
                return@launch
            }
            val installTime = snapBridgeInstallTime()
            if (installTime == null) {
                logEvent(L10n.t("未检测到 SnapBridge（$SNAPBRIDGE_PACKAGE），无法自动提取设备标识", "SnapBridge ($SNAPBRIDGE_PACKAGE) not found; cannot auto-extract the device ID"))
                updateState(ConnectionState.Error(L10n.t("未安装 SnapBridge，无法自动提取", "SnapBridge not installed; cannot auto-extract")))
                return@launch
            }
            autoExtractCamera = camera
            autoExtractActive = true
            autoExtractQueue.clear()
            logEvent(L10n.t("SnapBridge 安装时间：${formatTimestamp(installTime)}，开始扫描相机广播...", "SnapBridge installed: ${formatTimestamp(installTime)}. Scanning for the camera..."))
            scanForAutoExtract(camera)
        }
    }

    /** SnapBridge install time (milliseconds), or null if SnapBridge is not installed. */
    private fun snapBridgeInstallTime(): Long? = try {
        packageManager.getPackageInfo(SNAPBRIDGE_PACKAGE, 0).firstInstallTime
    } catch (_: PackageManager.NameNotFoundException) {
        null
    }

    private fun scanForAutoExtract(camera: PairedCamera) {
        val scanner = this.scanner ?: run {
            logEvent(L10n.t("蓝牙扫描器不可用", "Bluetooth scanner unavailable"))
            autoExtractActive = false
            return
        }
        updateState(ConnectionState.Scanning)
        reconnectScanCallback?.let { try { scanner.stopScan(it) } catch (_: Exception) {} }
        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult?) {
                result ?: return
                val name = result.device.name
                val advertised = extractAdvertisedDeviceId(result.scanRecord)
                val nameMatch = name != null && name == camera.name
                if (advertised == null && !nameMatch) {
                    return
                }
                Log.d(TAG, "Auto-extract scan found: ${result.device.address} name=$name advertised=$advertised")
                try { scanner.stopScan(this) } catch (_: Exception) {}
                reconnectScanCallback = null
                onAutoExtractCameraFound(
                    camera = camera.copy(address = result.device.address),
                    advertisedDevice = advertised ?: camera.device
                )
            }

            override fun onScanFailed(errorCode: Int) {
                Log.w(TAG, "Auto-extract scan failed: $errorCode")
                logEvent(L10n.t("自动提取：扫描失败（$errorCode）", "Auto-extract: scan failed ($errorCode)"))
                autoExtractActive = false
                updateState(ConnectionState.Error(L10n.t("扫描失败: $errorCode", "Scan failed: $errorCode")))
            }
        }
        reconnectScanCallback = callback
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        try {
            scanner.startScan(emptyList(), settings, callback)
            serviceScope.launch {
                delay(30_000)
                if (autoExtractActive && reconnectScanCallback === callback) {
                    try { scanner.stopScan(callback) } catch (_: Exception) {}
                    reconnectScanCallback = null
                    autoExtractActive = false
                    logEvent(L10n.t("自动提取：30 秒内未找到相机广播", "Auto-extract: no camera advertisement within 30s"))
                    updateState(ConnectionState.Idle)
                }
            }
        } catch (e: SecurityException) {
            logEvent(L10n.t("自动提取：缺少蓝牙权限", "Auto-extract: missing Bluetooth permission"))
            autoExtractActive = false
            updateState(ConnectionState.Error(L10n.t("缺少蓝牙权限", "Missing Bluetooth permission")))
        } catch (e: Exception) {
            logEvent(L10n.t("自动提取：扫描启动失败 ${e.message}", "Auto-extract: failed to start scan ${e.message}"))
            autoExtractActive = false
        }
    }

    private fun onAutoExtractCameraFound(camera: PairedCamera, advertisedDevice: Long) {
        val installTime = snapBridgeInstallTime() ?: run {
            autoExtractActive = false
            return
        }
        val now = System.currentTimeMillis()
        logEvent(L10n.t("相机广播设备ID=0x%08X，反解候选种子...", "Camera advertises device ID=0x%08X. Solving candidate seeds...").format(advertisedDevice))
        val candidates = SnapBridgeIdSolver.candidatesFor(advertisedDevice, installTime, now)
        if (candidates.isEmpty()) {
            logEvent(L10n.t("安装时间范围内无候选，尝试全时段（2015 年至今）候选...", "No candidates in the install window; trying all-time candidates (2015-present)..."))
            val all = SnapBridgeIdSolver.candidatesFor(advertisedDevice, 1_420_070_400_000L, now)
            if (all.isEmpty()) {
                logEvent(L10n.t("无任何候选种子，自动提取失败", "No candidate seeds; auto-extraction failed"))
                autoExtractActive = false
                updateState(ConnectionState.Error(L10n.t("未能反解出候选设备标识", "Could not solve any candidate device ID")))
                return
            }
            logEvent(L10n.t("全时段共 ${all.size} 个候选（较慢），开始测试...", "${all.size} all-time candidate(s) (slow); starting tests..."))
            autoExtractQueue.addAll(all)
        } else {
            logEvent(L10n.t("找到 ${candidates.size} 个候选（按时间排序），开始测试...", "Found ${candidates.size} candidate(s); starting tests..."))
            autoExtractQueue.addAll(candidates)
        }
        autoExtractCamera = camera
        currentDevice = bluetoothAdapter.getRemoteDevice(camera.address)
        savedCamera = camera
        testNextAutoExtractCandidate()
    }

    private fun testNextAutoExtractCandidate() {
        val candidate = autoExtractQueue.removeFirstOrNull()
        val camera = autoExtractCamera
        if (candidate == null || camera == null) {
            autoExtractActive = false
            logEvent(L10n.t("所有候选均被相机拒绝，未能提取设备标识", "All candidates rejected by the camera; extraction failed"))
            updateState(ConnectionState.Idle)
            return
        }
        logEvent(L10n.t("测试候选 ${candidate.fixedIdentityHex}（种子时间 ${formatTimestamp(candidate.seed)}）", "Testing candidate ${candidate.fixedIdentityHex} (seed time ${formatTimestamp(candidate.seed)})"))
        settingsRepository.setFixedDeviceId(candidate.deviceIdHex)
        pairingMode = PairingMode.RECONNECT
        savedCamera = camera
        adoptedDeviceId = null
        controllerName = settingsRepository.spoofControllerName() ?: DEFAULT_CONTROLLER_NAME
        currentDevice = bluetoothAdapter.getRemoteDevice(camera.address)
        connectCurrentDevice()
    }

    private fun formatTimestamp(ms: Long): String {
        return try {
            java.time.Instant.ofEpochMilli(ms)
                .atZone(java.time.ZoneId.systemDefault())
                .format(java.time.format.DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss"))
        } catch (_: Exception) {
            ms.toString()
        }
    }

    @SuppressLint("MissingPermission")
    private fun startReconnectScan(camera: PairedCamera, round: Int = 0) {
        val scanner = this.scanner
        if (scanner == null) {
            Log.e(TAG, "BluetoothLeScanner is null, cannot start reconnect scan")
            currentDevice = bluetoothAdapter.getRemoteDevice(camera.address)
            connectCurrentDevice()
            return
        }
        reconnectScanRound = round
        updateState(ConnectionState.Connecting)
        logEvent(if (round == 0) L10n.t("正在扫描已保存的相机...", "Scanning for the saved camera...") else L10n.t("未发现相机广播，继续扫描...", "Camera not found yet, continuing to scan..."))
        reconnectScanTimeoutJob?.cancel()
        reconnectScanCallback?.let { try { scanner.stopScan(it) } catch (_: Exception) {} }

        lastAdvertisedDevice = null
        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult?) {
                result ?: return
                val name = result.device.name
                val advertised = extractAdvertisedDeviceId(result.scanRecord)
                val nameMatch = name != null && name == camera.name
                if (advertised == null && !nameMatch) {
                    Log.d(TAG, "Reconnect scan (other): ${result.device.address} name=$name rssi=${result.rssi}")
                    return
                }
                Log.d(
                    TAG,
                    "Reconnect scan candidate: ${result.device.address} name=$name rssi=${result.rssi} " +
                        "manufacturer=[${formatManufacturerData(result.scanRecord)}]"
                )
                if (advertised != null) {
                    // The camera advertises the device ID it expects when a pairing record
                    // exists. Adopting it makes the camera treat this app as that device,
                    // so switching between SnapBridge and this app needs no record deletion.
                    if (advertised != lastAdvertisedDevice) {
                        lastAdvertisedDevice = advertised
                        logEvent(
                            L10n.t("检测到相机广播：期望设备ID=0x%08X（%s，RSSI=%d）", "Camera found: expects device ID=0x%08X (%s, RSSI=%d)")
                                .format(advertised, result.device.address, result.rssi)
                        )
                    }
                    if (settingsRepository.fixedDeviceId() != null) {
                        Log.d(TAG, "Fixed device ID configured, ignoring advertised ID $advertised")
                    } else if (advertised != camera.device) {
                        Log.d(
                            TAG,
                            "Adopting advertised device ID 0x%08X (saved was 0x%08X)".format(advertised, camera.device)
                        )
                        logEvent(
                            L10n.t("设备ID不匹配（已保存0x%08X，相机期望0x%08X），自动采用相机ID，无需删除配对记录", "Device ID mismatch (saved 0x%08X, camera expects 0x%08X); adopting the camera ID, no need to delete pairings")
                                .format(camera.device, advertised)
                        )
                        val adopted = camera.copy(device = advertised)
                        settingsRepository.saveCamera(adopted)
                        refreshSavedCameras()
                        savedCamera = adopted
                    }
                }
                reconnectScanRound = 0
                reconnectRetryCount = 0
                Log.d(TAG, "Reconnect scan found current BLE address: ${result.device.address} (saved was ${camera.address})")
                reconnectScanTimeoutJob?.cancel()
                reconnectScanCallback?.let { try { scanner.stopScan(it) } catch (_: Exception) {} }
                reconnectScanCallback = null
                // Update the persisted address so next time we can try directly first.
                val updated = (savedCamera ?: camera).copy(address = result.device.address)
                settingsRepository.saveCamera(updated)
                refreshSavedCameras()
                savedCamera = updated
                currentDevice = result.device
                connectCurrentDevice()
            }

            override fun onBatchScanResults(results: MutableList<ScanResult>?) {
                results?.forEach { onScanResult(ScanSettings.CALLBACK_TYPE_ALL_MATCHES, it) }
            }

            override fun onScanFailed(errorCode: Int) {
                Log.w(TAG, "Reconnect scan failed: errorCode=$errorCode")
                reconnectScanCallback = null
                currentDevice = bluetoothAdapter.getRemoteDevice(camera.address)
                connectCurrentDevice()
            }
        }
        reconnectScanCallback = callback
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        try {
            // No service-UUID filter: the camera uses a fresh random BLE address between
            // sessions and may not always include the service UUID in its advertisement.
            // We match on the advertised name / Nikon manufacturer data in the callback.
            scanner.startScan(emptyList(), settings, callback)
            reconnectScanTimeoutJob = serviceScope.launch {
                delay(20_000)
                if (reconnectScanCallback != null) {
                    try { scanner.stopScan(reconnectScanCallback!!) } catch (_: Exception) {}
                    reconnectScanCallback = null
                    if (round == 0) {
                        Log.w(TAG, "Reconnect scan round 1 timed out; scanning again")
                        startReconnectScan(camera, round = 1)
                    } else {
                        Log.w(TAG, "Reconnect scan timed out; using saved address as fallback")
                        currentDevice = bluetoothAdapter.getRemoteDevice(camera.address)
                        connectCurrentDevice()
                    }
                }
            }
        } catch (e: SecurityException) {
            updateState(ConnectionState.Error("Missing permission: ${e.message}"))
        } catch (e: Exception) {
            Log.w(TAG, "Reconnect scan start failed: ${e.message}", e)
            currentDevice = bluetoothAdapter.getRemoteDevice(camera.address)
            connectCurrentDevice()
        }
    }

    fun disconnect() {
        serviceScope.launch {
            startupReconnectTimeoutJob?.cancel()
            startupReconnectTimeoutJob = null
            idWriteTimeoutJob?.cancel()
            geoTimeoutJob?.cancel()
            bondingTimeoutJob?.cancel()
            reconnectScanTimeoutJob?.cancel()
            reconnectScanCallback?.let { try { scanner.stopScan(it) } catch (_: Exception) {} }
            reconnectScanCallback = null
            stopClassicDiscovery()
            stopScan()
            bleManager?.disconnect()
            bleManager?.close()
            bleManager = null
            currentDevice = null
            currentStage1 = null
            pairingStep = 0
            reBondAfterRemoval = false
            updateState(ConnectionState.Idle)
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
                bleManager?.writeGeo(payload)
                updateState(ConnectionState.Busy)
                logEvent(L10n.t("暂无定位，发送兜底数据", "No fix yet; sending fallback data"))
            }
        }
    }

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
        bleManager?.writeGeo(payload)
        lastSentLocation = location
        lastSentTime = System.currentTimeMillis()
        _gpsState.update { it.copy(lastSentTime = lastSentTime) }
        updateState(ConnectionState.Busy)
        logEvent(L10n.t("已发送 GPS (%.5f, %.5f)", "GPS sent (%.5f, %.5f)").format(location.latitude, location.longitude))
        geoTimeoutJob?.cancel()
        geoTimeoutJob = serviceScope.launch {
            delay(10_000)
            if (state.value is ConnectionState.Busy) {
                updateState(ConnectionState.Ready)
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
            delay(10_000)
            val current = state.value
            if (current !is ConnectionState.Ready &&
                current !is ConnectionState.Busy &&
                current !is ConnectionState.Error
            ) {
                Log.w(TAG, "Startup reconnect timed out (state=$current), stopping")
                logEvent(L10n.t("10 秒内未能连接相机，已停止自动连接", "Could not connect within 10s; auto-connect stopped"))
                disconnect()
            }
        }
        connectToSavedCamera(camera)
    }

    private fun hasBluetoothPermission(): Boolean {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
    }

    // -------------------------------------------------------------------------
    // BLE callbacks
    // -------------------------------------------------------------------------

    override fun onEvent(event: BleEvent) {
        serviceScope.launch {
            when (event) {
                is BleEvent.Connected -> {
                    Log.d(TAG, "onEvent: Connected")
                    updateState(ConnectionState.Discovering)
                }
                is BleEvent.ServicesDiscovered -> {
                    Log.d(TAG, "onEvent: ServicesDiscovered")
                    bleManager?.prepare()
                }
                is BleEvent.SubscriptionsEnabled -> {
                    Log.d(TAG, "onEvent: SubscriptionsEnabled")
                    updateState(ConnectionState.Pairing)
                    beginHandshake()
                }

                is BleEvent.MtuChanged -> {
                    Log.d(TAG, "onEvent: MtuChanged mtu=${event.mtu}")
                    logEvent(L10n.t("MTU 已调整到 ${event.mtu}", "MTU set to ${event.mtu}"))
                }
                is BleEvent.PairIndication -> {
                    Log.d(TAG, "onEvent: PairIndication ${event.data.size} bytes")
                    handlePairIndication(event.data)
                }
                is BleEvent.Not1Notification -> {
                    Log.d(TAG, "onEvent: Not1Notification ${event.data.size} bytes")
                    handleNot1Notification(event.data)
                }
                is BleEvent.WriteDone -> {
                    Log.d(TAG, "onEvent: WriteDone uuid=${event.uuid} status=${event.status}")
                    handleWriteDone(event.uuid, event.status)
                }
                is BleEvent.Disconnected -> {
                    Log.d(TAG, "onEvent: Disconnected")
                    if (isAwaitingBond) {
                        Log.d(TAG, "Disconnected while waiting for classic bond - keeping state")
                    } else if (reconnectScanCallback != null) {
                        Log.d(TAG, "Disconnected while reconnect scan in flight - keeping state")
                    } else {
                        updateState(ConnectionState.Error(L10n.t("相机已断开", "Camera disconnected")))
                    }
                }
                is BleEvent.Error -> {
                    Log.e(TAG, "onEvent: Error ${event.message}")
                    if (isAwaitingBond) {
                        Log.d(TAG, "Ignoring BLE error while waiting for classic bond: ${event.message}")
                    } else if (
                        event.message.contains("Connection state change") &&
                        savedCamera != null &&
                        pairingMode == PairingMode.RECONNECT &&
                        reconnectRetryCount < 2
                    ) {
                        // The camera rejected the connection (usually status=133 because it
                        // expects a different device ID, e.g. SnapBridge's). Rescan: the
                        // camera advertises the ID it expects, and we adopt it automatically.
                        reconnectRetryCount++
                        logEvent(
                            L10n.t("连接被相机拒绝(status=133)，自动重新扫描以采用相机期望的设备ID（第 ${reconnectRetryCount}/2 次）", "Connection rejected by camera (status=133); rescanning to adopt the expected device ID (attempt ${reconnectRetryCount}/2)")
                        )
                        startReconnectScan(savedCamera!!, round = 0)
                    } else {
                        logEvent(L10n.t("蓝牙错误: ${event.message}", "Bluetooth error: ${event.message}"))
                        if (event.message.contains("Connection state change")) {
                            logEvent(L10n.t("无法连接相机：请确认相机已开机并进入配对模式后重试", "Cannot connect: make sure the camera is on and in pairing mode, then retry"))
                        }
                        updateState(ConnectionState.Error(event.message))
                    }
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Private helpers
    // -------------------------------------------------------------------------

    private fun connectCurrentDevice() {
        val device = currentDevice ?: run {
            Log.w(TAG, "connectCurrentDevice: no current device")
            return
        }
        isAwaitingBond = false
        classicBondComplete = false
        classicDevice = null
        reconnectScanTimeoutJob?.cancel()
        reconnectScanCallback?.let { try { scanner.stopScan(it) } catch (_: Exception) {} }
        reconnectScanCallback = null
        stage1RetryCount = 0
        Log.d(TAG, "connectCurrentDevice: ${device.address} (${device.name ?: "no name"}) bondState=${device.bondState}")
        updateState(ConnectionState.Connecting)
        bleManager?.close()
        bleManager = CameraBleManager(this, this).also { it.connect(device) }
    }

    private fun beginHandshake() {
        // SnapBridge authenticates first (stage 1-4); the controller name is written to
        // the ID characteristic only AFTER the handshake succeeds (see writeControllerId).
        // Writing it earlier makes the camera reject the write with status=128.
        sendStage1()
    }

    private fun sendStage1() {
        Log.d(TAG, "sendStage1: generating stage 1 (mode=$pairingMode)")
        // Priority: manually fixed ID > camera-advertised ID > saved ID > fresh random.
        // Pass the full identity (device + nonce) so a SnapBridge-compatible 16-hex-digit
        // fixed ID is honored end to end; the camera rejects writes otherwise.
        val fixedIdentity = settingsRepository.fixedIdentity()
        val override = fixedIdentity?.device ?: adoptedDeviceId
        currentStage1 = pairingEngine.createStage1(
            savedCamera,
            deviceOverride = override,
            nonceOverride = fixedIdentity?.nonce
        )
        pairingStep = 1
        currentStage1?.let {
            Log.d(TAG, "sendStage1: stage 1 timestamp=${it.timestamp.toHexString()} device=${it.device.toHexString()} nonce=${it.nonce.toHexString()} override=$override")
            bleManager?.writePairMessage(it.encode())
        }
        if (override != null) {
            logEvent(L10n.t("已发送配对第 1 阶段（使用指定设备ID=0x%08X）", "Pairing stage 1 sent (using fixed device ID=0x%08X)").format(override))
        } else {
            logEvent(L10n.t("已发送配对第 1 阶段", "Pairing stage 1 sent"))
        }
    }

    private fun handlePairIndication(data: ByteArray) {
        Log.d(TAG, "handlePairIndication: step=$pairingStep data=${data.toHex()}")
        when (pairingStep) {
            1 -> {
                if (autoExtractActive) {
                    autoExtractActive = false
                    autoExtractQueue.clear()
                    val fixed = settingsRepository.fixedDeviceIdRaw()
                    Log.d(TAG, "Auto-extract: camera accepted candidate $fixed")
                    logEvent(
                        L10n.t("✅ 自动提取成功！相机接受了设备标识 ${fixed?.uppercase() ?: "?"}，已保存并继续连接", "✅ Auto-extraction succeeded! The camera accepted the device ID ${fixed?.uppercase() ?: "?"}; saved and continuing")
                    )
                }
                val stage2 = PairingMessage.decode(data)
                Log.d(TAG, "Received stage 2: timestamp=${stage2.timestamp.toHexString()} device=${stage2.device.toHexString()} nonce=${stage2.nonce.toHexString()}")
                val stage3 = pairingEngine.verifyStage2AndBuildStage3(
                    currentStage1 ?: return,
                    stage2
                )
                if (stage3 == null) {
                    Log.e(TAG, "Salt verification failed - handshake aborted")
                    logEvent(L10n.t("盐校验失败 - 握手中止", "Salt verification failed - handshake aborted"))
                    updateState(ConnectionState.Error("Blowfish salt mismatch"))
                    return
                }
                Log.d(TAG, "Sending stage 3: device=${stage3.device.toHexString()} nonce=${stage3.nonce.toHexString()}")
                bleManager?.writePairMessage(stage3.encode())
                pairingStep = 2
                logEvent(L10n.t("已发送配对第 3 阶段", "Pairing stage 3 sent"))
            }
            2 -> {
                val stage4 = PairingMessage.decode(data)
                val serial = pairingEngine.extractSerial(stage4)
                Log.d(TAG, "Received stage 4: serial='$serial' timestamp=${stage4.timestamp.toHexString()} raw=${data.toHex()}")
                logEvent(L10n.t("相机序列号: $serial", "Camera serial: $serial"))
                // SnapBridge/smart-device handshake does not send stage 5; the camera sends the
                // final 01 00 success notification on NOT1 after stage 4. Wait briefly for it;
                // if it does not arrive, write the controller ID anyway.
                pairingStep = 3
                idWriteTimeoutJob?.cancel()
                idWriteTimeoutJob = serviceScope.launch {
                    delay(3_000)
                    if (pairingStep != 3) return@launch
                    Log.d(TAG, "No NOT1 success after stage 4, writing controller ID anyway")
                    writeControllerId()
                }
                Log.d(TAG, "Waiting for final OK on NOT1 before writing ID")
            }
            else -> {
                Log.w(TAG, "Unexpected PAIR indication in step $pairingStep")
                logEvent(L10n.t("意外的配对数据（步骤 $pairingStep）", "Unexpected pairing data (step $pairingStep)"))
            }
        }
    }

    private fun handleNot1Notification(data: ByteArray) {
        Log.d(TAG, "handleNot1Notification: step=$pairingStep data=${data.toHex()}")
        val isSuccess = data.size >= 2 && data[0] == 0x01.toByte() && data[1] == 0x00.toByte()
        if (isSuccess) {
            Log.d(TAG, "Received success notification 01 00 on NOT1")
            when (pairingStep) {
                3 -> {
                    idWriteTimeoutJob?.cancel()
                    writeControllerId()
                }
                4 -> {
                    idWriteTimeoutJob?.cancel()
                    Log.d(TAG, "Final OK received while waiting for ID write; proceeding")
                    pairingStep = 5
                    checkBondedAndFinish()
                }
                else -> {
                    Log.d(TAG, "Success notification ignored in step $pairingStep")
                }
            }
        } else {
            logEvent("NOT1: ${data.joinToString(" ") { "%02x".format(it) }}")
        }
    }

    private fun writeControllerId() {
        if (pairingStep >= 4) {
            Log.d(TAG, "writeControllerId: already triggered or done, skipping")
            return
        }
        pairingStep = 4
        idWriteTimeoutJob?.cancel()

        // SnapBridge and the Z50II reference write a fixed 32-byte ASCII name.
        val nameBytes = controllerName.toByteArray(Charsets.US_ASCII)
        val padded = ByteArray(32) { 0x00 }
        val copyLen = minOf(nameBytes.size, padded.size)
        nameBytes.copyInto(padded, 0, 0, copyLen)
        Log.d(TAG, "writeControllerId: $controllerName -> ${padded.toHex()}")
        bleManager?.writeId(padded)
        logEvent(L10n.t("写入控制器名称: $controllerName", "Writing controller name: $controllerName"))

        // Some cameras (notably Z50II) do not always acknowledge the ID write.
        // Proceed after a short timeout so we don't hang forever.
        idWriteTimeoutJob = serviceScope.launch {
            delay(3_000)
            Log.w(TAG, "ID write callback did not arrive in 3s, proceeding anyway")
            if (pairingStep == 4) {
                pairingStep = 5
                checkBondedAndFinish()
            }
        }
    }

    private fun handleWriteDone(uuid: java.util.UUID, status: Int) {
        Log.d(TAG, "handleWriteDone: uuid=$uuid status=$status step=$pairingStep")
        if (status != android.bluetooth.BluetoothGatt.GATT_SUCCESS) {
            Log.e(TAG, "Write failed on $uuid: status=$status")
            logEvent(L10n.t("写入失败: status=$status", "Write failed: status=$status"))
            if (uuid == CameraBleManager.ID_UUID && pairingStep == 0) {
                Log.w(TAG, "Device name write failed (status=$status); continuing with stage 1 anyway")
                logEvent(L10n.t("设备名写入失败(status=$status)，继续尝试配对握手", "Device name write failed (status=$status); continuing the handshake"))
                sendStage1()
                return
            }
            if (uuid == CameraBleManager.PAIR_UUID && pairingStep == 1) {
                if (autoExtractActive) {
                    Log.d(TAG, "Auto-extract: candidate rejected (status=$status), trying next")
                    logEvent(L10n.t("候选被相机拒绝(status=$status)，1.5 秒后测试下一个...", "Candidate rejected (status=$status); testing the next one in 1.5s..."))
                    serviceScope.launch {
                        delay(1_500)
                        testNextAutoExtractCandidate()
                    }
                    return
                }
                if (stage1RetryCount < 1 && reconnectRetryCount < 2) {
                    stage1RetryCount++
                    reconnectRetryCount++
                    logEvent(L10n.t("配对写入被相机拒绝(133)（第 ${reconnectRetryCount}/2 次），1 秒后自动重试...", "Pairing write rejected (133) (attempt ${reconnectRetryCount}/2); retrying in 1s..."))
                    serviceScope.launch {
                        delay(1_000)
                        if (pairingStep != 1) return@launch
                        if (bleManager?.isConnected == true) {
                            beginHandshake()
                        } else {
                            Log.d(TAG, "GATT disconnected during stage-1 retry; reconnecting first")
                            logEvent(L10n.t("连接已断开，先重新连接...", "Disconnected; reconnecting first..."))
                            connectCurrentDevice()
                        }
                    }
                    return
                }
                val bondState = currentDevice?.bondState
                val bondText = when (bondState) {
                    BluetoothDevice.BOND_BONDED -> L10n.t("已配对(12)", "Paired (12)")
                    BluetoothDevice.BOND_BONDING -> L10n.t("配对中(11)", "Pairing (11)")
                    else -> L10n.t("未配对(10)", "Not paired (10)")
                }
                val fixed = settingsRepository.fixedIdentity()
                logEvent(
                    L10n.t("相机拒绝了配对写入(status=133)。当前蓝牙配对状态: $bondText。", "Camera rejected the pairing write (status=133). Current bond state: $bondText.") +
                        (if (fixed?.nonce != null) {
                            L10n.t("已使用固定标识(device=0x%08X,nonce=0x%08X)仍被拒绝：请确认该标识与相机端配对记录一致", "Rejected even with the fixed identity (device=0x%08X,nonce=0x%08X): make sure it matches the camera's pairing record").format(fixed.device, fixed.nonce)
                        } else {
                            L10n.t("请检查：1) 相机已开机且未连接其他设备；2) 若刚用过 SnapBridge，请在固定设备标识中填入 SnapBridge 的完整16位标识（设备ID+nonce）", "Check: 1) the camera is on and not connected elsewhere; 2) if you just used SnapBridge, enter its full 16-hex ID (device+nonce) as the fixed device ID")
                        })
                )
                updateState(ConnectionState.Error("Pairing rejected by camera (status=$status)"))
            }
            return
        }
        when (uuid) {
            CameraBleManager.ID_UUID -> {
                idWriteTimeoutJob?.cancel()
                when (pairingStep) {
                    0 -> {
                        Log.d(TAG, "Reconnect device name written; starting stage 1")
                        logEvent(L10n.t("设备名写入成功，开始配对握手", "Device name written; starting the handshake"))
                        sendStage1()
                    }
                    4 -> {
                        Log.d(TAG, "ID write confirmed, checking bond state")
                        pairingStep = 5
                        checkBondedAndFinish()
                    }
                }
            }
            CameraBleManager.GEO_UUID -> {
                updateState(ConnectionState.Ready)
                logEvent(L10n.t("GPS 发送成功", "GPS sent"))
            }
            else -> Unit
        }
    }

    private fun checkBondedAndFinish() {
        val bleDevice = currentDevice ?: return
        val classicBonded = classicBondComplete || classicDevice?.bondState == BluetoothDevice.BOND_BONDED
        Log.d(TAG, "checkBondedAndFinish: mode=$pairingMode bleBondState=${bleDevice.bondState} classicBonded=$classicBonded")
        if (bleDevice.bondState == BluetoothDevice.BOND_BONDED || classicBonded) {
            onBonded()
            return
        }
        // The camera may already be paired with the OS under its classic Bluetooth
        // address (from SnapBridge or a previous session). The camera's classic name is
        // the controller name (e.g. "Android_MRR-W29_4909"), so match against it too.
        // Look it up in the bonded-device list before starting a fresh classic pairing.
        val bonded = bluetoothAdapter?.bondedDevices?.firstOrNull {
            namesMatch(it.name, bleDevice.name) ||
                namesMatch(it.name, savedCamera?.name) ||
                namesMatch(it.name, controllerName)
        }
        if (bonded != null) {
            Log.d(TAG, "Found bonded classic device ${bonded.address} (${bonded.name})")
            classicDevice = bonded
            classicBondComplete = true
            onBonded()
            return
        }
        Log.d(TAG, "No bonded classic device found, running classic pairing")
        // The camera's classic Bluetooth address is usually different from its BLE address.
        // We start classic discovery during the handshake and disconnect the BLE GATT after
        // the handshake so the classic pairing/system dialog can proceed. When the classic
        // device bonds, we reconnect on the BLE address and complete the flow.
        updateState(ConnectionState.Bonding)
        isAwaitingBond = true
        Log.d(TAG, "Not bonded yet; disconnecting BLE GATT to allow classic pairing")
        bleManager?.disconnect()
        bondingTimeoutJob?.cancel()
        bondingTimeoutJob = serviceScope.launch {
            delay(90_000)
            if (isAwaitingBond) {
                Log.w(TAG, "Bonding did not complete within 90s")
                logEvent(L10n.t("配对超时：未完成蓝牙配对。请确认相机处于配对模式后重试，或在系统 设置->蓝牙 中手动点击相机完成配对", "Pairing timed out. Make sure the camera is in pairing mode and retry, or pair manually in System Settings -> Bluetooth"))
                isAwaitingBond = false
                stopClassicDiscovery()
                updateState(ConnectionState.Error("Classic bonding timed out"))
            }
        }
        // Give the camera a moment to switch to classic mode after the BLE handshake
        // before scanning for it over classic Bluetooth.
        serviceScope.launch {
            delay(1_500)
            if (isAwaitingBond) startClassicDiscovery()
        }
    }

    private fun onBonded() {
        startupReconnectTimeoutJob?.cancel()
        startupReconnectTimeoutJob = null
        bondingTimeoutJob?.cancel()
        stopClassicDiscovery()
        isAwaitingBond = false
        Log.d(TAG, "onBonded: mode=$pairingMode")
        if (currentDevice != null && currentStage1 != null) {
            val camera = PairedCamera(
                name = currentDevice?.name ?: "Nikon",
                address = currentDevice?.address ?: return,
                addressType = currentDevice?.type ?: BluetoothDevice.DEVICE_TYPE_UNKNOWN,
                device = currentStage1?.device ?: return,
                nonce = currentStage1?.nonce ?: return,
                controllerName = controllerName
            )
            settingsRepository.saveCamera(camera)
            refreshSavedCameras()
            Log.d(TAG, "Saved paired camera: ${camera.name} [${camera.address}]")
        }
        pairingStep = 6
        updateState(ConnectionState.Ready)
        startLocationUpdates()
        startKeepAlive()
        logEvent(L10n.t("相机就绪", "Camera ready"))
    }

    @SuppressLint("MissingPermission")
    private fun reconnectAfterBonding() {
        val device = currentDevice ?: run {
            Log.w(TAG, "reconnectAfterBonding: no current device")
            return
        }
        val stage1 = currentStage1 ?: run {
            Log.w(TAG, "reconnectAfterBonding: no stage 1 data")
            return
        }
        Log.d(TAG, "reconnectAfterBonding: ${device.address} using saved stage 1 data")
        pairingMode = PairingMode.RECONNECT
        savedCamera = PairedCamera(
            name = device.name ?: "Nikon",
            address = device.address,
            addressType = device.type,
            device = stage1.device,
            nonce = stage1.nonce,
            controllerName = controllerName
        )
        settingsRepository.saveCamera(savedCamera!!)
        refreshSavedCameras()
        connectCurrentDevice()
        classicBondComplete = true
    }

    @SuppressLint("MissingPermission")
    private fun removeBondCompat(device: BluetoothDevice): Boolean {
        // BluetoothDevice.removeBond() is a hidden API; call it via reflection.
        return try {
            val method = BluetoothDevice::class.java.getMethod("removeBond")
            method.invoke(device) as? Boolean ?: false
        } catch (e: Exception) {
            Log.w(TAG, "removeBond via reflection failed: ${e.message}")
            false
        }
    }

    private fun namesMatch(foundName: String?, targetName: String?): Boolean {
        if (foundName.isNullOrBlank() || targetName.isNullOrBlank()) return false
        val a = foundName.trim().lowercase(Locale.ROOT)
        val b = targetName.trim().lowercase(Locale.ROOT)
        return a == b || a.contains(b) || b.contains(a)
    }
    @SuppressLint("MissingPermission")
    private fun startClassicDiscovery() {
        val adapter = bluetoothAdapter
        if (adapter == null) {
            Log.e(TAG, "BluetoothAdapter is null, cannot start classic discovery")
            return
        }
        if (adapter.isDiscovering) {
            Log.d(TAG, "Classic discovery already running")
            return
        }
        // Clean up any previous receiver.
        stopClassicDiscovery()
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                when (intent.action) {
                    BluetoothAdapter.ACTION_DISCOVERY_STARTED -> {
                        Log.d(TAG, "Classic discovery started")
                    }
                    BluetoothAdapter.ACTION_DISCOVERY_FINISHED -> {
                        Log.d(TAG, "Classic discovery finished")
                        if (isAwaitingBond || state.value == ConnectionState.Bonding) {
                            Log.d(TAG, "Discovery finished while still awaiting bond; restarting in 1s")
                            discoveryRestartJob?.cancel()
                            discoveryRestartJob = serviceScope.launch {
                                delay(1_000)
                                startClassicDiscovery()
                            }
                        }
                    }
                    BluetoothDevice.ACTION_FOUND -> {
                        val found = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                            intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
                        } else {
                            @Suppress("DEPRECATION")
                            intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
                        }
                        val foundName = intent.getStringExtra(BluetoothDevice.EXTRA_NAME) ?: found?.name
                        Log.d(TAG, "Classic discovery found: ${found?.address} name=$foundName")
                        if (found == null) return
                        val target = currentDevice ?: return
                        // The camera often advertises a different classic Bluetooth address
                        // than its BLE address, so match by name (with a fuzzy match, since
                        // some ROMs report a slightly different name over classic discovery).
                        if (found.address == target.address || namesMatch(foundName, target.name)) {
                            Log.d(TAG, "Target camera found via classic discovery, creating bond")
                            classicDevice = found
                            stopClassicDiscovery()
                            if (found.bondState == BluetoothDevice.BOND_BONDED) {
                                if (pairingMode == PairingMode.NEW) {
                                    // The camera never confirmed this stale bond (no 6-digit code
                                    // was ever shown on the camera), so it is not a valid pairing.
                                    // Remove it and re-pair from scratch so the system dialog and
                                    // the camera passkey prompt both appear.
                                    Log.w(TAG, "NEW mode found stale bond on ${found.address}; removing to re-pair")
                                    logEvent(L10n.t("检测到旧配对记录，正在移除并重新配对...", "Stale pairing detected; removing and re-pairing..."))
                                    reBondAfterRemoval = true
                                    classicDevice = found
                                    bondingTimeoutJob?.cancel()
                                    bondingTimeoutJob = serviceScope.launch {
                                        delay(15_000)
                                        if (isAwaitingBond && classicDevice == found && reBondAfterRemoval) {
                                            logEvent(L10n.t("移除旧配对超时，请到系统 设置->蓝牙 删除相机配对记录后重试", "Failed to remove the stale pairing in time; delete the camera's pairing in System Settings -> Bluetooth and retry"))
                                        }
                                    }
                                    @Suppress("MissingPermission")
                                    val removed = removeBondCompat(found)
                                    Log.d(TAG, "removeBond() returned $removed")
                                    if (!removed) {
                                        reBondAfterRemoval = false
                                        logEvent(L10n.t("移除旧配对失败，请到系统 设置->蓝牙 手动删除相机配对记录", "Could not remove the stale pairing; delete it manually in System Settings -> Bluetooth"))
                                    }
                                    return
                                }
                                Log.d(TAG, "Camera already bonded, skipping createBond")
                                logEvent(L10n.t("相机已完成配对，继续连接...", "Camera paired; continuing..."))
                                classicBondComplete = true
                                isAwaitingBond = false
                                // The BLE GATT link was dropped after the handshake; reconnect
                                // before reporting Ready so the GPS channel actually works.
                                reconnectAfterBonding()
                                return
                            }
                            logEvent(L10n.t("发现相机 (${foundName ?: found.address})，请求配对...", "Found camera (${foundName ?: found.address}); requesting pairing..."))
                            bondingTimeoutJob?.cancel()
                            bondingTimeoutJob = serviceScope.launch {
                                delay(20_000)
                                if (isAwaitingBond && classicDevice == found) {
                                    logEvent(L10n.t("配对请求未确认：若手机无配对弹窗，请到系统 设置->蓝牙 手动点击相机完成配对", "Pairing request not confirmed: if no dialog appeared, pair manually in System Settings -> Bluetooth"))
                                }
                            }
                            @Suppress("MissingPermission")
                            val created = found.createBond()
                            Log.d(TAG, "createBond() from discovery returned $created")
                            if (!created) {
                                bondingTimeoutJob?.cancel()
                                serviceScope.launch {
                                    delay(3_000)
                                    if (isAwaitingBond && classicDevice == found) {
                                        Log.w(TAG, "createBond returned false, retrying discovery")
                                        logEvent(L10n.t("配对请求失败，重试发现...", "Pairing request failed; retrying discovery..."))
                                        startClassicDiscovery()
                                    }
                                }
                            }
                        } else {
                            Log.d(TAG, "Ignoring non-camera device: $foundName (${found.address})")
                        }
                    }
                }
            }
        }
        discoveryReceiver = receiver
        val filter = IntentFilter().apply {
            addAction(BluetoothAdapter.ACTION_DISCOVERY_STARTED)
            addAction(BluetoothAdapter.ACTION_DISCOVERY_FINISHED)
            addAction(BluetoothDevice.ACTION_FOUND)
        }
        ContextCompat.registerReceiver(this, receiver, filter, ContextCompat.RECEIVER_EXPORTED)
        val started = adapter.startDiscovery()
        Log.d(TAG, "startDiscovery() returned $started")
        if (started) {
            logEvent(L10n.t("正在通过蓝牙搜索相机...", "Searching for the camera over Bluetooth..."))
        } else {
            Log.w(TAG, "startDiscovery() returned false, will retry in 2s")
            discoveryRestartJob?.cancel()
            discoveryRestartJob = serviceScope.launch {
                delay(2_000)
                startClassicDiscovery()
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun stopClassicDiscovery() {
        discoveryRestartJob?.cancel()
        discoveryReceiver?.let {
            try {
                unregisterReceiver(it)
            } catch (_: IllegalArgumentException) {
                // already unregistered
            }
            discoveryReceiver = null
        }
        bluetoothAdapter?.takeIf { it.isDiscovering }?.cancelDiscovery()
    }

    private fun handleScanResult(result: ScanResult) {
        val record = result.scanRecord
        val name = result.device.name ?: "<no name>"
        val address = result.device.address ?: "<no address>"
        val serviceUuids = record?.serviceUuids?.map { it.uuid.toString() } ?: emptyList()
        val hasNikonService = record?.serviceUuids?.any { it.uuid == CameraBleManager.SERVICE_UUID } == true

        Log.d(
            TAG,
            "[BLE SCAN] name=$name address=$address rssi=${result.rssi} " +
                "services=$serviceUuids " +
                "manufacturer=[${formatManufacturerData(record)}] " +
                "isConnectable=${result.isConnectable}"
        )

        if (hasNikonService) {
            Log.d(TAG, "[BLE SCAN] Nikon service UUID found on $address")
        }

        // Only add Nikon cameras to the UI list.
        if (!hasNikonService) {
            return
        }

        val saved = savedCameras.value
        val isNew = saved.none { it.address == result.device.address }
        val hasManData = (record?.manufacturerSpecificData?.size() ?: 0) > 0

        if (hasManData) {
            val advertised = extractAdvertisedDeviceId(record)
            if (advertised != null && advertised != lastAdvertisedDevice) {
                lastAdvertisedDevice = advertised
                Log.d(TAG, "[BLE SCAN] Camera $address advertises known device ID=0x%08X".format(advertised))
                if (pairingMode == PairingMode.NEW) {
                    logEvent(L10n.t("发现已配对相机（设备ID=0x%08X），选择后可直接切换连接", "Found a paired camera (device ID=0x%08X); select to switch directly").format(advertised))
                }
            }
        }

        val discovered = DiscoveredCamera(result, isNew)
        val current = _discoveredCameras.value.toMutableList()
        if (current.none { it.address == discovered.address }) {
            current.add(discovered)
            _discoveredCameras.value = current
            Log.d(TAG, "[BLE SCAN] Added ${discovered.name} (${discovered.address}) to UI list")
        }
    }

    private fun logScanResult(result: ScanResult, source: String) {
        val record = result.scanRecord
        val name = result.device.name ?: "<no name>"
        val address = result.device.address ?: "<no address>"
        val serviceUuids = record?.serviceUuids?.map { it.uuid.toString() } ?: emptyList()
        Log.v(
            TAG,
            "[$source] name=$name address=$address rssi=${result.rssi} " +
                "services=$serviceUuids manufacturer=[${formatManufacturerData(record)}]"
        )
    }

    private fun formatManufacturerData(record: android.bluetooth.le.ScanRecord?): String {
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
    private fun extractAdvertisedDeviceId(record: ScanRecord?): Long? {
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

    /** Parses the flattened manufacturer data held by [DiscoveredCamera]. */
    private fun extractAdvertisedDeviceId(data: ByteArray?): Long? {
        if (data == null || data.size < 6) return null
        val company = (data[0].toLong() and 0xFF) or ((data[1].toLong() and 0xFF) shl 8)
        if (company != 0x0399L) return null
        return ((data[2].toLong() and 0xFF) or
                ((data[3].toLong() and 0xFF) shl 8) or
                ((data[4].toLong() and 0xFF) shl 16) or
                ((data[5].toLong() and 0xFF) shl 24)) and 0xFFFFFFFFL
    }

    private fun refreshSavedCameras() {
        _savedCameras.value = settingsRepository.loadSavedCameras()
    }

    private fun logEvent(message: String) {
        _events.tryEmit(message)
    }

    private fun updateState(newState: ConnectionState) {
        _state.value = newState
        updateNotification()
    }

    // -------------------------------------------------------------------------
    // Notifications
    // -------------------------------------------------------------------------

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                L10n.t("相机连接", "Camera connection"),
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = L10n.t("保持与尼康相机的连接", "Keeps the connection to your Nikon camera")
            }
            val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            manager.createNotificationChannel(channel)
        }
    }

    private fun buildNotification(): Notification {
        val contentIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_CLEAR_TOP
            },
            PendingIntent.FLAG_IMMUTABLE
        )
        val disconnectIntent = PendingIntent.getService(
            this,
            0,
            Intent(this, CameraConnectionService::class.java).apply {
                action = ACTION_DISCONNECT
            },
            PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(L10n.t("尼康智能GPS", "Nikon Smart GPS"))
            .setContentText(state.value.label)
            .setSmallIcon(R.drawable.ic_launcher_foreground)
            .setContentIntent(contentIntent)
            .addAction(R.drawable.ic_launcher_foreground, L10n.t("断开", "Disconnect"), disconnectIntent)
            .setOngoing(true)
            .build()
    }

    private fun updateNotification() {
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        manager.notify(NOTIFICATION_ID, buildNotification())
    }

    // -------------------------------------------------------------------------
    // Binder
    // -------------------------------------------------------------------------

    inner class LocalBinder : Binder() {
        val service: CameraConnectionService
            get() = this@CameraConnectionService
    }

    private enum class PairingMode { NEW, RECONNECT }

    companion object {
        const val ACTION_DISCONNECT = "info.skyblond.nsp.DISCONNECT"
        private const val SNAPBRIDGE_PACKAGE = "com.nikon.snapbridge.cmru"
        private const val CHANNEL_ID = "camera_connection_channel"
        private const val NOTIFICATION_ID = 1
        private const val DEFAULT_CONTROLLER_NAME = "nsg-poc"
        private const val GPS_UPDATE_INTERVAL_MS = 5_000L
        private const val NETWORK_UPDATE_INTERVAL_MS = 15_000L
        private const val GPS_UPDATE_MIN_DISTANCE_M = 1f
        private const val MIN_SEND_DISTANCE_M = 3f
        private const val MAX_SEND_INTERVAL_MS = 20_000L
    }
}

private fun ByteArray.toHex(): String =
    joinToString(" ") { "%02x".format(it) }

private fun Long.toHexString(): String =
    "0x%016x".format(this)
