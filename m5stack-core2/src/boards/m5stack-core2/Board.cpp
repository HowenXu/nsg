#include "../Board.h"

#include "BoardConst.h"
#include "Logging.h"
#include "Screen.h"

void Board::onMainSetupBeforeSerial() {
    // set CPU to 160MHz to save power
    setCpuFrequencyMhz(160);
}

void Board::onMainSetupAfterSerial() {
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
}

BootModeEnum Board::detectBootMode() {
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

void Board::onMainSetupEnd() {
    // sync screen backlight status
    screen.turnOnBacklight();
}

void Board::onMainLoopBeforeApp() {
    M5.update();
    screen.loopBeforeApp();
}

void Board::onMainLoopAfterApp() { screen.loopAfterApp(); }

void Board::onPairingScanning(std::vector<ScannedCamera> const& cameraList, size_t& selectedCameraIdx, std::function<void()> const& onSelect) {
    // draw result on screen
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
    // process user input
    // btnA -> prev page, btnB -> Confirm, btnC -> next page
    if (screen.shouldHandleInput()) {
        if (screen.btnB()->wasPressed() && selectedCameraIdx < cameraList.size()) {
            M5.Speaker.tone(1000, 100);
            onSelect();
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

void Board::onPairingStartBleHandshake(ScannedCamera const& selectedCamera) {
    // draw it anyway because handshake is blocking
    screen.drawStringMiddleCenter("Handshaking...", 3, 0xFFFFFF, 0x000000, screen.height() / 2);
}

void Board::onPairingBleHandshakeSuccess(ScannedCamera const& selectedCamera) { screen.clearScreen(); }

void Board::onPairingBleHandshakeFailed(ScannedCamera const& selectedCamera) { screen.clearScreen(); }

void Board::onPairingStartBtPairingInit(ScannedCamera const& selectedCamera) {
    // draw it anyway because search and bonding is blocking
    screen.drawStringMiddleCenter("Pairing...", 3, 0xFFFFFF, 0x000000, screen.height() / 2);
}

void Board::onPairingBtPairingInitSuccess(ScannedCamera const& selectedCamera) { screen.clearScreen(); }

void Board::onPairingBtPairingInitFailed(ScannedCamera const& selectedCamera) { screen.clearScreen(); }

void Board::onPairingCodeConfirm(bool codeReady, uint32_t code, std::function<void(bool)> const& onConfirm) {
    if (screen.shouldDraw()) {
        screen.drawStringMiddleCenter("Confirm code", 3, 0xFFFFFF, 0x000000, screen.height() / 2 - 50);
    }

    if (!codeReady) {
        delay(100);
        return;
    }

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
            onConfirm(true);
            screen.clearScreen();
        } else if (screen.btnC()->wasPressed()) {
            M5.Speaker.tone(1000, 100);
            onConfirm(false);
            screen.clearScreen();
        }
    }
}

void Board::onPairingWaitBtResult(bool const isPairDone, bool const isPairSuccess, bool const isChangingState) {
    if (screen.shouldDraw()) {
        screen.drawStringMiddleCenter("Connecting...", 3, 0xd3d3d3, 0x000000, screen.height() / 2);
    }
    if (!isPairDone) {
        // pairing not done/timeout, keep waiting
        delay(50);
        return;
    }
    if (isChangingState) {
        // clear screen before changing to next state
        screen.clearScreen();
    }
    // pair done, or timeout
    if (isPairSuccess && !isChangingState) {  // pairing success but waiting
        delay(50);
    }
}

void Board::onPairingFinished(SavedCameraInfo const& camera) {
    // draw it anyway
    screen.drawStringMiddleCenter("Saving camera...", 3, 0xd3d3d3, 0x000000, screen.height() / 2);
}

void Board::onPairingFailed() {
    if (screen.shouldDraw()) {
        screen.drawStringMiddleCenter("FAILED", 3, 0xd30000, 0x000000, screen.height() / 2 - 30);
        screen.drawStringMiddleCenter("Please reset manually", 2, 0xd3d3d3, 0x000000, screen.height() / 2 + 60);
    }
}

void Board::onNormalUpdateStatus(GnssSnapshot const& gnssStatus, BleStatusSnapshot const& bleStatus) {
    if (screen.shouldDraw()) {
        screen.drawStringMiddleCenter("NSG", 3, 0xFFFFFF, 0x000000, screen.topBarHeight() + 20);
        // light gray if fixed, otherwise dark gray
        uint32_t textColor = gnssStatus.gnssValid ? 0xd3d3d3 : 0x404040;
        int32_t textX = 60;

        char buffer[64];
        // GNSS fix, cyan if fixed, otherwise yellow
        // extra 2 space to cover INVALID with VALID
        snprintf(buffer, sizeof(buffer), "FIX: %s  ", gnssStatus.gnssValid ? "VALID" : "INVALID");
        screen.drawString(buffer, 2, gnssStatus.gnssValid ? 0x00FFFF : 0xFFFF00, 0x000000,  //
                          middle_left, textX, screen.topBarHeight() + 50);
        // latitude
        auto latAbs = std::fabs(gnssStatus.lat);
        uint8_t latD = static_cast<uint8_t>(latAbs);
        double latM = (latAbs - latD) * 60.0;
        snprintf(buffer, sizeof(buffer), "LAT: %s %3d\xf8 %06.3f'   ", gnssStatus.lat >= 0 ? "N" : "S", latD, latM);
        screen.drawStringCP437(buffer, 2, textColor, 0x000000,  //
                               middle_left, textX, screen.topBarHeight() + 75);
        // longitude
        auto lonAbs = std::fabs(gnssStatus.lon);
        uint8_t lonD = static_cast<uint8_t>(lonAbs);
        double lonM = (lonAbs - lonD) * 60.0;
        snprintf(buffer, sizeof(buffer), "LON: %s %3d\xf8 %06.3f'   ", gnssStatus.lon >= 0 ? "E" : "W", lonD, lonM);
        screen.drawStringCP437(buffer, 2, textColor, 0x000000,  //
                               middle_left, textX, screen.topBarHeight() + 100);
        // altitude, some extra space to cover digits change
        snprintf(buffer, sizeof(buffer), "ALT: %s %d M      ", gnssStatus.altitudeMeters >= 0 ? "+" : "-",  //
                 gnssStatus.altitudeMeters >= 0 ? gnssStatus.altitudeMeters : -gnssStatus.altitudeMeters);
        screen.drawString(buffer, 2, textColor, 0x000000,  //
                          middle_left, textX, screen.topBarHeight() + 125);
        // satellites count
        snprintf(buffer, sizeof(buffer), "SAT: %d      ", gnssStatus.satellites);
        screen.drawString(buffer, 2, textColor, 0x000000,  //
                          middle_left, textX, screen.topBarHeight() + 150);
        // BLE connection
        snprintf(buffer, sizeof(buffer), "BLE: %u / %d      ", bleStatus.activeConnections, CONFIG_BTDM_CTRL_BLE_MAX_CONN);
        screen.drawString(buffer, 2, 0xd3d3d3, 0x000000,  //
                          middle_left, textX, screen.topBarHeight() + 175);
        // Paired devices
        snprintf(buffer, sizeof(buffer), "CAM: %u paired      ", bleStatus.pairedCount);
        screen.drawString(buffer, 2, 0xd3d3d3, 0x000000,  //
                          middle_left, textX, screen.topBarHeight() + 200);
    }
}

void Board::setRTC(RtcSnapshot const& rtc) {
    // we don't know weekday from GNSS, set it to 0
    M5.Rtc.setDateTime({{(int16_t)rtc.year, (int8_t)rtc.month, (int8_t)rtc.day, 0}, {(int8_t)rtc.hour, (int8_t)rtc.minute, (int8_t)rtc.second}});
}

RtcSnapshot Board::getRTC() {
    auto dt = M5.Rtc.getDateTime();
    return {(uint16_t)dt.date.year, (uint8_t)dt.date.month, (uint8_t)dt.date.date, (uint8_t)dt.time.hours, (uint8_t)dt.time.minutes, (uint8_t)dt.time.seconds};
}
