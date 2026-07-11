#include "NormalMode.h"

#include <M5Unified.h>

#include "Config.h"
#include "Logging.h"
#include "Screen.h"
#include "UBlox.h"

NormalMode::NormalMode() {}

NormalMode::~NormalMode() { bleWorker.stop(); }

void NormalMode::setup() {
    // init GNSS
    gnss.setRxBufferSize(GNSS_RX_BUFFER_SIZE);
    gnss.begin(UBLOX_GNSS_TARGET_BAUD_RATE, SERIAL_8N1, UBLOX_GNSS_RX_PIN, UBLOX_GNSS_TX_PIN);

    // send signal config with fallback baud rate
    NSG_LOG_INFO("NormalMode::setup", "Config UBlox GNSS...");
    while (true) {
        // drain rx buffer before send ublox command
        while (gnss.available()) gnss.read();
        if (UBlox::sendConfig(gnss)) {
            // give ublox GNSS subsystem a bit time to restart after satellite config changes
            delay(500);
            break;
        }
        // still fail, try the fallback baud rate
        if (nearBaudRate(gnss, UBLOX_GNSS_TARGET_BAUD_RATE)) {
            NSG_LOG_INFO("NormalMode::setup", "Change GNSS baud rate from %d to %d...", gnss.baudRate(), UBLOX_GNSS_FALLBACK_BAUD_RATE);
            gnss.updateBaudRate(UBLOX_GNSS_FALLBACK_BAUD_RATE);
        } else {
            // already on default baud rate but still not working
            esp_restart();
        }
    }
    // update baud rate to target
    if (!nearBaudRate(gnss, UBLOX_GNSS_TARGET_BAUD_RATE)) {
        if (UBlox::setBaudRate(gnss, UBLOX_GNSS_TARGET_BAUD_RATE)) {
            gnss.updateBaudRate(UBLOX_GNSS_TARGET_BAUD_RATE);
            NSG_LOG_DEBUG("NormalMode::setup", "Upgraded GNSS baud rate to %d", UBLOX_GNSS_TARGET_BAUD_RATE);
        } else {
            esp_restart();
        }
    }

    // start the BLE worker on core 0 (loads saved cameras + scanner internally)
    if (!bleWorker.start()) {
        NSG_LOG_FATAL("NormalMode::setup", "failed to start BLE worker");
    }
}

void NormalMode::loop() {
    // first process GNSS UART
    bool nmeaGotNewCommand = false;
    while (gnss.available()) {
        if (nmea.process(gnss.read())) {
            nmeaGotNewCommand = true;
        }
    }

    uint8_t gnssValid = nmea.isValid() ? 1 : 0;
    // lat and lon returned in millionths of a degree
    double lat = ((double)nmea.getLatitude()) / 1000000.0;
    double lon = ((double)nmea.getLongitude()) / 1000000.0;
    int32_t altitude = 0;
    if (!nmea.getAltitude(altitude)) {
        altitude = 0;
        gnssValid = 0;
    }
    // altitude returned in millimeter
    altitude = altitude / 1000;
    uint8_t satellites = nmea.getNumSatellites();

    if (screen.shouldDraw()) {
        auto bleStatus = bleWorker.getBleStatusSnapshot();
        screen.drawStringMiddleCenter("NSG", 3, 0xFFFFFF, 0x000000, screen.topBarHeight() + 20);
        // light gray if fixed, otherwise dark gray
        uint32_t textColor = gnssValid ? 0xd3d3d3 : 0x404040;
        int32_t textX = 60;

        char buffer[64];
        // GNSS fix, cyan if fixed, otherwise yellow
        // extra 2 space to cover INVALID with VALID
        snprintf(buffer, sizeof(buffer), "FIX: %s  ", gnssValid ? "VALID" : "INVALID");
        screen.drawString(buffer, 2, gnssValid ? 0x00FFFF : 0xFFFF00, 0x000000,  //
                          middle_left, textX, screen.topBarHeight() + 50);
        // latitude
        auto latAbs = std::fabs(lat);
        uint8_t latD = static_cast<uint8_t>(latAbs);
        double latM = (latAbs - latD) * 60.0;
        snprintf(buffer, sizeof(buffer), "LAT: %s %3d\xf8 %06.3f'   ", lat >= 0 ? "N" : "S", latD, latM);
        screen.drawStringCP437(buffer, 2, textColor, 0x000000,  //
                               middle_left, textX, screen.topBarHeight() + 75);
        // longitude
        auto lonAbs = std::fabs(lon);
        uint8_t lonD = static_cast<uint8_t>(lonAbs);
        double lonM = (lonAbs - lonD) * 60.0;
        snprintf(buffer, sizeof(buffer), "LON: %s %3d\xf8 %06.3f'   ", lon >= 0 ? "E" : "W", lonD, lonM);
        screen.drawStringCP437(buffer, 2, textColor, 0x000000,  //
                               middle_left, textX, screen.topBarHeight() + 100);
        // altitude, some extra space to cover digits change
        snprintf(buffer, sizeof(buffer), "ALT: %s %d M      ", altitude >= 0 ? "+" : "-", altitude >= 0 ? altitude : -altitude);
        screen.drawString(buffer, 2, textColor, 0x000000,  //
                          middle_left, textX, screen.topBarHeight() + 125);
        // satellites count
        snprintf(buffer, sizeof(buffer), "SAT: %d      ", satellites);
        screen.drawString(buffer, 2, textColor, 0x000000,  //
                          middle_left, textX, screen.topBarHeight() + 150);
        // BLE connection
        snprintf(buffer, sizeof(buffer), "BLE: %d / %d      ", bleStatus.activeConnections, CONFIG_BTDM_CTRL_BLE_MAX_CONN);
        screen.drawString(buffer, 2, 0xd3d3d3, 0x000000,  //
                          middle_left, textX, screen.topBarHeight() + 175);
        // Paired devices
        snprintf(buffer, sizeof(buffer), "CAM: %d paired      ", bleStatus.pairedCount);
        screen.drawString(buffer, 2, 0xd3d3d3, 0x000000,  //
                          middle_left, textX, screen.topBarHeight() + 200);
    }

    // if we got update from GPS
    if (nmeaGotNewCommand) {
        // sync time from GNSS periodically
        if (nmea.isValid() && (millis() - nmeaLastSync > GNSS_TIME_SYNC_INTERVAL_MS || nmeaLastSync == 0)) {
            uint16_t year = nmea.getYear();
            uint8_t month = nmea.getMonth();
            uint8_t day = nmea.getDay();
            uint8_t hour = nmea.getHour();
            uint8_t minute = nmea.getMinute();
            uint8_t second = nmea.getSecond();
            // ensure we have valid time after fix, maybe RMC sentence will come after location fixed
            if (year >= 2026) {
                // we don't know weekday from GNSS, set it to 0
                // hold the BLE worker's lock so it cannot read the RTC mid-write
                {
                    BleWorker::Lock lk(bleWorker);
                    M5.Rtc.setDateTime({{(int16_t)year, (int8_t)month, (int8_t)day, 0}, {(int8_t)hour, (int8_t)minute, (int8_t)second}});
                }
                NSG_LOG_INFO("NormalMode::loop", "Synced time from GNSS: %04d/%02d/%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
                nmeaLastSync = millis();
            }
        }
        bool rtcValid = false;
        // hold the BLE worker's lock so it cannot read the RTC mid-write
        {
            BleWorker::Lock lk(bleWorker);
            rtcValid = isRTCValid();
        }

        // push current GNSS/RTC state to the BLE worker for payload building
        bleWorker.setGnssSnapshot({lat, lon, altitude, satellites, gnssValid, rtcValid});
    }
}

bool NormalMode::isRTCValid() {
    auto datetime = M5.Rtc.getDateTime();
    return datetime.date.year >= 2026;
}

bool NormalMode::nearBaudRate(HardwareSerial& serial, uint32_t targetBaudRate) {
    uint32_t currentBaudRate = serial.baudRate();
    uint32_t diff = currentBaudRate > targetBaudRate ? currentBaudRate - targetBaudRate : targetBaudRate - currentBaudRate;
    // note: APB clock is always 80MHz,
    //   for 38400, hardware timer is set to 2080, so baud rate is 80MHz / 2080 = 38461.54 (38461)
    //   for 115200, hw timer is set to 690, so baud rate is 80MHz / 690 = 115942
    //   so we need to check if the diff is smaller than 2%
    uint32_t gate = targetBaudRate / 50;
    return diff <= gate;
}
