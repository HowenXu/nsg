#ifndef CLASSIC_BT_H
#define CLASSIC_BT_H

#include <BluetoothSerial.h>

#include <atomic>
#include <string>

class ClassicBT {
   public:
    ClassicBT(std::string name);
    ~ClassicBT();
    // return true -> start pairing, need to check and confirm code.
    // return false -> failed
    bool searchAndInitiatePair(uint32_t searchTimeoutMs);
    bool isPairCodeReady();
    uint32_t getPairCode();
    // only send the confirm reply
    void confirmPairCode(bool accept);
    bool isPairingDone(const uint32_t timeoutMs);
    bool isPairingSuccess();

   private:
    BluetoothSerial serialBT;
    std::atomic_uint32_t pairCode;
    std::string targetName;
    // these bool flags are shared with callbacks
    std::atomic_bool pairCodeReady = false;
    std::atomic_bool authDone = false;
    std::atomic_bool authSuccess = false;
    // pairing timer, set after send confirm replay
    // used to calculate isPairingTimeout
    uint32_t authStart = 0;
    // classic BT address of the paired device, filled by searchAndInitiatePair()
    uint8_t classicAddr[ESP_BD_ADDR_LEN] = {0};
};

#endif
