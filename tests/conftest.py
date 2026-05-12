# Pytest configuration and fixtures for ESP32 HR Hub tests
import subprocess, time, serial
from serial.tools import list_ports
import pytest

BAUD_RATE = 115200
SERIAL_TIMEOUT = 1.0
BOARD = "esp32:esp32:esp32"
SKETCH_DIR = "/Users/gue/Development/personal/espHrHub"

def find_esp32_port():
    ports = list_ports.comports()
    for port in ports:
        if "usbserial" in port.device:
            return port.device
    return None

SERIAL_PORT = find_esp32_port()

def flash_esp32(port):
    cmd = ["arduino-cli", "upload", "-p", port, "--fqbn", BOARD, SKETCH_DIR, "--upload-property", "upload.speed=115200"]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if result.returncode != 0:
            print(f"Upload stdout: {result.stdout}")
            print(f"Upload stderr: {result.stderr}")
            return False
        return True
    except subprocess.TimeoutExpired:
        print("Upload timeout")
        return False

@pytest.fixture
def serial_connection():
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=SERIAL_TIMEOUT)
    time.sleep(2)
    ser.flushInput()
    yield ser
    ser.close()

@pytest.fixture
def flashed_esp32():
    port = find_esp32_port()
    if not port:
        pytest.fail("No ESP32 board detected. Connect the device and retry.")
    print(f"Flashing ESP32 on port: {port}")
    if flash_esp32(port):
        time.sleep(3)
        return port
    else:
        pytest.fail("Failed to flash ESP32. Check connection and try again.")

@pytest.fixture
def serial_with_timeout():
    def _get_connection(timeout=SERIAL_TIMEOUT):
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=timeout)
        time.sleep(2)
        ser.flushInput()
        return ser
    yield _get_connection
