#include "../Board.h"

#include "Logging.h"

void Board::onMainSetupBeforeSerial() {
    // set CPU to 160MHz to save power
    setCpuFrequencyMhz(160);
}

void Board::onMainSetupAfterSerial() {
    // TODO: init ESP32 RTC
    // TODO: setup pin for boot mode detection
}

BootModeEnum Board::detectBootMode() {
    // TODO wait for a while and read detect pin
    //   if short to GND (read 0) -> pairing mode, otherwise normal mode
    return BootModeEnum::NORMAL;
}

void Board::onMainSetupEnd() {}

void Board::onMainLoopBeforeApp() {}

void Board::onMainLoopAfterApp() {}

void Board::onPairingScanning(std::vector<ScannedCamera> const& cameraList, size_t& selectedCameraIdx, std::function<void()> const& onSelect) {
    // TODO: automatically select first camera found
}

void Board::onPairingStartBleHandshake(ScannedCamera const& selectedCamera) {
    // TODO print serial log
}

void Board::onPairingBleHandshakeSuccess(ScannedCamera const& selectedCamera) {
    // TODO print serial log
}

void Board::onPairingBleHandshakeFailed(ScannedCamera const& selectedCamera) {
    // TODO print serial log
}

void Board::onPairingStartBtPairingInit(ScannedCamera const& selectedCamera) {
    // TODO print serial log
}

void Board::onPairingBtPairingInitSuccess(ScannedCamera const& selectedCamera) {
    // TODO print serial log
}

void Board::onPairingBtPairingInitFailed(ScannedCamera const& selectedCamera) {
    // TODO print serial log
}

void Board::onPairingCodeConfirm(bool codeReady, uint32_t code, std::function<void(bool)> const& onConfirm) {
    // TODO auto confirm when code is available, if no code yet, delay 100ms
}

void Board::onPairingWaitBtResult(bool const isPairDone, bool const isPairSuccess, bool const isChangingState) {
    // TODO print serial log, if not done, call delay 50ms if not done, call delay 50ms if isPairSuccess && !isChangingState
}

void Board::onPairingFinished(SavedCameraInfo const& camera) {
    // TODO print serial log
}

void Board::onPairingFailed() {
    // TODO print serial log using fatal mode and suggest manual reset
}

void Board::onNormalUpdateStatus(GnssSnapshot const& gnssStatus, BleStatusSnapshot const& bleStatus) {
    // TODO print serial log
}

void Board::setRTC(RtcSnapshot const& rtc) {
    // TODO ESP32 internal RTC
}

RtcSnapshot Board::getRTC() {
    // TODO ESP32 internal RTC
    return {0, 0, 0, 0, 0, 0};
}
