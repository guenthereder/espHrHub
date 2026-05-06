.PHONY: verify build upload

BOARD        ?= esp32:esp32:esp32
SKETCH_DIR   ?= $(CURDIR)
PORT         ?= $(shell arduino-cli board list 2>/dev/null | awk 'NR>1 && $$1!="" && $$5~/USB/ {print $$1; exit}')

verify: build

build:
	@echo "Compiling for $(BOARD)..."
	arduino-cli compile --fqbn $(BOARD) "$(SKETCH_DIR)"
	@echo "OK: firmware compiles cleanly."

upload:
	@[ -n "$(PORT)" ] || (echo "FAIL: no board detected. Connect the ESP32 and retry."; exit 1)
	arduino-cli upload -p $(PORT) --fqbn $(BOARD) "$(SKETCH_DIR)"
