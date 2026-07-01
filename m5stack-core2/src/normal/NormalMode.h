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
};

#endif  // NORMAL_MODE_H
