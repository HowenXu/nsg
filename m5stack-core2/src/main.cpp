#include <Arduino.h>
#include <BLEDevice.h>

#include "boards/Board.h"
#include "BootMode.h"
#include "Config.h"
#include "Esp32RandomGenerator.h"
#include "Logging.h"
#include "Utils.h"
#include "normal/NormalMode.h"
#include "pairing/PairingMode.h"

BootModeEnum bootModeType = BootModeEnum::NORMAL;
NormalMode* normalMode = nullptr;
PairingMode* pairingMode = nullptr;

void setup() {
    Board::onMainSetupBeforeSerial();

    // enable default serial as monitor
    Serial.begin(115200);
    NSG_LOG_DEBUG("MainSetup", "Serial initialized");

    Board::onMainSetupAfterSerial();

    // init BLE stack (required by both boot modes)
    Esp32RandomGenerator rnd;
    const uint32_t id = Config::getOrGenerateId(rnd);
    const std::string bleDeviceName = Utils::generateFullId(id);
    NSG_LOG_INFO("MainSetup", "BLE device name: %s", bleDeviceName.c_str());
    if (!BLEDevice::init(String(bleDeviceName.c_str()))) {
        NSG_LOG_FATAL("MainSetup", "failed to initialize BLE");
    }

    // collect boot up mode
    NSG_LOG_DEBUG("MainSetup", "Detecting boot mode...");
    bootModeType = Board::detectBootMode();

    switch (bootModeType) {
        case BootModeEnum::NORMAL:
            normalMode = new NormalMode();
            normalMode->setup();
            break;

        case BootModeEnum::PAIRING:
            pairingMode = new PairingMode();
            pairingMode->setup();
            break;

        default:
            NSG_LOG_FATAL("MainSetup", "Unexpected boot type");
            break;
    }

    Board::onMainSetupEnd();
}

void loop() {
    Board::onMainLoopBeforeApp();
    switch (bootModeType) {
        case BootModeEnum::NORMAL:
            if (normalMode) {
                normalMode->loop();
            } else {
                NSG_LOG_FATAL("MainLoop", "Boot type normal but nullptr");
            }
            break;

        case BootModeEnum::PAIRING:
            if (pairingMode) {
                pairingMode->loop();
            } else {
                NSG_LOG_FATAL("MainLoop", "Boot type pairing but nullptr");
            }
            break;

        default:
            NSG_LOG_FATAL("MainLoop", "Unexpected boot type");
            break;
    }
    Board::onMainLoopAfterApp();
}
