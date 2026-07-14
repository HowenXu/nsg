#ifndef BLE_WORKER_H
#define BLE_WORKER_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "common/NikonBLEScanner.h"
#include "boards/Board.h"
#include "ConnectedCamera.h"
#include "Esp32RandomGenerator.h"

// Min interval between TIME/GEO broadcasts sent to a single camera.
#ifndef NIKON_BLE_UPDATE_INTERVAL_MS
#define NIKON_BLE_UPDATE_INTERVAL_MS 30000
#endif

#ifndef TZ_OFFSET_HOUR
#define TZ_OFFSET_HOUR 8
#endif

// Owns all application-level BLE work and runs it on a dedicated core-0 task,
// offloading the core-1 loop so the screen keeps drawing during (re)connects.
//
// Concurrency rules:
//  - `scanner`, `connectedCameras`, `rnd` are touched ONLY by the worker task.
//  - `GnssSnapshot` / `BleStatusSnapshot` are read/written under the mutex.
//  - RTC reads the worker performs for payload building run under the same
//    mutex as the snapshots, so they cannot race with the core-1 time-sync
//    write (for example, M5.Rtc.setDateTime).
class BleWorker {
   public:
    // 16 KB stack (4096 words of StackType_t), placed in internal RAM via
    // static buffers (a dynamic stack could land in PSRAM under
    // CONFIG_SPIRAM_USE_MALLOC, which is unsafe for task stacks).
    // 16 KB is needed because the worker task frame holds all locals
    // (GnssSnapshot, TimeMessage, GeoMessage, …) simultaneously, and calls
    // Config::getSavedCameras() which puts a ~256-byte JsonDocument on the
    // stack plus deserializeJson overhead — all before the BLE handshake
    // (Blowfish + GATT) adds its own depth.
    static constexpr size_t TASK_STACK_WORDS = 4096;

    BleWorker();
    ~BleWorker();

    // Create the worker task pinned to core 0. Returns false on failure.
    bool start();

    // Signal the worker to stop and join it. Safe to call multiple times.
    void stop();

    // Feed GNSS/RTC state from the core-1 loop (thread-safe).
    void setGnssSnapshot(const GnssSnapshot& snap);

    // Read BLE status for the screen (thread-safe).
    BleStatusSnapshot getBleStatusSnapshot();

    // RAII guard for shared resources protected by the internal mutex
    // (snapshots + RTC). Usable from any task.
    class Lock {
       public:
        explicit Lock(BleWorker& w);
        ~Lock();

       private:
        BleWorker& w;
    };

   private:
    // BLE-owned resources: touched ONLY by the worker task.
    Esp32RandomGenerator rnd;
    std::unique_ptr<NikonBLEScanner> scanner;
    std::vector<ConnectedCamera> connectedCameras;

    // Cross-task state, guarded by mutex.
    SemaphoreHandle_t mutex = nullptr;
    GnssSnapshot gnssSnap;
    BleStatusSnapshot bleSnap;

    // Task control.
    std::atomic<bool> stopFlag = false;
    std::atomic<TaskHandle_t> taskHandle = nullptr;
    static StackType_t taskStack[TASK_STACK_WORDS];
    static StaticTask_t taskBuf;

    static void taskEntry(void* arg);
    void taskLoop();

    // Worker-only helper.
    size_t countActiveBLEConnections();
    bool isRTCValid();
};

#endif  // BLE_WORKER_H
