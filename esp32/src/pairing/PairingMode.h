#ifndef PAIRING_MODE_H
#define PAIRING_MODE_H

#include <memory>
#include <vector>

#include "common/NikonBLEClient.h"
#include "common/NikonBLEScanner.h"
#include "common/ScannedCamera.h"
#include "BootMode.h"
#include "ClassicBT.h"
#include "Esp32RandomGenerator.h"

#ifndef NIKON_BT_SEARCH_TIME_MS
#define NIKON_BT_SEARCH_TIME_MS 60000
#endif

#ifndef NIKON_BT_PAIR_TIMEOUT_MS
#define NIKON_BT_PAIR_TIMEOUT_MS 120000
#endif

#ifndef NIKON_BT_AFTER_PAIR_TIME_MS
#define NIKON_BT_AFTER_PAIR_TIME_MS 6000
#endif

class PairingMode : public BootMode {
   public:
    PairingMode();
    ~PairingMode();

    void setup() override;
    void loop() override;

   private:
    enum class State { SCANNING, BLE_HANDSHAKE, PAIRING, SHOW_CODE, CODE_CONFIRM, SUCCESS, FAIL };

    State state;
    Esp32RandomGenerator rnd;
    std::unique_ptr<NikonBLEScanner> scanner;
    std::unique_ptr<NikonBLEClient> pClient;
    std::unique_ptr<ClassicBT> classicBT;
    std::vector<ScannedCamera> cameraList;
    size_t selectedCameraIdx;

    uint32_t timeAfterPairSuccess = 0;

    void handleScanResults();
    void doBLEHandshake();
    void startPairingFlow();
    void showCodeAndWaitConfirm();
    void waitPairingResult();
    void saveAndReboot();

};

#endif  // PAIRING_MODE_H
