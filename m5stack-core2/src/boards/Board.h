#ifndef BOARD_H
#define BOARD_H

#include <cstdint>
#include <functional>
#include <vector>

#include "Config.h"
#include "common/ScannedCamera.h"

// boot mode enum
enum class BootModeEnum { PAIRING, NORMAL };

// GNSS
struct GnssSnapshot {
    double lat = 0;
    double lon = 0;
    int32_t altitudeMeters = 0;
    uint8_t satellites = 0;
    uint8_t gnssValid = 0;
};

// RTC
struct RtcSnapshot {
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
};

// BLE status
struct BleStatusSnapshot {
    uint32_t activeConnections = 0;
    uint32_t pairedCount = 0;
};

namespace Board {

// Called before serial initialization,
// for example, set CPU freq, etc.
void onMainSetupBeforeSerial();
// General setup, can use logging
void onMainSetupAfterSerial();
// detect and return the boot mode.
BootModeEnum detectBootMode();
// Called at the end of the main setup.
void onMainSetupEnd();

// Called at the start of the main loop
void onMainLoopBeforeApp();
// Called at the end of the main loop
void onMainLoopAfterApp();

// handle Pairing mode scanning result, call onSelect to select the camera, set index to selectedCameraIdx
void onPairingScanning(std::vector<ScannedCamera> const& cameraList, size_t& selectedCameraIdx, std::function<void()> const& onSelect);
// called at the start of Pairing mode BLE handshake stage
void onPairingStartBleHandshake(ScannedCamera const& selectedCamera);
void onPairingBleHandshakeSuccess(ScannedCamera const& selectedCamera);
void onPairingBleHandshakeFailed(ScannedCamera const& selectedCamera);
// called at the start of Pairing mode classic BT pairing
void onPairingStartBtPairingInit(ScannedCamera const& selectedCamera);
void onPairingBtPairingInitSuccess(ScannedCamera const& selectedCamera);
void onPairingBtPairingInitFailed(ScannedCamera const& selectedCamera);
// show classic bt pairing code and handle user input
void onPairingCodeConfirm(bool const codeReady, uint32_t const code, std::function<void(bool)> const& onConfirm);
// wait for the classic bt pairing result
void onPairingWaitBtResult(bool const isPairDone, bool const isPairSuccess, bool const isChangingState);
// finished pairing
void onPairingFinished(SavedCameraInfo const& camera);
// failed pairing
void onPairingFailed();

// on normal mode loop to update status with gnss and ble info
void onNormalUpdateStatus(GnssSnapshot const& gnssStatus, BleStatusSnapshot const& bleStatus);
// update RTC
void setRTC(RtcSnapshot const& rtc);
// get RTC
RtcSnapshot getRTC();

}  // namespace Board

#endif