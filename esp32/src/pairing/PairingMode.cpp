#include "PairingMode.h"

#include <Arduino.h>

#include <cstring>

#include "boards/Board.h"
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
    selectedCameraIdx = 0;
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
            Board::onPairingFailed();
            delay(10);
            break;
    }
    // prevent watchdog goes crazy
    yield();
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
        // prevent blocking
        yield();
    }

    Board::onPairingScanning(cameraList, selectedCameraIdx, [this]() {
        scanner->stopScanning();
        state = State::BLE_HANDSHAKE;
        NSG_LOG_INFO("PairingMode::handleScanningInput", "selecting %s", cameraList[selectedCameraIdx].name);
    });
}

void PairingMode::doBLEHandshake() {
    const ScannedCamera& camera = cameraList[selectedCameraIdx];
    const BLEAddress cameraAddr(const_cast<uint8_t*>(camera.addr), camera.addrType);

    Board::onPairingStartBleHandshake(camera);

    // Perform BLE handshake.
    pClient.reset(new NikonBLEClient(rnd));
    if (!pClient->doHandshake(cameraAddr, camera.addrType)) {
        state = State::FAIL;
        NSG_LOG_ERROR("PairingMode::doBLEHandshake", "BLE Handshake failed");
        Board::onPairingBleHandshakeFailed(camera);
    } else {
        state = State::PAIRING;
        NSG_LOG_INFO("PairingMode::doBLEHandshake", "BLE Handshake success");
        Board::onPairingBleHandshakeSuccess(camera);
    }
    NSG_LOG_INFO("PairingMode::doBLEHandshake", "Disconnecting BLE connection");
    pClient->disconnect();
}

void PairingMode::startPairingFlow() {
    const ScannedCamera& camera = cameraList[selectedCameraIdx];
    const std::string cameraName(camera.name);

    Board::onPairingStartBtPairingInit(camera);

    // Start Classic Bluetooth pairing.
    classicBT.reset(new ClassicBT(cameraName));
    // TODO: blocking? will it hurt anything?
    if (!classicBT->searchAndInitiatePair(NIKON_BT_SEARCH_TIME_MS)) {
        state = State::FAIL;
        NSG_LOG_ERROR("PairingMode::startPairingFlow", "Failed to search and initiate pairing");
        Board::onPairingBtPairingInitFailed(camera);
    } else {
        state = State::SHOW_CODE;
        NSG_LOG_INFO("PairingMode::startPairingFlow", "Initiated pairing");
        Board::onPairingBtPairingInitSuccess(camera);
    }
}

void PairingMode::showCodeAndWaitConfirm() {
    Board::onPairingCodeConfirm(classicBT->isPairCodeReady(), classicBT->getPairCode(), [this](bool result) {
        classicBT->confirmPairCode(result);
        if (result) {
            timeAfterPairSuccess = 0;
            state = State::CODE_CONFIRM;
        } else {
            state = State::FAIL;
        }
    });
}

void PairingMode::waitPairingResult() {
    if (classicBT->isPairingDone(NIKON_BT_PAIR_TIMEOUT_MS)) {  // pair done, or timeout
        const bool isPairingSuccess = classicBT->isPairingSuccess();
        if (isPairingSuccess) {  // pairing success
            if (timeAfterPairSuccess == 0) {
                // set time and start waiting
                timeAfterPairSuccess = millis();
                NSG_LOG_INFO("PairingMode::waitPairingResult", "Waiting %d ms for camera to make connection...", NIKON_BT_AFTER_PAIR_TIME_MS);
            }
            // wait extra time for camera to make the connection
            if (millis() - timeAfterPairSuccess < NIKON_BT_AFTER_PAIR_TIME_MS) {
                Board::onPairingWaitBtResult(true, isPairingSuccess, false);
                return;  // keep waiting
            } else {
                NSG_LOG_INFO("PairingMode::waitPairingResult", "Classic BT bond established");
                state = State::SUCCESS;
                Board::onPairingWaitBtResult(true, isPairingSuccess, true);
            }
        } else {  // pairing not success
            state = State::FAIL;
            NSG_LOG_ERROR("PairingMode::waitPairingResult", "Pairing failed");
            Board::onPairingWaitBtResult(true, isPairingSuccess, true);
        }
    } else {  // pairing in progress
        Board::onPairingWaitBtResult(false, false, false);
    }
}

void PairingMode::saveAndReboot() {
    const ScannedCamera& camera = cameraList[selectedCameraIdx];
    NSG_LOG_INFO("PairingMode::saveAndReboot", "Saving paired camera info...");
    SavedCameraInfo cameraInfo(String(camera.name), pClient->getDevice(), pClient->getNonce());
    Board::onPairingFinished(cameraInfo);
    Config::addToSavedCameras(cameraInfo);

    // Clean up before reboot.
    classicBT.reset();
    pClient.reset();
    scanner.reset();
    NSG_LOG_INFO("PairingMode::saveAndReboot", "rebooting...");
    ESP.restart();
}
