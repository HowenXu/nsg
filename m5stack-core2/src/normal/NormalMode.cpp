#include "NormalMode.h"

#include <M5Unified.h>

#include "../common/NikonBLEClient.h"
#include "../common/NikonBLEScanner.h"
#include "Config.h"
#include "Logging.h"

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

    scanner.reset(new NikonBLEScanner(NikonBLEScannerMode::PAIRED));
    if (!scanner->startScanning()) {
        NSG_LOG_FATAL("NormalSetup", "failed to start BLE scanning");
    }

    // init GNSS
    // TODO: GNSS pin and baud rate as build flag/option?
    gnss.begin(38400, SERIAL_8N1, 13, 14);

    // TODO: check RTC, if year < 2026, force waiting for GPS fix to update time

    // TODO: currently do not need screen, add timeout for backlight?
    M5.Display.setBrightness(0);
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
    }

    // if we got update from GPS
    if (nmeaGotNewCommand) {
        // sync time from GNSS every 2 minutes
        if (nmea.isValid() && (millis() - nmeaLastSync > 120000 || nmeaLastSync == 0)) {
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

        TimeMessage timeMessage(0, 0, 0, 0, 0, 0, 0, 0, 0);
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
        // loop through cameras and send payload
        for (auto& item : connectedCameras) {
            // TODO: broadcast interval as build flag/option?
            if (millis() - item.lastBroadcastMillis < 30000) continue;
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
            // sending GEO payload
            NSG_LOG_INFO("NormalLoop", "Sending GEO payload to %s...", item.info.bleName.c_str());
            auto geoMessage = generateGeoMessage(lat, lon, altitude, satellites, gnssValid);
            if (!item.pClient->sendGeoPayload(geoMessage)) {
                NSG_LOG_WARN("NormalLoop", "Failed to send GEO payload to %s", item.info.bleName.c_str());
                item.pClient->disconnect();
            }
            NSG_LOG_INFO("NormalMode::loop", "GNSS: valid=%d, lat=%f, lon=%f, alt=%d, sat=%d", gnssValid, lat, lon, altitude, satellites);

            // update broadcast time
            item.lastBroadcastMillis = millis();
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
