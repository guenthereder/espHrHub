# ESP32 HR Hub Test Suite

This directory contains integration tests for the ESP32 HR Hub project.

## Requirements

- Python 3.8+
- pyserial
- pytest

Install requirements:
```bash
pip install pyserial pytest
```

## Running Tests

### Basic Test Run
```bash
make test
```

Or manually:
```bash
pytest tests/ -v
```

### With Verbose Output
```bash
pytest tests/ -v --tb=short
```

### Run Specific Test
```bash
pytest tests/test_hubsanity.py::test_boot_sequence -v
```

## Test Overview

### test_hubsanity.py

**test_boot_sequence**
- Verifies ESP32 starts correctly
- Waits for "=== Setup Complete ===" message
- 15-second timeout
- Runs without requiring an ESP32 to be connected

**test_ble_advertising**
- Confirms BLE advertising starts
- Looking for "ESP32 HR Hub" or "Advertising started" messages
- Requires ESP32 to be connected

**test_scan_functionality**
- Tests scan command (send 'S')
- Verifies "Scan complete" message appears
- Requires ESP32 to be connected

**test_sensor_connection_workflow**
- Tests sensor connection initialization
- Verifies either connection attempts or "no sensors configured" message
- Requires ESP32 to be connected

**test_hr_data_detection**
- Detects HR data in serial output
- Looks for BPM/HR patterns
- Acceptable to find no HR data if no sensor connected
- Requires ESP32 to be connected

**test_led_state_verification**
- Verifies LED state through serial logs
- Looks for LED-related messages
- Requires ESP32 to be connected

**test_reconnection_behavior**
- Tests reconnection monitor is running
- Looks for "Attempting to reconnect" messages
- Requires ESP32 to be connected

## Serial Port Configuration

Default serial port: /dev/tty.usbserial-2030
Baud rate: 115200
Timeout: 1 second

To use a different port, update SERIAL_PORT in conftest.py.

## Makefile Targets

- make test - Run all tests (requires ESP32 for most tests)
- make verify - Compile sketch (no hardware needed)
- make upload - Flash to ESP32

## Continuous Integration

Tests automatically run as part of make verify workflow before deployment.
