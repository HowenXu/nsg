#include "../Board.h"

#include <sys/time.h>

#include "BoardConst.h"
#include "Logging.h"

void Board::onMainSetupBeforeSerial() {
    // nop
}

void Board::onMainSetupAfterSerial() {
    // init ESP32 internal RTC, use GMT timezone
    // (the system clock is backed by the internal RTC; no battery, so it
    //  reads 1970-01-01 on cold boot until GNSS syncs it via setRTC)
    setenv("TZ", "GMT", 1);
    tzset();
    NSG_LOG_DEBUG("ESP32-WROOM-32E", "Internal RTC initialized (TZ=GMT)");
    // setup pin for boot mode detection
    pinMode(BOOTMODE_DETECT_PIN, INPUT_PULLUP);
}

BootModeEnum Board::detectBootMode() {
    NSG_LOG_INFO("ESP32-WROOM-32E", "Detecting boot mode... Short pin %d to GND to enter pairing mode", BOOTMODE_DETECT_PIN);
    // wait for a while and read detect pin
    delay(BOOTMODE_DETECT_DELAY_MS);

    // if short to GND (read 0) -> pairing mode
    if (!digitalRead(BOOTMODE_DETECT_PIN)) {
        NSG_LOG_INFO("ESP32-WROOM-32E", "Entering Pairing mode");
        return BootModeEnum::PAIRING;
    } else {
        NSG_LOG_INFO("ESP32-WROOM-32E", "Entering Normal mode");
        return BootModeEnum::NORMAL;
    }
}

void Board::onMainSetupEnd() {}

void Board::onMainLoopBeforeApp() {}

void Board::onMainLoopAfterApp() {}

void Board::onPairingScanning(std::vector<ScannedCamera> const& cameraList, size_t& selectedCameraIdx, std::function<void()> const& onSelect) {
    // automatically select first camera found
    if (!cameraList.empty()) {
        NSG_LOG_INFO("ESP32-WROOM-32E", "Automatically select first camera");
        selectedCameraIdx = 0;
        onSelect();
    }
}

void Board::onPairingStartBleHandshake(ScannedCamera const& selectedCamera) {
    NSG_LOG_INFO("ESP32-WROOM-32E", "Start BLE handshake with %s", selectedCamera.name);
}

void Board::onPairingBleHandshakeSuccess(ScannedCamera const& selectedCamera) {
    NSG_LOG_INFO("ESP32-WROOM-32E", "BLE handshake with %s succeeded!", selectedCamera.name);
}

void Board::onPairingBleHandshakeFailed(ScannedCamera const& selectedCamera) {
    NSG_LOG_WARN("ESP32-WROOM-32E", "BLE handshake with %s failed!", selectedCamera.name);
}

void Board::onPairingStartBtPairingInit(ScannedCamera const& selectedCamera) {
    NSG_LOG_INFO("ESP32-WROOM-32E", "Start classic BT pairing with %s", selectedCamera.name);
}

void Board::onPairingBtPairingInitSuccess(ScannedCamera const& selectedCamera) {
    NSG_LOG_INFO("ESP32-WROOM-32E", "Successfully initiated classic BT pairing with %s", selectedCamera.name);
}

void Board::onPairingBtPairingInitFailed(ScannedCamera const& selectedCamera) {
    NSG_LOG_WARN("ESP32-WROOM-32E", "Failed to initiate classic BT pairing with %s", selectedCamera.name);
}

void Board::onPairingCodeConfirm(bool codeReady, uint32_t code, std::function<void(bool)> const& onConfirm) {
    if (!codeReady) {
        delay(100);
        return;
    }
    NSG_LOG_INFO("ESP32-WROOM-32E", "Classic BT pairing code: %06u, auto confirm...", code);
    onConfirm(true);
}

void Board::onPairingWaitBtResult(bool const isPairDone, bool const isPairSuccess, bool const isChangingState) {
    if (!isPairDone) {
        // pairing not done/timeout, keep waiting
        delay(50);
        return;
    }
    if (isChangingState) {
        NSG_LOG_INFO("ESP32-WROOM-32E", "Finished classic BT pairing");
    }
    // pair done, or timeout
    if (isPairSuccess && !isChangingState) {  // pairing success but waiting
        delay(50);
    }
}

void Board::onPairingFinished(SavedCameraInfo const& camera) { NSG_LOG_INFO("ESP32-WROOM-32E", "Successfully paired with %s", camera.bleName.c_str()); }

void Board::onPairingFailed() { NSG_LOG_ERROR("ESP32-WROOM-32E", "Failed to pair, please manually reset..."); }

static uint32_t normalUpdateStatusLastPrint = 0;

void Board::onNormalUpdateStatus(GnssSnapshot const& gnssStatus, BleStatusSnapshot const& bleStatus) {
    if (millis() - normalUpdateStatusLastPrint <= 10000) return;
    normalUpdateStatusLastPrint = millis();
    NSG_LOG_INFO("GPS Status", "FIX: %s", gnssStatus.gnssValid ? "VALID" : "INVALID");
    // latitude
    auto latAbs = std::fabs(gnssStatus.lat);
    uint8_t latD = static_cast<uint8_t>(latAbs);
    double latM = (latAbs - latD) * 60.0;
    NSG_LOG_INFO("GPS Status", "LAT: %s %3d deg %06.3f'", gnssStatus.lat >= 0 ? "N" : "S", latD, latM);
    // longitude
    auto lonAbs = std::fabs(gnssStatus.lon);
    uint8_t lonD = static_cast<uint8_t>(lonAbs);
    double lonM = (lonAbs - lonD) * 60.0;
    NSG_LOG_INFO("GPS Status", "LON: %s %3d deg %06.3f'", gnssStatus.lon >= 0 ? "E" : "W", lonD, lonM);
    // altitude, some extra space to cover digits change
    NSG_LOG_INFO("GPS Status", "ALT: %s %d M", gnssStatus.altitudeMeters >= 0 ? "+" : "-",  //
                 gnssStatus.altitudeMeters >= 0 ? gnssStatus.altitudeMeters : -gnssStatus.altitudeMeters);
    // satellites count
    NSG_LOG_INFO("GPS Status", "SAT: %d", gnssStatus.satellites);
    // BLE connection
    NSG_LOG_INFO("GPS Status", "BLE: %u / %d", bleStatus.activeConnections, CONFIG_BTDM_CTRL_BLE_MAX_CONN);
    // Paired devices
    NSG_LOG_INFO("GPS Status", "CAM: %u paired", bleStatus.pairedCount);
}

void Board::setRTC(RtcSnapshot const& rtc) {
    // rtc holds UTC; TZ=GMT so mktime treats tm as UTC
    struct tm t{};
    t.tm_year = rtc.year - 1900;
    t.tm_mon = rtc.month - 1;
    t.tm_mday = rtc.day;
    t.tm_hour = rtc.hour;
    t.tm_min = rtc.minute;
    t.tm_sec = rtc.second;
    t.tm_isdst = -1;
    struct timeval tv{};
    tv.tv_sec = mktime(&t);
    settimeofday(&tv, nullptr);
}

RtcSnapshot Board::getRTC() {
    time_t now = time(nullptr);
    struct tm t{};
    gmtime_r(&now, &t);
    return {(uint16_t)(t.tm_year + 1900), (uint8_t)(t.tm_mon + 1), (uint8_t)t.tm_mday, (uint8_t)t.tm_hour, (uint8_t)t.tm_min, (uint8_t)t.tm_sec};
}
