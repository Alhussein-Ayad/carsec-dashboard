The interface deployment link: https://alhussein-ayad.github.io/carsec-dashboard/

To setup the System follow this steps:
Using Arduino IDE
1. Download these libraries:
STM32FreeRTOS — runs two concurrent tasks (main system logic + SMS sending) without blocking each other.
MFRC522 — communicates with the RC522 RFID reader over SPI to read and verify the authorized card UID.
hd44780 — drives the 16x2 I2C LCD display to show system state, GPS info, and alerts (FreeRTOS-safe).
TinyGPS++ — parses NMEA sentences from the GPS module UART stream to extract latitude and longitude.
ArduinoJson — serializes system status data (state, GPS, door, engine) into JSON for the ESP32 web API responses.

2. Download these board managers:
STM32 MCU based boards by STMicroelectronics — for uploading code to the STM32F411 BlackPill.
esp32 by Espressif Systems — for uploading code to the ESP32 38-pin WiFi dashboard module.
