#include "Config.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_bt_api.h>

#include "Logging.h"
#include "RandomGenerator.h"

SavedCameraInfo::SavedCameraInfo(String bleName, uint32_t device, uint32_t nonce, String btAddr)
    : bleName(bleName), device(device), nonce(nonce), btAddr(btAddr) {}

void SavedCameraInfo::addToJsonArray(JsonDocument& parent) const {
    auto doc = parent.add<JsonObject>();
    doc["bleName"] = bleName;
    doc["device"] = device;
    doc["nonce"] = nonce;
    doc["btAddr"] = btAddr;
}

uint32_t Config::getOrGenerateId(RandomGenerator& randomGenerator) {
    Preferences nvs;
    if (!nvs.begin("nsg", false)) {
        NSG_LOG_FATAL("Config::getOrGenerateId", "Failed to open NVS");
    }

    auto result = nvs.getUInt("id", 0);
    while (result == 0) {
        result = randomGenerator.nextUInt32();
        // really bad luck, keep trying
        if (result == 0) continue;
        // save this non-zero id
        if (!nvs.putUInt("id", result)) {
            NSG_LOG_FATAL("Config::getOrGenerateId", "Failed to save id to NVS");
        }
    }

    nvs.end();
    return result;
}

std::vector<SavedCameraInfo> Config::getSavedCameras() {
    Preferences nvs;
    // read-only; if the namespace doesn't exist yet (first boot / NVS erased),
    // there are simply no saved cameras — return empty list instead of crashing
    if (!nvs.begin("nsg", true)) {
        return {};
    }

    auto json = nvs.getString("savedCameras", "[]");
    nvs.end();

    JsonDocument doc;
    auto error = deserializeJson(doc, json);
    if (error) {
        NSG_LOG_FATAL("Config::getSavedCameras", "Failed to parse Json, error: %s", error.c_str());
    }

    std::vector<SavedCameraInfo> result;
    auto jsonArr = doc.as<JsonArray>();
    // extra for potential new item
    result.reserve(jsonArr.size() + 1);

    for (JsonObject item : jsonArr) {
        String bleName = item["bleName"];
        uint32_t device = item["device"];
        uint32_t nonce = item["nonce"];
        String btAddr = item["btAddr"];
        SavedCameraInfo obj(bleName, device, nonce, btAddr);
        result.push_back(obj);
    }

    return result;
}

void Config::addToSavedCameras(const SavedCameraInfo& cameraInfo) {
    std::vector<SavedCameraInfo> cameras = getSavedCameras();

    // loop through existing cameras list and find item with the same name
    // if so, replace the existing item
    bool found = false;
    for (auto& camera : cameras) {
        if (camera.bleName == cameraInfo.bleName) {
            camera = cameraInfo;
            found = true;
            break;
        }
    }
    // otherwise push back
    if (!found) {
        cameras.push_back(cameraInfo);
    }

    JsonDocument doc;
    // convert to json array
    doc.to<JsonArray>();
    // collect items
    for (auto& camera : cameras) {
        camera.addToJsonArray(doc);
    }

    String json;
    serializeJson(doc, json);

    Preferences nvs;
    if (!nvs.begin("nsg", false)) {
        NSG_LOG_FATAL("Config::addToSavedCameras", "Failed to open NVS");
    }
    nvs.putString("savedCameras", json.c_str());
    nvs.end();
}

void Config::reconcileSavedCamerasWithBondList() {
    // Requires Bluedroid to be enabled (BLEDevice::init() must have been called).
    int bondNum = esp_bt_gap_get_bond_device_num();
    if (bondNum < 0) {
        NSG_LOG_WARN("Config::reconcileSavedCamerasWithBondList", "BT stack not enabled, skipping reconcile");
        return;
    }

    auto cameras = getSavedCameras();
    if (cameras.empty()) {
        return;
    }

    // Fetch the actual bond list from the BT stack (ground truth).
    std::vector<esp_bd_addr_t> bondList(bondNum);
    int actualNum = bondNum;
    if (esp_bt_gap_get_bond_device_list(&actualNum, bondList.data()) != ESP_OK) {
        NSG_LOG_WARN("Config::reconcileSavedCamerasWithBondList", "Failed to get bond device list");
        return;
    }

    bool changed = false;
    for (auto it = cameras.begin(); it != cameras.end();) {
        if (it->btAddr.isEmpty()) {
            // No classic BT address recorded — keep (shouldn't happen after erase+flash).
            ++it;
            continue;
        }
        // Parse "xx:xx:xx:xx:xx:xx" into esp_bd_addr_t
        esp_bd_addr_t addr;
        auto scannedLen =
            sscanf(it->btAddr.c_str(), "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx", &addr[0], &addr[1], &addr[2], &addr[3], &addr[4], &addr[5]);
        if (scannedLen != ESP_BD_ADDR_LEN) {
            NSG_LOG_WARN("Config::reconcileSavedCamerasWithBondList", "Malformed btAddr '%s' for %s, keeping", it->btAddr.c_str(),
                         it->bleName.c_str());
            ++it;
            continue;
        }
        // Check if this address still exists in the BT bond list
        bool found = false;
        for (int i = 0; i < actualNum; i++) {
            if (memcmp(bondList[i], addr, ESP_BD_ADDR_LEN) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            NSG_LOG_INFO("Config::reconcileSavedCamerasWithBondList", "Removing orphaned camera %s (bond evicted)", it->bleName.c_str());
            it = cameras.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }

    if (!changed) {
        return;
    }

    // Save filtered list back to NVS
    JsonDocument doc;
    doc.to<JsonArray>();
    for (auto& camera : cameras) {
        camera.addToJsonArray(doc);
    }
    String json;
    serializeJson(doc, json);

    Preferences nvs;
    if (!nvs.begin("nsg", false)) {
        NSG_LOG_FATAL("Config::reconcileSavedCamerasWithBondList", "Failed to open NVS");
    }
    nvs.putString("savedCameras", json.c_str());
    nvs.end();
}
