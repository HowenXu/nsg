# NSG - ESP32 impl

## Hardware

Supported boards:
- **M5Stack Core2 v1.1** with u-blox NEO-M10 GPS module — fully functional
- **ESP32 WROOM 32E** with u-blox GNSS module — work in progress

Supported GNSS:
- u-blox NEO-M10 GPS module (or anything speaks the same protocol)

## Software approach

- Use PlatformIO with the Arduino framework. M5Unified is used only on the M5Stack Core2 board.
- A board abstraction layer (`src/boards/Board.h`) decouples application logic from board-specific hardware. Each board provides its own implementation in `src/boards/<board>/Board.cpp`, and only the selected board's sources are compiled via `build_src_filter` in `platformio.ini`.
- The core-1 `loop()` only processes GNSS UART + RTC time-sync and draws the screen. All application-level BLE work (scan-queue handling, (re)connect/handshake, TIME/GEO broadcast) runs in a dedicated FreeRTOS task pinned to core 0 (`BleWorker`), so a (re)connect — which can block for up to 45s — never freezes the UI.
- Drain the GPS serial buffer at the start of every loop so no NMEA data is missed.

### Build environments

No `default_envs` is set — specify the environment explicitly:

| Environment | Board |
|---|---|
| `m5stack-core2-release` / `m5stack-core2-debug` | M5Stack Core2 |
| `esp32-wroom-32e-release` / `esp32-wroom-32e-debug` | ESP32 WROOM 32E |
| `native` | Host machine (unit tests) |

Example: `pio run -e m5stack-core2-release`

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
+ Board abstraction (`src/boards/Board.h`) — board-agnostic interface for setup, loop, pairing UI, normal mode status, and RTC access
+ Board implementations (`src/boards/m5stack-core2/`, `src/boards/esp32-wroom-32e/`)
+ SavedCamera (camera name, device, nonce)
+ PendingCamera (camera name, address)
+ ConnectedCamera (camera name, last gps push)

## Modes

The device has two modes:

1. **Pairing mode** — run when the user taps a button during the 3-second boot splash. Scans for a new Nikon camera, runs the 4-stage BLE handshake, bonds over Bluetooth Classic, and saves the camera info.
2. **Normal mode** — the default. Scans for saved cameras, reconnects when in range, and sends the 41-byte GPS payload to the camera whenever a fresh GPS fix is available. Should support multiple cameras connecting at the same time.

## UI / power saving (M5Stack Core2 only)

- Show a simple status screen with GPS fix and connection status.
- Turn the screen backlight off after a timeout; wake it on touch or button press.
- Start with one camera; multi-camera support can be added later.

## Open tasks

- Finish ESP32 WROOM 32E board support (boot mode detection, RTC, serial-based pairing flow)
