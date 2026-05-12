#!/usr/bin/env python3
"""Reset ESP32 and trigger HR strap scan."""

import serial
import time
import sys

def find_esp32_port():
    """Find the ESP32 serial port."""
    import subprocess
    result = subprocess.run(
        ["arduino-cli", "board", "list"],
        capture_output=True, text=True
    )
    for line in result.stdout.split('\n'):
        if 'usbserial' in line.lower() and 'USB' in line:
            return line.split()[0]
    # Fallback to known ports
    import glob
    ports = glob.glob('/dev/cu.usbserial-*') + glob.glob('/dev/tty.usbserial-*')
    return ports[0] if ports else None

def reset_and_scan():
    port = find_esp32_port()
    if not port:
        print("ERROR: No ESP32 found. Connect the board and retry.")
        sys.exit(1)
    
    print(f"ESP32 found on {port}")
    print("Opening serial connection...")
    
    try:
        ser = serial.Serial(port, 115200, timeout=1)
    except Exception as e:
        print(f"ERROR: Cannot open {port}: {e}")
        sys.exit(1)
    
    # Clear buffer
    ser.flushInput()
    
    print("Sending scan command...")
    ser.write(b'S\n')
    time.sleep(0.5)
    
    print("\n=== SCAN RESULTS ===")
    start = time.time()
    found_devices = []
    
    while time.time() - start < 15:
        try:
            line = ser.readline().decode('utf-8', errors='replace').strip()
            if line:
                print(line)
                if 'MAC:' in line:
                    found_devices.append(line)
        except:
            break
    
    ser.close()
    
    if found_devices:
        print(f"\nFound {len(found_devices)} devices.")
        print("\nIf TICKR FIT is listed above, press BOOT button on ESP32")
        print("to auto-connect. The strap must NOT be connected to phone/watch.")
    else:
        print("\nNo devices found. Possible issues:")
        print("- TICKR FIT not in pairing mode (remove battery 30s, reinstall)")
        print("- Strap still connected to phone (use Airplane Mode)")
        print("- Strap too far from ESP32 (>3 meters)")
    
    print("\n=== TIPS ===")
    print("1. If you see 'TICKR FIT', press the BOOT button on ESP32")
    print("2. If connection fails, put phone in Airplane Mode first")
    print("3. Factory reset strap: remove battery 60s, reinstall, scan immediately")

if __name__ == '__main__':
    reset_and_scan()
