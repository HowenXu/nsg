# nsg Android remote

English | [中文](README.zh-CN.md)

The Android app turns a spare phone into a GPS source for Nikon Z cameras. It connects over Bluetooth smart-device mode and continuously writes location data into the camera, so photos come out geotagged.

This is the Android part of [hurui200320/nsg](https://github.com/hurui200320/nsg). The SnapBridge pairing protocol was reverse engineered by [gkoh/furble](https://github.com/gkoh/furble). The device-ID recovery algorithm lives in [snapbridge-id-extractor](https://github.com/HowenXu/snapbridge-id-extractor).

## Status

The app works and is used regularly. The current version is 1.0.1.

It has been tested on a Nikon Z7 II. In theory it should work with any Z camera that supports SnapBridge smart-device mode over Bluetooth. Only Bluetooth mode is implemented, so Z-mount mirrorless cameras and the last generation of DSLRs with Bluetooth are supported.

Phones tested so far: Huawei Mate 40 Pro and Huawei MatePad Pro 10.8. The minimum Android version is 7.0 (API 24).

## What it does

- Pairs a new camera and lists nearby cameras.
- Connects to saved cameras, including a default camera at startup.
- Recovers the SnapBridge device identity from an existing pairing, so you can switch between SnapBridge and this app without deleting the pairing record.
- Injects GPS and network location into the camera.
- Keeps a foreground service alive in the background and reconnects when the camera comes back.
- Shows a small log panel so connection and extraction progress are visible.
- Follows the system language: Chinese on Chinese systems, English otherwise.

## Install

Download `nsg.apk` from [Releases](https://github.com/HowenXu/nsg/releases) and install it.

On first launch, grant:

- Location permission, used for GPS.
- Bluetooth permission, used to connect to the camera.
- Notification permission, needed by the foreground service.
- Battery-optimisation exemption. On Chinese ROMs, also add the app to the autostart/background whitelist.

## Pair a camera

### Camera already used with SnapBridge

This is the common case.

1. Open the app and tap **Pair new camera**.
2. Pick the camera from the list.
3. If the camera already has a SnapBridge pairing record, the app runs the device-ID recovery automatically. The log panel shows the progress.
4. When it finishes, the app connects and the status becomes **Ready**.

After that, use **Connect saved camera** for normal connections. Switching back to SnapBridge does not require deleting anything.

### New camera

Tap **Pair new camera**, select the camera, and confirm the system Bluetooth pairing dialog.

## Use

Once the status is **Ready**, the camera LCD shows the GPS indicator or coordinates. Photos taken at that point should contain location data in EXIF.

You can leave the app in the background. The foreground service keeps the connection alive and continues sending location updates at a low rate.

## Advanced settings

Open the three-dot menu and choose **Advanced settings**. Most people do not need to change anything here.

- **Camera-side device name**: the controller name shown by the camera. It is generated in the SnapBridge format by default.
- **Fixed device ID**: the complete 16-hex SnapBridge identity. The app fills it in after a successful extraction.

## Known issues

- Do not clear SnapBridge's data or reinstall it. It will generate a new identity and you will need to run the extraction again.
- The Nikon camera LCD sometimes renders fractional minutes incorrectly. For example, `51.002'` may appear as `51.2'`. The EXIF value written to the photo is still correct.
- Only Bluetooth smart-device mode is supported. Cameras that require Wi-Fi only are out of scope.
- Some Chinese ROMs kill background services aggressively. The battery and autostart settings matter more than the app itself on those phones.

## Build

Local release builds are unsigned by default, so any clean checkout can build without a keystore:

```bash
cd android
./gradlew assembleRelease
```

The output is `app/build/outputs/apk/release/app-release-unsigned.apk`.

The release workflow builds, signs, verifies, and uploads a signed `nsg.apk` when a `v*` tag is pushed. The workflow needs these repository secrets:

| Secret | Meaning |
| --- | --- |
| `KEYSTORE_BASE64` | Base64 of the release keystore |
| `KEYSTORE_PASSWORD` | Keystore password |
| `KEY_ALIAS` | Key alias |
| `KEY_PASSWORD` | Key password |

The project is licensed under the terms in the repository root. See [CONTRIBUTORS.md](../CONTRIBUTORS.md) for contributors.
