#include "PairingMode.h"

#include <Arduino.h>
#include <M5Unified.h>

#include <cstring>

#include "Config.h"
#include "Logging.h"
#include "Screen.h"
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
            handleScanningInput();
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
            if (screen.shouldDraw()) {
                screen.drawStringMiddleCenter("FAILED", 3, 0xd30000, 0x000000, screen.height() / 2 - 30);
                screen.drawStringMiddleCenter("Please reset manually", 2, 0xd3d3d3, 0x000000, screen.height() / 2 + 60);
            }
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

    if (screen.shouldDraw()) {
        screen.drawStringMiddleCenter("Scanning", 3, 0xFFFFFF, 0x000000, screen.topBarHeight() + 25);

        bool hasPrev = selectedCameraIdx > 0;
        bool hasNext = !cameraList.empty() && selectedCameraIdx < cameraList.size() - 1;
        size_t selectedPage = selectedCameraIdx / SCANNING_PAGE_SIZE;
        // inclusive
        size_t pageStartIndex = selectedPage * SCANNING_PAGE_SIZE;
        // exclusive
        size_t pageEndIndex = pageStartIndex + SCANNING_PAGE_SIZE;
        if (pageEndIndex > cameraList.size()) {
            pageEndIndex = cameraList.size();
        }
        for (size_t i = pageStartIndex; i < pageEndIndex; i++) {
            auto& item = cameraList[i];
            screen.drawStringMiddleCenter(item.name, 2,                                  //
                                          i == selectedCameraIdx ? 0x00FFFF : 0xd3d3d3,  // cyan if select, light grey otherwise
                                          0x000000,                                      //
                                          screen.topBarHeight() + 60 + (i - pageStartIndex) * 25);
        }

        screen.drawStringAboveBtnA("Prev", 2,                      //
                                   hasPrev ? 0xd3d3d3 : 0x000000,  // light grey if valid, otherwise black (hide)
                                   0x000000);
        screen.drawStringAboveBtnC("Next", 2,                      //
                                   hasNext ? 0xd3d3d3 : 0x000000,  // light grey if valid, otherwise black (hide)
                                   0x000000);
        screen.drawStringAboveBtnB("Confirm", 2,                               //
                                   !cameraList.empty() ? 0xd3d3d3 : 0x000000,  // light grey if valid, otherwise black (hide)
                                   0x000000);
    }
}

void PairingMode::handleScanningInput() {
    // btnA -> prev page, btnB -> Confirm, btnC -> next page
    if (screen.shouldHandleInput()) {
        if (screen.btnB()->wasPressed() && selectedCameraIdx < cameraList.size()) {
            M5.Speaker.tone(1000, 100);
            scanner->stopScanning();
            state = State::BLE_HANDSHAKE;
            NSG_LOG_INFO("PairingMode::handleScanningInput", "selecting %s", cameraList[selectedCameraIdx].name);
            screen.clearScreen();
        } else if (screen.btnA()->wasPressed() && selectedCameraIdx > 0) {
            M5.Speaker.tone(1000, 100);
            selectedCameraIdx--;
            if (selectedCameraIdx % SCANNING_PAGE_SIZE == SCANNING_PAGE_SIZE - 1) {
                screen.clearScreen();
            }
        } else if (screen.btnC()->wasPressed() && !cameraList.empty() && selectedCameraIdx < cameraList.size() - 1) {
            M5.Speaker.tone(1000, 100);
            selectedCameraIdx++;
            if (selectedCameraIdx % SCANNING_PAGE_SIZE == 0) {
                screen.clearScreen();
            }
        }
    }
}

void PairingMode::doBLEHandshake() {
    const ScannedCamera& camera = cameraList[selectedCameraIdx];
    const BLEAddress cameraAddr(const_cast<uint8_t*>(camera.addr), camera.addrType);

    // draw it anyway because handshake is blocking
    screen.drawStringMiddleCenter("Handshaking...", 3, 0xFFFFFF, 0x000000, screen.height() / 2);

    // Perform BLE handshake.
    pClient.reset(new NikonBLEClient(rnd));
    if (!pClient->doHandshake(cameraAddr, camera.addrType)) {
        state = State::FAIL;
        NSG_LOG_ERROR("PairingMode::doBLEHandshake", "BLE Handshake failed");
        screen.clearScreen();
    } else {
        state = State::PAIRING;
        NSG_LOG_INFO("PairingMode::doBLEHandshake", "BLE Handshake success");
        screen.clearScreen();
    }
    NSG_LOG_INFO("PairingMode::doBLEHandshake", "Disconnecting BLE connection");
    pClient->disconnect();
}

void PairingMode::startPairingFlow() {
    const ScannedCamera& camera = cameraList[selectedCameraIdx];
    const std::string cameraName(camera.name);

    // draw it anyway because search and bonding is blocking
    screen.drawStringMiddleCenter("Pairing...", 3, 0xFFFFFF, 0x000000, screen.height() / 2);

    // Start Classic Bluetooth pairing.
    classicBT.reset(new ClassicBT(cameraName));
    // TODO: blocking? will it hurt anything?
    if (!classicBT->searchAndInitiatePair(NIKON_BT_SEARCH_TIME_MS)) {
        state = State::FAIL;
        NSG_LOG_ERROR("PairingMode::startPairingFlow", "Failed to search and initiate pairing");
        screen.clearScreen();
    } else {
        state = State::SHOW_CODE;
        NSG_LOG_INFO("PairingMode::startPairingFlow", "Initiated pairing");
        screen.clearScreen();
    }
}

void PairingMode::showCodeAndWaitConfirm() {
    if (screen.shouldDraw()) {
        screen.drawStringMiddleCenter("Confirm code", 3, 0xFFFFFF, 0x000000, screen.height() / 2 - 50);
    }

    if (!classicBT->isPairCodeReady()) {
        delay(100);
        return;
    }
    const uint32_t code = classicBT->getPairCode();

    if (screen.shouldDraw()) {
        char buffer[10];
        snprintf(buffer, sizeof(buffer), "%06u", code);
        screen.drawStringMiddleCenter(buffer, 4, 0xFFFFFF, 0x000000, screen.height() / 2);
        screen.drawStringAboveBtnA("Confirm", 2, 0x00d300, 0x000000);
        screen.drawStringAboveBtnC("Reject", 2, 0xd30000, 0x000000);
    }

    if (screen.shouldHandleInput()) {
        if (screen.btnA()->wasPressed()) {
            M5.Speaker.tone(1000, 100);
            classicBT->confirmPairCode(true);
            timeAfterPairSuccess = 0;
            state = State::CODE_CONFIRM;
            screen.clearScreen();
        } else if (screen.btnC()->wasPressed()) {
            M5.Speaker.tone(1000, 100);
            classicBT->confirmPairCode(false);
            state = State::FAIL;
            screen.clearScreen();
        }
    }
}

void PairingMode::waitPairingResult() {
    if (screen.shouldDraw()) {
        screen.drawStringMiddleCenter("Connecting...", 3, 0xd3d3d3, 0x000000, screen.height() / 2);
    }
    if (!classicBT->isPairingDone(NIKON_BT_PAIR_TIMEOUT_MS)) {
        // pairing not done/timeout, keep waiting
        delay(50);
        return;
    }
    // pair done, or timeout
    if (classicBT->isPairingSuccess()) {  // pairing success
        if (timeAfterPairSuccess == 0) {
            // set time and start waiting
            timeAfterPairSuccess = millis();
            NSG_LOG_INFO("PairingMode::waitPairingResult", "Waiting %d ms for camera to make connection...", NIKON_BT_AFTER_PAIR_TIME_MS);
        }
        // wait extra time for camera to make the connection
        if (millis() - timeAfterPairSuccess < NIKON_BT_AFTER_PAIR_TIME_MS) {
            delay(50);
            return;  // keep waiting
        } else {
            NSG_LOG_INFO("PairingMode::waitPairingResult", "Classic BT bond established");
            state = State::SUCCESS;
            screen.clearScreen();
        }
    } else {  // pairing not success
        state = State::FAIL;
        NSG_LOG_ERROR("PairingMode::waitPairingResult", "Pairing failed");
        screen.clearScreen();
    }
}

void PairingMode::saveAndReboot() {
    // draw it anyway
    screen.drawStringMiddleCenter("Saving camera...", 3, 0xd3d3d3, 0x000000, screen.height() / 2);
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
