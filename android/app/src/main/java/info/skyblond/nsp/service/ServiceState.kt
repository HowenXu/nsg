package info.skyblond.nsp.service

import info.skyblond.nsp.ui.L10n

sealed class ConnectionState {
    data object Idle : ConnectionState()
    data object Scanning : ConnectionState()
    data object Connecting : ConnectionState()
    data object Discovering : ConnectionState()
    data object Pairing : ConnectionState()
    data object Bonding : ConnectionState()
    data object Ready : ConnectionState()
    data object Busy : ConnectionState()
    data class Error(val reason: String) : ConnectionState()

    val label: String
        get() = when (this) {
            is Idle -> L10n.t("空闲", "Idle")
            is Scanning -> L10n.t("扫描中...", "Scanning...")
            is Connecting -> L10n.t("连接中...", "Connecting...")
            is Discovering -> L10n.t("发现服务中...", "Discovering...")
            is Pairing -> L10n.t("配对中...", "Pairing...")
            is Bonding -> L10n.t("经典蓝牙配对中...", "Classic bonding...")
            is Ready -> L10n.t("就绪", "Ready")
            is Busy -> L10n.t("忙碌", "Busy")
            is Error -> L10n.t("错误: ", "Error: ") + reason
        }
}

data class GpsState(
    val enabled: Boolean = false,
    val hasFix: Boolean = false,
    val latitude: Double? = null,
    val longitude: Double? = null,
    val accuracyMeters: Float? = null,
    val lastFixTime: Long? = null,
    val lastSentTime: Long? = null
)
