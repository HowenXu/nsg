#include "NormalMode.h"

#include <M5Unified.h>

#include "../common/NikonBLEClient.h"
#include "../common/NikonBLEScanner.h"
#include "Config.h"
#include "Logging.h"
#include "Screen.h"
#include "UBlox.h"

NormalMode::NormalMode() : connectedCameras() {}

NormalMode::~NormalMode() {
    if (scanner) {
        scanner->stopScanning();
    }
}

void NormalMode::setup() {
    Config::reconcileSavedCamerasWithBondList();

    auto savedCameras = Config::getSavedCameras();
    connectedCameras.reserve(savedCameras.size());
    for (const auto& saved : savedCameras) {
        NSG_LOG_INFO("NormalMode::setup", "Loadding saved camera %s", saved.bleName);
        connectedCameras.emplace_back(saved);
    }

    // init GNSS
    gnss.setRxBufferSize(GNSS_RX_BUFFER_SIZE);
    gnss.begin(UBLOX_GNSS_TARGET_BAUD_RATE, SERIAL_8N1, UBLOX_GNSS_RX_PIN, UBLOX_GNSS_TX_PIN);

    // send signal config with fallback baud rate
    NSG_LOG_INFO("NormalMode::setup", "Config UBlox GNSS...");
    while (true) {
        // drain rx buffer before send ublox command
        while (gnss.available()) gnss.read();
        if (UBlox::sendSatelliteConfig(gnss)) {
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
            M5.Speaker.tone(440, 10000);
            NSG_LOG_FATAL("NormalMode::setup", "Failed to config UBlox GNSS...");
        }
    }
    // update baud rate to target
    if (!nearBaudRate(gnss, UBLOX_GNSS_TARGET_BAUD_RATE)) {
        if (UBlox::setBaudRate(gnss, UBLOX_GNSS_TARGET_BAUD_RATE)) {
            gnss.updateBaudRate(UBLOX_GNSS_TARGET_BAUD_RATE);
            NSG_LOG_DEBUG("NormalMode::setup", "Upgraded GNSS baud rate to %d", UBLOX_GNSS_TARGET_BAUD_RATE);
        } else {
            NSG_LOG_FATAL("NormalMode::setup", "Failed to set GNSS baud rate to %d", UBLOX_GNSS_TARGET_BAUD_RATE);
        }
    }

    scanner.reset(new NikonBLEScanner(NikonBLEScannerMode::PAIRED));
    if (!scanner->startScanning()) {
        NSG_LOG_FATAL("NormalSetup", "failed to start BLE scanning");
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
    // process BLE scan queue
    ScannedCamera scanned;
    bool scanStopped = false;
    while (xQueueReceive(scanner->scanResultQueue, &scanned, (TickType_t)0)) {
        // search for connected, if it is advertising, then it's disconnected
        // we need to (re)initialize the BLE client
        for (auto& item : connectedCameras) {
            if (item.info.bleName == scanned.name && item.info.device == scanned.device) {
                if (item.pClient && !item.pClient->isConnected()) {
                    // disconnected, kill current client and restart
                    item.pClient->disconnect();
                    item.pClient.reset();
                }
                if (!item.pClient) {
                    if (countActiveBLEConnections() >= CONFIG_BTDM_CTRL_BLE_MAX_CONN) {
                        NSG_LOG_WARN("NormalLoop", "Max BLE connections (%d) reached, skipping %s", CONFIG_BTDM_CTRL_BLE_MAX_CONN,
                                     item.info.bleName.c_str());
                        continue;
                    }
                    item.pClient.reset(new NikonBLEClient(rnd, item.info.device, item.info.nonce));
                    if (!scanStopped) {
                        // stop scanning to free up the attenna
                        scanner->stopScanning();
                        scanStopped = true;
                    }
                    auto bleAddr = BLEAddress(scanned.addr);
                    // TODO: handshake is blocking, will it hurt anything?
                    if (!item.pClient->doHandshake(bleAddr, scanned.addrType)) {
                        NSG_LOG_ERROR("NormalLoop", "Failed to reconnect to %s due to handshake failure", bleAddr.toString().c_str());
                        // clean up stale client asap
                        item.pClient.reset();
                    } else {
                        NSG_LOG_INFO("NormalLoop", "BLE connected to %s", bleAddr.toString().c_str());
                        item.lastBroadcastMillis = 0;
                    }
                }
            }
        }
        yield();
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
        snprintf(buffer, sizeof(buffer), "BLE: %d / %d      ", countActiveBLEConnections(), CONFIG_BTDM_CTRL_BLE_MAX_CONN);
        screen.drawString(buffer, 2, 0xd3d3d3, 0x000000,  //
                          middle_left, textX, screen.topBarHeight() + 175);
        // Paired devices
        snprintf(buffer, sizeof(buffer), "CAM: %d / %d      ", connectedCameras.size(), CONFIG_BT_SMP_MAX_BONDS);
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
                M5.Rtc.setDateTime({{(int16_t)year, (int8_t)month, (int8_t)day, 0}, {(int8_t)hour, (int8_t)minute, (int8_t)second}});
                NSG_LOG_INFO("NormalMode::loop", "Synced time from GNSS: %04d/%02d/%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
                nmeaLastSync = millis();
            }
        }

        // only send payload when RTC is valid
        // since GEO payload has time info, and camera will check the time
        // if the time in GEO payload drift from camera's clock, it reject the payload
        if (isRTCValid()) {
            TimeMessage timeMessage(0, 0, 0, 0, 0, 0, 0, 0, 0);
            // loop through cameras and send payload
            for (auto& item : connectedCameras) {
                if (millis() - item.lastBroadcastMillis < NIKON_BLE_UPDATE_INTERVAL_MS) continue;
                if (!item.pClient) continue;
                if (!item.pClient->isConnected()) continue;

                // stop scanning to free up the attenna
                if (!scanStopped) {
                    scanner->stopScanning();
                    scanStopped = true;
                }
                // sending TIME payload
                updateTimeMessageWithRTC(timeMessage);
                NSG_LOG_INFO("NormalLoop", "Sending TIME payload to %s...", item.info.bleName.c_str());
                if (!item.pClient->sendTimePayload(timeMessage)) {
                    NSG_LOG_WARN("NormalLoop", "Failed to send TIME payload to %s", item.info.bleName.c_str());
                    item.pClient->disconnect();
                }
                // sending GEO payload, skip if we already sent a invalid GEO payload
                // otherwise, if we kept sending invalid GEO payload, camera will reject
                if (item.lastGeoValid || gnssValid) {
                    NSG_LOG_INFO("NormalLoop", "Sending GEO payload to %s...", item.info.bleName.c_str());
                    auto geoMessage = generateGeoMessage(lat, lon, altitude, satellites, gnssValid);
                    if (!item.pClient->sendGeoPayload(geoMessage)) {
                        // camera rejected the message, set last geo invalid so we won't send invalid GEO again on reconnect
                        item.lastGeoValid = false;
                        NSG_LOG_WARN("NormalLoop", "Failed to send GEO payload to %s", item.info.bleName.c_str());
                        item.pClient->disconnect();
                    } else {
                        item.lastGeoValid = gnssValid;
                    }
                }
                // update broadcast time
                item.lastBroadcastMillis = millis();
            }
        } else {
            NSG_LOG_WARN("NormalMode::loop", "RTC invalid, skip sending BLE payload");
        }
    }

    // if scan stopped, resume scan
    if (scanStopped) {
        // restart scanning
        if (!scanner->startScanning()) {
            NSG_LOG_FATAL("NormalSetup", "failed to start BLE scanning");
        }
    }
}

void NormalMode::updateTimeMessageWithRTC(TimeMessage& message) {
    auto datetime = M5.Rtc.getDateTime();
    // here is the UTC time
    message.year = datetime.date.year;
    message.month = datetime.date.month;
    message.day = datetime.date.date;
    message.hour = datetime.time.hours;
    message.minute = datetime.time.minutes;
    message.second = datetime.time.seconds;
    message.dstOffset = 0;
    message.tzOffsetHours = Config::getTzOffsetHours();
    message.tzOffsetMinutes = 0;
}

GeoMessage NormalMode::generateGeoMessage(double lat, double lon, int32_t altitude, uint8_t satellites, uint8_t valid) {
    auto datetime = M5.Rtc.getDateTime();
    return GeoMessage::fromDecimal(lat, lon, altitude, satellites, datetime.date.year, datetime.date.month, datetime.date.date, datetime.time.hours,
                                   datetime.time.minutes, datetime.time.seconds, 0, valid);
}

int NormalMode::countActiveBLEConnections() const {
    int count = 0;
    for (const auto& item : connectedCameras) {
        if (item.pClient && item.pClient->isConnected()) {
            count++;
        }
    }
    return count;
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
