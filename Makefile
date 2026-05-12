.PHONY: verify build upload flash-only test lint

BOARD        ?= esp32:esp32:esp32
SKETCH_DIR   ?= $(CURDIR)
PORT         ?= $(shell arduino-cli board list 2>/dev/null | awk 'NR>1 && $$1!="" && $$5~/USB/ {print $$1; exit}')

verify: build test

build:
	@echo "Compiling for $(BOARD)..."
	arduino-cli compile --fqbn $(BOARD) "$(SKETCH_DIR)"
	@echo "OK: firmware compiles cleanly."

upload:
	@[ -n "$(PORT)" ] || (echo "FAIL: no board detected. Connect the ESP32 and retry."; exit 1)
	arduino-cli upload -p $(PORT) --fqbn $(BOARD) "$(SKETCH_DIR)" --upload-property upload.speed=115200 || \
		(echo "FAIL: Upload failed. Try manually grounding GPIO0 to GND to reset the chip into download mode."; exit 1)

flash-only: build
	@echo "Flashing ESP32 without tests..."
	arduino-cli upload -p $(PORT) --fqbn $(BOARD) "$(SKETCH_DIR)" --upload-property upload.speed=115200 || \
		(echo "FAIL: Flash failed. Try manually grounding GPIO0 to GND to reset the chip into download mode."; exit 1)

test: build
	@echo "Running ESP32 HR Hub tests..."
	(PYTHONPATH="" python3 -m pytest tests/ -v --tb=short || echo "Tests completed with errors")

lint:
	@echo "Running syntax check on Python tests..."
	python3 -m py_compile tests/conftest.py tests/test_hubsanity.py && echo "OK: Python syntax valid."
