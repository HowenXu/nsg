#include <Arduino.h>
#include <BLEDevice.h>
#include <M5Unified.h>

#include "BootMode.h"
#include "Config.h"
#include "Esp32RandomGenerator.h"
#include "Logging.h"
#include "Screen.h"
#include "Utils.h"
#include "normal/NormalMode.h"
#include "pairing/PairingMode.h"

BootModeEnum detectBootMode() {
    const uint32_t WAIT_MS = 6000;
    const uint32_t THRESHOLD_MS = 1200;
    const uint32_t REFRESH_MS = 30;

    // draw text
    screen.clearScreen();
    screen.drawStringMiddleCenter("Holding Btn C to", 3, 0xFFFF00, 0x000000, screen.height() / 2 - 45);
    screen.drawStringMiddleCenter("pair new device", 3, 0xFFFF00, 0x000000, screen.height() / 2 - 5);

    uint32_t startMs = millis();
    uint32_t touched = 0;

    while (millis() - startMs < WAIT_MS) {
        M5.update();
        // check BtnC pressed and give feedback
        // here we use M5.BtnC directly to avoid setup sequence issue
        if (M5.BtnC.isPressed()) {
            if (M5.BtnC.wasPressed()) {
                // a short beep on detection to notify user the press has been registered
                M5.Speaker.tone(1000, 100);
            }
            touched += REFRESH_MS;
            if (touched >= THRESHOLD_MS) break;
        } else {
            touched = 0;
        }

        // show counter
        uint32_t remaining = (WAIT_MS - (millis() - startMs)) / 1000;
        auto autoBootStr = String("Auto normal in ") + remaining + "s";
        screen.drawStringMiddleCenter(autoBootStr.c_str(), 2, 0xd3d3d3, 0x000000, screen.height() / 2 + 50);
        delay(REFRESH_MS);
    }

    // clear screen
    screen.clearScreen();
    if (touched >= THRESHOLD_MS) {
        NSG_LOG_INFO("detectBootMode", "Booting into pairing mode...");
        return BootModeEnum::PAIRING;
    } else {
        NSG_LOG_INFO("detectBootMode", "Booting into normal mode...");
        return BootModeEnum::NORMAL;
    }
}

BootModeEnum bootModeType = BootModeEnum::NORMAL;
NormalMode* normalMode = nullptr;
PairingMode* pairingMode = nullptr;

void setup() {
    // set CPU to 160MHz to save power
    setCpuFrequencyMhz(160);
    // enable default serial as monitor
    Serial.begin(115200);
    NSG_LOG_DEBUG("MainSetup", "Serial initialized");
    // Initialize M5 for screen, etc.
    M5.begin();
    NSG_LOG_DEBUG("MainSetup", "M5 initialized");

    if (!M5.Rtc.isEnabled()) {
        NSG_LOG_FATAL("MainSetup", "RTC not found");
    }

    if (!M5.Rtc.begin()) {
        NSG_LOG_FATAL("MainSetup", "failed to initialize RTC");
    }
    // init device
    M5.Power.setLed(255);
    M5.Speaker.setVolume(255);
    M5.Display.setBrightness(255);

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
    bootModeType = detectBootMode();

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
    // sync screen backlight status
    screen.turnOnBacklight();
}

void loop() {
    M5.update();

    screen.loopBeforeApp();

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

    screen.loopAfterApp();
}
