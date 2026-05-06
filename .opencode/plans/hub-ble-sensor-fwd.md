# Plan: ESP32 Heart Rate Hub Implementation

## Goal
Implement a complete BLE heart rate hub that acts as both peripheral (to Apple Watch) and central (to multiple HR sensors), with scanning, sensor selection, robust reconnection, and data forwarding.

## Scope
- `espHrHub.ino` — Complete rewrite of BLE scanning, sensor management, reconnection logic
- `config.h` — New file for user configuration (pins, MAC addresses)
- `.gitignore` — Update to exclude config.h

## Out of scope
- No display/HTML UI (Serial only for now)
- No OTA updates
- No data logging/storage
- No battery management

## Approach
1. **Create config.h** with pin definitions, sensor storage (array of structs), and configurable parameters
2. **Implement BLE scan callback** to discover sensors with service "180D"
3. **Add button (GPIO)** for manual sensor discovery triggered from main loop
4. **Maintain sensor list** (struct with MAC, lastHR, lastTimestamp, connected flag)
5. **Implement exponential backoff** reconnection with state tracking
6. **Sensor switching logic** via Serial commands ('S' to scan, '1','2',... to select)
7. **LED feedback** (blinking = scanning, solid = connected, off = disconnected)

## Test Plan
- Compile test: `make verify` should pass without errors
- Manual tests (to be run by user with board):
  1. Power on, verify serial output shows "Scanning for sensors..."
  2. Press button, verify scan output lists discovered sensors
  3. Send serial command to select sensor (e.g., '1'), verify connection
  4. Monitor heart rate data forwarding to Apple Watch
  5. Simulate disconnection, verify reconnection with backoff delay

## Risks
- NimBLE client limits: ESP32 may only support ~5 simultaneous connections
- Button debounce needed to prevent multiple scan triggers
- Memory fragmentation with dynamic client creation/destruction
- BLE advertising interference during scanning (may need to stop advertising temporarily)

## Open questions
1. How many sensor slots should we support? (recommend 4-6 max)
2. Should we store discovered MACs in flash/EEPROM for persistence?
3. Should sensor switching be instant or require disconnection/reconnection delay?
4. What pin to use for the discovery button? (GPIO0 commonly used, but may conflict with serial)
