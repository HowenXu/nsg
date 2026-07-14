#ifndef NIKON_BLE_SCANNER_H
#define NIKON_BLE_SCANNER_H

#include <BLEDevice.h>
#include <string>
#include "ScannedCamera.h"

enum class NikonBLEScannerMode {
    NEW_DEVICE,  // camera in pairing mode (no/other manufacturer data) -- was PairingScanner
    PAIRED       // already-paired camera advertising manufacturer data -- was PairedScanner
};

class NikonBLEScanner : public BLEAdvertisedDeviceCallbacks {
   public:
    explicit NikonBLEScanner(NikonBLEScannerMode mode);
    ~NikonBLEScanner();
    bool startScanning();
    void stopScanning();

    // read/receive Camera value from here
    QueueHandle_t scanResultQueue;

   private:
    BLEScan* pBLEScan;
    NikonBLEScannerMode mode;
    // callback
    void onResult(BLEAdvertisedDevice advertisedDevice) override;
    // fill a ScannedCamera object
    void fillScannedCamera(ScannedCamera* result, std::string deviceName, uint32_t device, BLEAddress deviceAddr, uint8_t addrType);
};

#endif
