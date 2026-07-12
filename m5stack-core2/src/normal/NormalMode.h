#ifndef NORMAL_MODE_H
#define NORMAL_MODE_H

#include <HardwareSerial.h>
#include <MicroNMEA.h>

#include "BleWorker.h"
#include "BootMode.h"

#ifndef UBLOX_GNSS_RX_PIN
#define UBLOX_GNSS_RX_PIN 13
#endif

#ifndef UBLOX_GNSS_TX_PIN
#define UBLOX_GNSS_TX_PIN 14
#endif

#ifndef UBLOX_GNSS_TARGET_BAUD_RATE
#define UBLOX_GNSS_TARGET_BAUD_RATE 115200
#endif

#ifndef UBLOX_GNSS_FALLBACK_BAUD_RATE
#define UBLOX_GNSS_FALLBACK_BAUD_RATE 38400
#endif

#ifndef GNSS_TIME_SYNC_INTERVAL_MS
#define GNSS_TIME_SYNC_INTERVAL_MS 120000
#endif

#ifndef GNSS_RX_BUFFER_SIZE
#define GNSS_RX_BUFFER_SIZE 4096
#endif

class NormalMode : public BootMode {
   public:
    NormalMode();
    ~NormalMode();

    void setup() override;
    void loop() override;

   private:
    // Owns all BLE work on a dedicated core-0 task; the core-1 loop only feeds
    // it GNSS/RTC state and reads back BLE status
    BleWorker bleWorker;

    // use serial 2, RX on GPIO 13 and TX on GPIO 14
    HardwareSerial gnss = {2};
    char nmeaBuffer[128];
    MicroNMEA nmea = {nmeaBuffer, sizeof(nmeaBuffer)};
    uint32_t nmeaLastSync = 0;
};

#endif  // NORMAL_MODE_H
