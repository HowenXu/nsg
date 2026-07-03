#include "BleWorker.h"

#include <M5Unified.h>

#include "../common/NikonBLEClient.h"
#include "Config.h"
#include "GeoMessage.h"
#include "Logging.h"
#include "TimeMessage.h"

namespace {

// Fill a TIME message from an already-locked RTC read. The caller is
// responsible for having read `dt` under the BleWorker mutex.
void updateTimeMessageWithRTC(TimeMessage& message, const m5::rtc_datetime_t& dt) {
    // here is the UTC time
    message.year = dt.date.year;
    message.month = dt.date.month;
    message.day = dt.date.date;
    message.hour = dt.time.hours;
    message.minute = dt.time.minutes;
    message.second = dt.time.seconds;
    message.dstOffset = 0;
    message.tzOffsetHours = TZ_OFFSET_HOUR;
    message.tzOffsetMinutes = 0;
}

GeoMessage generateGeoMessage(const GnssSnapshot& snap, const m5::rtc_datetime_t& dt) {
    return GeoMessage::fromDecimal(snap.lat, snap.lon, snap.altitudeMeters, snap.satellites,  //
                                   dt.date.year, dt.date.month, dt.date.date,                 //
                                   dt.time.hours, dt.time.minutes, dt.time.seconds, 0,        //
                                   snap.gnssValid);
}

}  // namespace

StackType_t BleWorker::taskStack[BleWorker::TASK_STACK_WORDS];
StaticTask_t BleWorker::taskBuf;

BleWorker::BleWorker() = default;

BleWorker::~BleWorker() { stop(); }

bool BleWorker::start() {
    if (taskHandle) return true;
    mutex = xSemaphoreCreateMutex();
    if (!mutex) {
        NSG_LOG_ERROR("BleWorker::start", "failed to create mutex");
        return false;
    }
    stopFlag = false;
    taskHandle = xTaskCreateStaticPinnedToCore(taskEntry, "BleWorker", TASK_STACK_WORDS, this, 1, taskStack, &taskBuf, 0);
    if (!taskHandle) {
        NSG_LOG_ERROR("BleWorker::start", "failed to create task");
        vSemaphoreDelete(mutex);
        mutex = nullptr;
        return false;
    }
    return true;
}

void BleWorker::stop() {
    if (taskHandle) {
        stopFlag = true;
        // the worker polls stopFlag at most once per second (queue timeout),
        // so wait up to 50s for BLE clients that trying to make connection
        for (int i = 0; i < 500 && taskHandle != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (taskHandle != nullptr) {
            NSG_LOG_WARN("BleWorker::stop", "task did not exit gracefully, force deleting");
            vTaskDelete(taskHandle);
            taskHandle = nullptr;
        }
    }
    if (scanner) {
        scanner->stopScanning();
        scanner.reset();
    }
    connectedCameras.clear();
    if (mutex) {
        vSemaphoreDelete(mutex);
        mutex = nullptr;
    }
}

void BleWorker::setGnssSnapshot(const GnssSnapshot& snap) {
    Lock lk(*this);
    gnssSnap = snap;
}

BleStatusSnapshot BleWorker::getBleStatusSnapshot() {
    Lock lk(*this);
    return bleSnap;
}

BleWorker::Lock::Lock(BleWorker& w) : w(w) {
    if (w.mutex) xSemaphoreTake(w.mutex, portMAX_DELAY);
}

BleWorker::Lock::~Lock() {
    if (w.mutex) xSemaphoreGive(w.mutex);
}

size_t BleWorker::countActiveBLEConnections() {
    size_t count = 0;
    for (const auto& item : connectedCameras) {
        if (item.pClient && item.pClient->isConnected()) {
            count++;
        }
    }
    return count;
}

void BleWorker::taskEntry(void* arg) {
    auto* self = static_cast<BleWorker*>(arg);
    self->taskLoop();
    self->taskHandle = nullptr;
    vTaskDelete(nullptr);
}

void BleWorker::taskLoop() {
    // init: load saved cameras + scanner
    auto savedCameras = Config::getSavedCameras();
    connectedCameras.reserve(savedCameras.size());
    for (const auto& saved : savedCameras) {
        NSG_LOG_INFO("BleWorker::taskLoop", "Loading saved camera %s", saved.bleName.c_str());
        connectedCameras.emplace_back(saved);
    }

    scanner.reset(new NikonBLEScanner(NikonBLEScannerMode::PAIRED));
    if (!scanner->startScanning()) {
        NSG_LOG_FATAL("BleWorker::taskLoop", "failed to start BLE scanning");
    }

    while (!stopFlag) {
        bool scanStopped = false;

        // 1. block up to 1s for a scan result, then drain the rest (non-blocking).
        //    The 1s timeout doubles as the stop-flag poll interval and keeps
        //    the 30s broadcast timer ticking even with no advertising cameras.
        ScannedCamera scanned;
        if (xQueueReceive(scanner->scanResultQueue, &scanned, pdMS_TO_TICKS(1000))) {
            do {
                // search for a connected, if it is advertising, then it's disconnected
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
                                NSG_LOG_WARN("BleWorker", "Max BLE connections (%d) reached, skipping %s", CONFIG_BTDM_CTRL_BLE_MAX_CONN,
                                             item.info.bleName.c_str());
                                continue;
                            }
                            item.pClient.reset(new NikonBLEClient(rnd, item.info.device, item.info.nonce));
                            if (!scanStopped) {
                                // stop scanning to free up the antenna
                                scanner->stopScanning();
                                scanStopped = true;
                            }
                            auto bleAddr = BLEAddress(scanned.addr);
                            if (!item.pClient->doHandshake(bleAddr, scanned.addrType)) {
                                NSG_LOG_ERROR("BleWorker", "Failed to reconnect to %s due to handshake failure", bleAddr.toString().c_str());
                                // clean up stale client asap
                                item.pClient.reset();
                            } else {
                                NSG_LOG_INFO("BleWorker", "BLE connected to %s", bleAddr.toString().c_str());
                                item.lastBroadcastMillis = 0;
                            }
                        }
                    }
                }
                yield();
            } while (!stopFlag && xQueueReceive(scanner->scanResultQueue, &scanned, (TickType_t)0));
        }

        // 2. periodic broadcast: gather shared state under the mutex
        GnssSnapshot snap;
        {  // scope for lock to deconstruct when exit
            Lock lk(*this);
            snap = gnssSnap;
        }

        // only send payload when RTC is valid, since GEO payload has time info
        // and the camera will reject it if the time drifts from its own clock.
        if (snap.rtcValid) {
            TimeMessage timeMessage(0, 0, 0, 0, 0, 0, 0, 0, 0);
            for (auto& item : connectedCameras) {
                if (millis() - item.lastBroadcastMillis < NIKON_BLE_UPDATE_INTERVAL_MS) continue;
                if (!item.pClient) continue;
                if (!item.pClient->isConnected()) continue;

                // stop scanning to free up the antenna
                if (!scanStopped) {
                    scanner->stopScanning();
                    scanStopped = true;
                }
                // sending TIME payload, first getting the latest time
                m5::rtc_datetime_t dt{};
                {
                    Lock lk(*this);
                    dt = M5.Rtc.getDateTime();
                }
                updateTimeMessageWithRTC(timeMessage, dt);
                NSG_LOG_INFO("BleWorker", "Sending TIME payload to %s...", item.info.bleName.c_str());
                if (!item.pClient->sendTimePayload(timeMessage)) {
                    NSG_LOG_WARN("BleWorker", "Failed to send TIME payload to %s", item.info.bleName.c_str());
                    item.pClient->disconnect();
                }
                // sending GEO payload, skip if we already sent an invalid GEO payload
                // otherwise, if we kept sending invalid GEO payload, camera will reject
                if (item.lastGeoValid || snap.gnssValid) {
                    NSG_LOG_INFO("BleWorker", "Sending GEO payload to %s...", item.info.bleName.c_str());
                    auto geoMessage = generateGeoMessage(snap, dt);
                    if (!item.pClient->sendGeoPayload(geoMessage)) {
                        // camera rejected the message, set last geo invalid so we won't send invalid GEO again on reconnect
                        item.lastGeoValid = false;
                        NSG_LOG_WARN("BleWorker", "Failed to send GEO payload to %s", item.info.bleName.c_str());
                        item.pClient->disconnect();
                    } else {
                        item.lastGeoValid = snap.gnssValid;
                    }
                }
                // update broadcast time
                item.lastBroadcastMillis = millis();
            }
        }

        // 3. refresh the status snapshot for the UI.
        {
            Lock lk(*this);
            bleSnap.activeConnections = countActiveBLEConnections();
            bleSnap.pairedCount = connectedCameras.size();
        }

        // 4. resume scanning if we stopped it for radio-sensitive operations.
        if (scanStopped && !stopFlag) {
            if (!scanner->startScanning()) {
                NSG_LOG_FATAL("BleWorker", "failed to start BLE scanning");
            }
        }
    }

    // cleanup on stop
    if (scanner) {
        scanner->stopScanning();
    }
    for (auto& item : connectedCameras) {
        if (item.pClient) {
            item.pClient->disconnect();
            item.pClient.reset();
        }
    }
}
