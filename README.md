# espHrHub

BLE Heart Rate Hub for ESP32. Connects to external heart rate sensors (e.g., Wahoo TICKR FIT) and forwards live HR data to an Apple Watch or any BLE HR-capable device.

## What It Does

- **Central + Peripheral** in one: The ESP32 acts as a BLE central to connect to HR straps, and simultaneously as a BLE peripheral advertising as a heart rate sensor.
- **Auto-connect** to configured sensors at boot.
- **BOOT button** (GPIO0) triggers a scan to discover and auto-connect new HR sensors.
- **Robust reconnection** with exponential backoff when sensors disconnect.
- **Deferred cleanup** of BLE clients to avoid Bluedroid race-condition crashes.

## Hardware

- ESP32 dev board (tested on ESP32-WROOM-32)
- External BLE heart rate sensor (tested with Wahoo TICKR FIT)
- Apple Watch or any BLE HR monitor app for receiving forwarded data

## Quick Start

### 1. Configure Your Sensor

Copy `config.h` (or edit it) and set your sensor's MAC address:

```cpp
static const uint8_t DISCOVERED_MAC_ADDRESSES[MAX_SENSOR_SLOTS][6] = {
  {0xF0, 0x13, 0xC3, 0xFE, 0x70, 0x12}, // Your sensor MAC (little-endian)
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // Slot 1 - empty
  // ...
};
```

> **Note:** The Arduino BLE library (Bluedroid) stores MAC bytes in **little-endian** / **LSB-first** order. For human-readable MAC `12:70:FE:C3:13:F0`, store `{0xF0, 0x13, 0xC3, 0xFE, 0x70, 0x12}`.

### 2. Build

```bash
make build
```

Requires `arduino-cli` with the ESP32 core installed.

### 3. Flash

Connect the ESP32 via USB, then:

```bash
make upload
```

If upload fails, ground GPIO0 to GND and press RESET to enter download mode.

### 4. Pair with Apple Watch

Open the **Heart Rate** app on your Apple Watch. The ESP should appear as **"espHRhub"** in the sensor list. Select it to connect.

## Usage

### Serial Commands

Connect to the ESP32's serial port (115200 baud) to send commands:

| Command | Action |
|---------|--------|
| `S` / `s` | Scan for BLE sensors |
| `1`–`4` | Switch to sensor slot 1–4 |
| `c` / `C` | Force reconnect slot 0 |
| `?` | Show help |

### BOOT Button

Press the **BOOT button** (GPIO0) to trigger a scan. The ESP will:
1. Stop advertising
2. Scan for 5 seconds
3. Auto-connect to the first HR sensor found (if slot 0 is empty/disconnected)
4. Resume advertising

## Project Structure

```
espHrHub/
├── espHrHub.ino      # Main sketch (BLE central + peripheral)
├── config.h          # User config: MAC addresses, pins, timeouts
├── Makefile          # Build targets (build, upload, test, lint)
├── AGENTS.md         # Agent-specific build/test conventions
├── tests/            # pytest-based hardware integration tests
│   ├── tests.ino     # Test/debug sketch
│   ├── conftest.py   # Pytest fixtures (serial, flashing)
│   └── test_hubsanity.py
└── scripts/
    └── reset-strap.py
```

## Build Targets

```bash
make build        # Compile only
make upload       # Compile + flash to connected ESP32
make flash-only   # Flash without running tests
make test         # Run pytest hardware tests (requires ESP32 connected)
make lint         # Syntax-check Python test files
make verify       # Full gate: build + test
make reset-strap  # Reset ESP and trigger strap scan via serial
```

## How It Works

### BLE Architecture

The ESP32 runs **dual BLE roles** simultaneously:

- **BLE Central (Client)**: Scans for and connects to external HR sensors (UUID `180D`). Subscribes to notifications on characteristic `2A37`.
- **BLE Peripheral (Server)**: Advertises as a heart rate sensor with service UUID `180D`. Exposes `2A37` (HR measurement) with a `BLE2902` CCCD descriptor so Apple Watch can subscribe.

When an HR notification arrives from the strap, it is immediately forwarded to the watch via `characteristic->notify()`.

### Reconnection Logic

- `checkSensorReconnections()` runs in `loop()` every 50ms
- If a configured sensor is not connected, it retries with exponential backoff (1s → 2s → 4s ... up to 30s)
- Disconnect callbacks clear slot state so reconnection logic kicks in

### Why Deferred Client Deletion?

The ESP32's Bluedroid stack processes disconnect events asynchronously from a FreeRTOS task. Deleting a `BLEClient` while queued events still reference it causes heap corruption and crash/reboot loops. The fix:

1. Orphan old `BLEClient` to a side array
2. Wait ≥500ms in `loop()` for Bluedroid to drain its event queue
3. Delete only when `client->isConnected()` is false

See commit `00a4606` for details.

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| "All address types exhausted" | Wrong MAC byte order in `config.h` | Reverse bytes (LSB-first) |
| Watch sees "espHRhub" but no HR data | No sensor connected | Press BOOT button to scan/connect |
| ESP crashes/reboots when sensor turns off | `BLEClient` deleted during async disconnect | Already fixed (deferred deletion) |
| Upload fails | Wrong port or not in download mode | Check `arduino-cli board list`; ground GPIO0 + reset |

## Dependencies

- [arduino-cli](https://arduino.github.io/arduino-cli/latest/installation/)
- ESP32 Arduino Core v3.x (via `arduino-cli core install esp32:esp32`)
- Python 3 + pytest (for hardware tests)

## License

MIT
