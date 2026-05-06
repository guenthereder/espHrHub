# Plan: Fix Makefile Serial Port Detection for ESP32

## Goal
Fix `make upload` to correctly detect the ESP32 board's USB serial port instead of picking Bluetooth-Incoming-Port.

## Scope
- **File to change**: `Makefile` (line 5 - PORT variable definition)

## Out of scope
- Not changing upload logic or error handling
- Not adding `make scan-sensors` in this task (optional future work)
- Not changing board detection or FQBN configuration

## Approach
Update line 5 in Makefile to filter serial ports containing "(USB)" in the description:

**Current:**
```makefile
PORT ?= $(shell arduino-cli board list 2>/dev/null | awk 'NR>1 && $$1!="" {print $$1; exit}')
```

**Fixed:**
```makefile
PORT ?= $(shell arduino-cli board list 2>/dev/null | awk 'NR>1 && $$1!="" && $$5~/USB/ {print $$1; exit}')
```

This filters the `arduino-cli board list` output to only match ports where column 5 (Board Name) contains "USB", which is present in `(USB)` for the ESP32 port:
```
/dev/cu.usbserial-2030          serial   Serial Port (USB) Unknown
```

## Test plan
1. Run `make upload` with ESP32 connected - should use `/dev/cu.usbserial-2030`
2. Verify `make upload` fails correctly when no USB device is connected
3. Test that Bluetooth-Incoming-Port is ignored (no longer picked)

## Risks
- If arduino-cli board list format changes, the filter may break
- Non-USB serial devices won't be detected (acceptable since this is specifically for ESP32 USB upload)

## Open questions
None - plan is ready for implementation. User confirmed Option A (USB filtering) is acceptable.
