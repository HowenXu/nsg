#ifndef NORMAL_MODE_H
#define NORMAL_MODE_H

#include <HardwareSerial.h>
#include <MicroNMEA.h>

#include <memory>

#include "../common/NikonBLEScanner.h"
#include "BootMode.h"
#include "ConnectedCamera.h"
#include "Esp32RandomGenerator.h"
#include "GeoMessage.h"
#include "TimeMessage.h"

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

#ifndef NIKON_BLE_UPDATE_INTERVAL_MS
#define NIKON_BLE_UPDATE_INTERVAL_MS 30000
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
    Esp32RandomGenerator rnd;
    std::unique_ptr<NikonBLEScanner> scanner;
    std::vector<ConnectedCamera> connectedCameras;
    // use serial 2, RX on GPIO 13 and TX on GPIO 14
    HardwareSerial gnss = {2};
    char nmeaBuffer[128];
    MicroNMEA nmea = {nmeaBuffer, sizeof(nmeaBuffer)};
    uint32_t nmeaLastSync = 0;

    void updateTimeMessageWithRTC(TimeMessage& message);
    GeoMessage generateGeoMessage(double lat, double lon, int32_t altitude, uint8_t satellites, uint8_t valid);
    int countActiveBLEConnections() const;
    bool isRTCValid();
    bool nearBaudRate(HardwareSerial& serial, uint32_t targetBaudRate);
};

#endif  // NORMAL_MODE_H
