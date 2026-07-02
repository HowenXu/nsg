#include "NormalMode.h"

#include <M5Unified.h>

#include "../common/NikonBLEClient.h"
#include "../common/NikonBLEScanner.h"
#include "Config.h"
#include "Logging.h"
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
    // TODO: GNSS pin and baud rate as build flag/option?
    gnss.setRxBufferSize(4096);
    gnss.begin(115200, SERIAL_8N1, 13, 14);

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
        // still fail
        // note: APB clock is always 80MHz,
        //   for 38400, hardware timer is set to 2080, so baud rate is 80MHz / 2080 = 38461.54 (38461)
        //   for 115200, hw timer is set to 690, so baud rate is 80MHz / 690 = 115942
        if (gnss.baudRate() >= 115000) {
            NSG_LOG_INFO("NormalMode::setup", "Change GNSS baud rate from %d to 38400...", gnss.baudRate());
            // try the default baud rate
            gnss.updateBaudRate(38400);
            // wait for a while
            delay(1000);
        } else {
            // already on default baud rate but still not working
            M5.Speaker.tone(440, 10000);
            NSG_LOG_FATAL("NormalMode::setup", "Failed to config UBlox GNSS...");
        }
    }
    // update baud rate to 115200
    if (gnss.baudRate() < 115000) {
        if (UBlox::setBaudRate(gnss, 115200)) {
            gnss.updateBaudRate(115200);
            NSG_LOG_DEBUG("NormalMode::setup", "Upgraded GNSS baud rate to 115200");
        } else {
            NSG_LOG_FATAL("NormalMode::setup", "Failed to set GNSS baud rate to 115200");
        }
    }

    scanner.reset(new NikonBLEScanner(NikonBLEScannerMode::PAIRED));
    if (!scanner->startScanning()) {
        NSG_LOG_FATAL("NormalSetup", "failed to start BLE scanning");
    }

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
