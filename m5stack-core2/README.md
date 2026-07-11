# NSG - M5Stack Core2 impl

## Hardware

- M5Stack Core2 v1.1
- u-blox NEO-M10 GPS module

## Software approach

- Use PlatformIO with the Arduino framework and M5Unified.
- The core-1 `loop()` only processes GNSS UART + RTC time-sync and draws the screen. All application-level BLE work (scan-queue handling, (re)connect/handshake, TIME/GEO broadcast) runs in a dedicated FreeRTOS task pinned to core 0 (`BleWorker`), so a (re)connect — which can block for up to 45s — never freezes the UI.
- Drain the GPS serial buffer at the start of every loop so no NMEA data is missed.

Setup:
+ Initialize touch screen, decide mode
+ Call setup_pair() or setup_normal()

Setup Pair:
+ Initialize BLE scan, set up screen
+ Pairing, saving
+ Exit (reboot)

Setup Normal:
+ Start BLE scan, set handler to filter device and check device in manufacture data, put device address to somewhere
+ Setup GPS, set GPS fix = false

Loop (core 1):
+ Process GPS output, save GPS location and fix, update RTC
+ Draw status screen
+ Push GNSS/RTC snapshot to the BLE worker

BLE worker task (core 0):
+ Scan for pending device, do handshake for each one of them
+ For each connected device, check timer and send GPS payload (every 30s)
+ If got 0x80 or other error from camera, disconnect

Structure:
+ SavedCamera (camera name, device, nonce)
+ PendingCamera (camera name, address)
+ ConnectedCamera (camera name, last gps push)

## Modes

The device has two modes:

1. **Pairing mode** — run when the user taps a button during the 3-second boot splash. Scans for a new Nikon camera, runs the 4-stage BLE handshake, bonds over Bluetooth Classic, and saves the camera info.
2. **Normal mode** — the default. Scans for saved cameras, reconnects when in range, and sends the 41-byte GPS payload to the camera whenever a fresh GPS fix is available. Should support multiple cameras connecting at the same time.

## UI / power saving

- Show a simple status screen with GPS fix and connection status.
- Turn the screen backlight off after a timeout; wake it on touch or button press.
- Start with one camera; multi-camera support can be added later.

## Open tasks

- Implement a serial based pairing mode: s -> show scanned result, number -> select a given camera, y/n -> confirm code or reject code
- Add another target, which only has the ESP32 itself and GNSS model
- Figure out how to fit multiple target in the same project?
