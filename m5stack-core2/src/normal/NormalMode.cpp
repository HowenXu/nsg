#include "NormalMode.h"

#include "boards/Board.h"
#include "Config.h"
#include "Logging.h"
#include "UBlox.h"

NormalMode::NormalMode() {}

NormalMode::~NormalMode() { bleWorker.stop(); }

void NormalMode::setup() {
    // init GNSS
    gnss.setRxBufferSize(GNSS_RX_BUFFER_SIZE);
    gnss.begin(UBLOX_GNSS_TARGET_BAUD_RATE, SERIAL_8N1, UBLOX_GNSS_RX_PIN, UBLOX_GNSS_TX_PIN);

    // send signal config with fallback baud rate
    NSG_LOG_INFO("NormalMode::setup", "Config UBlox GNSS...");
    bool onFallbackBaudRate = false;
    while (true) {
        // drain rx buffer before send ublox command
        while (gnss.available()) gnss.read();
        if (UBlox::sendConfig(gnss)) {
            // give ublox GNSS subsystem a bit time to restart after satellite config changes
            delay(500);
            break;
        }
        // still fail, try the fallback baud rate
        if (!onFallbackBaudRate) {
            NSG_LOG_INFO("NormalMode::setup", "Change GNSS baud rate from %d to %d...", gnss.baudRate(), UBLOX_GNSS_FALLBACK_BAUD_RATE);
            gnss.updateBaudRate(UBLOX_GNSS_FALLBACK_BAUD_RATE);
            onFallbackBaudRate = true;
        } else {
            // already on default baud rate but still not working
            esp_restart();
        }
    }
    // update baud rate to target
    if (onFallbackBaudRate) {
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

    GnssSnapshot gnssSnapshot{lat, lon, altitude, satellites, gnssValid};

    Board::onNormalUpdateStatus(gnssSnapshot, bleWorker.getBleStatusSnapshot());

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
                // hold the BLE worker's lock so it cannot read the RTC mid-write
                {
                    BleWorker::Lock lk(bleWorker);
                    Board::setRTC({year, month, day, hour, minute, second});
                }
                NSG_LOG_INFO("NormalMode::loop", "Synced time from GNSS: %04d/%02d/%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
                nmeaLastSync = millis();
            }
        }

        // push current GNSS/RTC state to the BLE worker for payload building
        bleWorker.setGnssSnapshot(gnssSnapshot);
    }
}
