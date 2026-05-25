# 8-channel Zigbee Irrigation Controller

Zigbee 8-zone irrigation controller based on the ESP32-C6-Zero, with LCD1602 local interface and Home Assistant integration.

---

## Project introduction

Irrimunno is an open-source 8-zone irrigation controller based on the ESP32-C6-Zero.
It combines a local LCD1602 interface with Zigbee connectivity, allowing full control
both from the physical buttons on the device and from a home automation system such as
Home Assistant. Designed for residential gardens and small orchards, it replaces
commercial "black-box" controllers with a fully transparent, hackable alternative.

## Project function

Irrimunno manages up to 8 solenoid valve zones (EV1–EV8), each with an independently
configurable run timer (0–60 minutes). The device supports two operating modes:

- **Automatic sequence**: zones activate one after the other, each for its configured duration. Zones with a timer set to 0 are automatically skipped.
- **Manual control**: any zone can be toggled ON/OFF individually at any time from the LCD menu or via Zigbee commands from Home Assistant.

The LCD display shows real-time status (active zone, remaining time, Zigbee connection
state) and turns off automatically after 20 seconds of inactivity to save power.
Any button press wakes the display instantly.

## Project Parameters

* **Microcontroller**: Espressif ESP32-C6-Zero (RISC-V, 2.4 GHz Wi-Fi + Zigbee 3.0)
* **Irrigation zones**: 8 independent relay outputs (EV1–EV8)
* **Zone timer range**: 0–60 minutes per zone (0 = skip in sequence)
* **Display**: LCD1602 (16×2 characters) via PCF8574 I2C expander — software bit-bang I2C at ~50 kHz for maximum reliability under FreeRTOS multi-task scheduling
* **User interface**: 3 tactile buttons (LEFT / OK / RIGHT), full menu-driven navigation
* **Connectivity**: Zigbee 3.0 — exposes each zone as an ON/OFF switch and each timer as an Analog Output attribute, fully compatible with Home Assistant ZHA/Z2M
* **Power supply**: 6–24 V external supply (for relay coils and solenoid valves)
* **Display backlight auto-off**: 20 seconds (configurable in firmware)
* **Firmware**: ESP-IDF v5.1.2, FreeRTOS
* **License**: CERN-OHL-P-2.0

## Hardware description

The project is organized into the following blocks:

**Main controller**: The ESP32-C6-Zero runs the FreeRTOS application. A dedicated Zigbee task handles network joining and attribute reporting. A UI task (20 ms tick) reads the three buttons with edge detection and drives the LCD state machine.

**Relay board**: 8 relay channels controlled via GPIO. Each relay switches one irrigation valve. The `relay_seq` module handles automatic sequencing with FreeRTOS timers.

**LCD interface**: The LCD1602 is connected through a PCF8574 GPIO expander on an I2C bus. To work around reliability issues of the ESP32-C6 hardware I2C peripheral under FreeRTOS, a software bit-bang I2C implementation is used, based on `esp_rom_delay_us()` for precise, scheduler-immune timing. The PCF8574 module pull-up resistors are removed and replaced with 3.3 V external pull-ups to keep signal levels within ESP32 tolerances.

**Zigbee integration**: The device joins a Zigbee 3.0 network as an End Device and exposes 8 On/Off clusters (manual relay control) + 8 Analog Output clusters (timer values in minutes). All attributes are readable and writable from Home Assistant.

## Software

Built with ESP-IDF v5.1.2. Main modules:

| File | Description |
|------|-------------|
| `main/main.c` | Zigbee stack, attribute handling, app entry point |
| `main/relay_seq.c` | Relay control + automatic sequence logic |
| `main/ui.c` | Button reading, LCD menu state machine, display sleep |
| `main/lcd.c` | LCD1602 driver (bit-bang I2C via PCF8574) |
| `main/config.h` | All hardware pin assignments and tunable parameters |

## Important notes

* The PCF8574 I2C module must have its onboard pull-up resistors **removed** if the LCD is powered at 5 V and the ESP32 at 3.3 V. Replace them with external 4.7 kΩ resistors to 3.3 V to keep SDA/SCL within safe input voltage range for the ESP32.
* The ESP32-C6 hardware I2C peripheral can produce sporadic errors under FreeRTOS multi-task conditions (ESP-IDF 5.1.x). This firmware uses software bit-bang I2C as a definitive fix — do not replace it with the hardware driver.
* Set relay timer to 0 to skip that zone in the automatic sequence without deleting its configuration.
* Factory reset (Zigbee network leave + NVS erase): hold OK button at power-on for 5 s.

## OSHWLab project page

*(link coming soon)*

## License

[CERN Open Hardware Licence Version 2 - Permissive (CERN-OHL-P-2.0)](https://ohwr.org/cern_ohl_p_v2.txt)
