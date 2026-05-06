# Plan: Fix ESP32 Upload Error - Add Baud Rate Option

## Goal
Fix the "Chip Stopped Responding" upload error by adding `-b 115200` baud rate flag to slow down the upload process and prevent communication issues.

## Scope
- **File to change**: `Makefile` (line 14-16 - upload target)
- **No other files affected**

## Out of scope
- Not changing the serial port detection logic (handled in previous task)
- Not implementing manual reset via GPIO0 (Option C) - too board-specific
- Not adding verbose error retry logic beyond the basic baud rate fix

## Approach
1. Modify the `upload` target in Makefile to add `-b 115200` flag
2. Add error handling that suggests manual reset if upload fails
3. Keep the build target unchanged

**Current:**
```makefile
upload:
	@[ -n "$(PORT)" ] || (echo "FAIL: no board detected. Connect the ESP32 and retry."; exit 1)
	arduino-cli upload -p $(PORT) --fqbn $(BOARD) "$(SKETCH_DIR)"
```

**Fixed:**
```makefile
upload:
	@[ -n "$(PORT)" ] || (echo "FAIL: no board detected. Connect the ESP32 and retry."; exit 1)
	arduino-cli upload -p $(PORT) --fqbn $(BOARD) "$(SKETCH_DIR)" -b 115200 || \
		(echo "Upload failed. Try manual reset: hold GPIO0 to GND, press RESET, then retry."; exit 1)
```

## Test plan
1. Run `make upload` with ESP32 connected - should complete at 115200 baud
2. Verify upload completes without "Chip Stopped Responding" error
3. If upload intentionally fails (board disconnected), verify helpful error message appears

## Risks
- 115200 baud is slower than default 921600 (acceptable tradeoff for reliability)
- Some ESP32 boards may have clock drift issues at lower baud rates (rare with USB serial)

## Open questions
None - user confirmed Option A is acceptable.
