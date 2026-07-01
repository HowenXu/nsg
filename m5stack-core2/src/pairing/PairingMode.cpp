#include "PairingMode.h"

#include <Arduino.h>
#include <M5Unified.h>

#include <cstring>

#include "Config.h"
#include "Logging.h"
#include "Utils.h"

PairingMode::PairingMode() : state(State::SCANNING), scanner(nullptr), pClient(nullptr), classicBT(nullptr), cameraList(), selectedCameraIdx(0) {}

PairingMode::~PairingMode() {
    if (scanner != nullptr) {
        scanner->stopScanning();
    }
}

void PairingMode::setup() {
    scanner.reset(new NikonBLEScanner(NikonBLEScannerMode::NEW_DEVICE));
    if (!scanner->startScanning()) {
        NSG_LOG_FATAL("PairingMode::setup", "failed to start BLE scanning");
    }
}

void PairingMode::loop() {
    // Scanning: show scan results on screen and wait user to select
    // BLE_HANDSHAKE: after user select, do BLE handshake
    // PAIRING: after BLE handshake success, do classic BT pairing
    // SHOW_CODE: show code on screen and wait user to confirm
    // CODE_CONFIRM: user confirmed code, wait pairing result
    // SUCCESS: after pairing success, save camera info and reboot
    // FAIL: if any of the stage failed, jump to here and loop, user need to manually reset
    switch (state) {
        case State::SCANNING:
            handleScanResults();
            selectFirstCamera();
            break;
        case State::BLE_HANDSHAKE:
            doBLEHandshake();
            break;
        case State::PAIRING:
            startPairingFlow();
            break;
        case State::SHOW_CODE:
            showCodeAndWaitConfirm();
            break;
        case State::CODE_CONFIRM:
            waitPairingResult();
            break;
        case State::SUCCESS:
            saveAndReboot();
            break;
        case State::FAIL:
            // release resouces
            if (classicBT) classicBT.reset();
            if (pClient) pClient.reset();
            if (scanner) scanner.reset();
            // TODO: show message on screen?
            delay(50);
            break;
    }
}

void PairingMode::handleScanResults() {
    ScannedCamera camera;
    while (xQueueReceive(scanner->scanResultQueue, &camera, (TickType_t)0)) {
        auto deviceName = std::string(camera.name);
        auto deviceAddr = BLEAddress(camera.addr);
        bool dup = false;
        for (const auto& item : cameraList) {
            if (memcmp(item.addr, camera.addr, sizeof(item.addr)) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            cameraList.push_back(camera);
            NSG_LOG_INFO("PairingMode::handleScanResults", "Found %s, addr=%s", deviceName.c_str(), deviceAddr.toString().c_str());
        }
    }
    // prevent empty spin
    delay(20);
}

void PairingMode::selectFirstCamera() {
    // TODO: show list on screen and let the user select a camera.
    // For now, pair with the first discovered camera.
    if (!cameraList.empty()) {
        scanner->stopScanning();
        selectedCameraIdx = 0;
        state = State::BLE_HANDSHAKE;
        NSG_LOG_INFO("PairingMode::selectFirstCamera", "Pairing with %s", cameraList[selectedCameraIdx].name);
    }
}

void PairingMode::doBLEHandshake() {
    const ScannedCamera& camera = cameraList[selectedCameraIdx];
    const BLEAddress cameraAddr(const_cast<uint8_t*>(camera.addr), camera.addrType);

    // Perform BLE handshake.
    pClient.reset(new NikonBLEClient(rnd));
    if (!pClient->doHandshake(cameraAddr, camera.addrType)) {
        state = State::FAIL;
        NSG_LOG_ERROR("PairingMode::doBLEHandshake", "BLE Handshake failed");
    } else {
        state = State::PAIRING;
        NSG_LOG_INFO("PairingMode::doBLEHandshake", "BLE Handshake success");
    }
    NSG_LOG_INFO("PairingMode::doBLEHandshake", "Disconnecting BLE connection");
    pClient->disconnect();
}

void PairingMode::startPairingFlow() {
    const ScannedCamera& camera = cameraList[selectedCameraIdx];
    const std::string cameraName(camera.name);

    // Start Classic Bluetooth pairing.
    classicBT.reset(new ClassicBT(cameraName));
    // TODO: hardcoded to search for 1 minute.
    if (!classicBT->searchAndInitiatePair(60000)) {
        state = State::FAIL;
        NSG_LOG_ERROR("PairingMode::startPairingFlow", "Failed to search and initiate pairing");
    } else {
        state = State::SHOW_CODE;
        NSG_LOG_INFO("PairingMode::startPairingFlow", "Initiated pairing");
    }
}

void PairingMode::showCodeAndWaitConfirm() {
    const uint32_t code = classicBT->getPairCode();
    NSG_LOG_INFO("PairingMode::showCodeAndWaitConfirm", "Pair code: %06u", code);
    // TODO: show code on screen and let user confirm.
    // TODO: also vibration and speaker beep?
    // TODO: if user reject the code, jump to fail
    classicBT->confirmPairCode(true);
    timeAfterPairSuccess = 0;
    state = State::CODE_CONFIRM;
}

void PairingMode::waitPairingResult() {
    if (!classicBT->isPairingDone(120000)) {
        // pairing not done/timeout, keep waiting
        delay(50);
        return;
    }
    // pair done, or timeout
    if (classicBT->isPairingSuccess()) {  // pairing success
        if (timeAfterPairSuccess == 0) {
            // set time and start waiting
            timeAfterPairSuccess = millis();
            NSG_LOG_INFO("PairingMode::waitPairingResult", "Waiting 6s for camera to make connection...");
        }
        // wait 6s to give camera sometime to make the connection
        if (millis() - timeAfterPairSuccess < 6000) {
            delay(50);
            return;  // keep waiting
        } else {
            NSG_LOG_INFO("PairingMode::waitPairingResult", "Classic BT bond established");
            state = State::SUCCESS;
        }
    } else {  // pairing not success
        state = State::FAIL;
        NSG_LOG_ERROR("PairingMode::waitPairingResult", "Pairing failed");
    }
}

void PairingMode::saveAndReboot() {
    const ScannedCamera& camera = cameraList[selectedCameraIdx];
    NSG_LOG_INFO("PairingMode::saveAndReboot", "Saving paired camera info...");
    // Format classic BT address as "xx:xx:xx:xx:xx:xx" (matches BLEAddress::toString format)
    const uint8_t* addr = classicBT->getClassicAddr();
    char btAddrStr[18];
    snprintf(btAddrStr, sizeof(btAddrStr), "%02x:%02x:%02x:%02x:%02x:%02x", addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    SavedCameraInfo cameraInfo(String(camera.name), pClient->getDevice(), pClient->getNonce(), String(btAddrStr));
    Config::addToSavedCameras(cameraInfo);
    Config::reconcileSavedCamerasWithBondList();

    // Clean up before reboot.
    classicBT.reset();
    pClient.reset();
    scanner.reset();
    NSG_LOG_INFO("PairingMode::saveAndReboot", "rebooting...");
    ESP.restart();
}
