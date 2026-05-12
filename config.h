/*
 * User config file - DO NOT commit this to git. Copy to config.h and customize for your setup.
 */

#ifndef CONFIG_H
#define CONFIG_H

/**
 * Pin Definitions
 * -----------------
 * GPIO pin assignments for hardware components.
 */
#define BUTTON_PIN 0      // Built-in BOOT button on ESP32 dev boards (GPIO0)
#define LED_PIN 2         // Built-in LED on ESP32 dev boards (GPIO2)

/**
 * BLE Configuration
 * -----------------
 * Settings for Bluetooth Low Energy scanning and connection.
 */
#define MAX_SENSOR_SLOTS 4           // Maximum number of sensor slots to support
#define SCAN_DURATION_MS 5000        // Duration to scan for sensors in milliseconds
#define MAX_RECONNECT_DELAY_MS 30000 // Maximum delay between reconnection attempts

/**
 * MAC Address Storage
 * -------------------
 * Array to store discovered sensor MAC addresses.
 * Format: uint8_t mac[6] where mac[0] is the first byte (LSB).
 * Initialize with {0, 0, 0, 0, 0, 0} for empty slots.
 */
static const uint8_t DISCOVERED_MAC_ADDRESSES[MAX_SENSOR_SLOTS][6] = {
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // Slot 0 - default empty
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // Slot 1 - default empty
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // Slot 2 - default empty
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}  // Slot 3 - default empty
};

/**
 * Serial Console Command Mapping
 * -------------------------------
 * Commands accepted via serial console for sensor management.
 */
#define COMMAND_SCAN 'S' // Scan for sensors (also accepts lowercase 's')
#define COMMAND_SELECT_1 '1' // Select sensor 1
#define COMMAND_SELECT_2 '2' // Select sensor 2
#define COMMAND_SELECT_3 '3' // Select sensor 3
#define COMMAND_SELECT_4 '4' // Select sensor 4
#define COMMAND_HELP '?'     // Show help

/**
 * Status LED Modes
 * ----------------
 * LED behavior patterns for different system states.
 */
#define LED_MODE_OFF 0            // No power / disconnected - LED off
#define LED_MODE_SCANNING 1       // Scanning for sensors - slow blink (500ms)
#define LED_MODE_CONNECTING 2     // Connecting to sensor - fast blink (100ms)
#define LED_MODE_CONNECTED 3      // Connected to a sensor - solid on

#endif // CONFIG_H
