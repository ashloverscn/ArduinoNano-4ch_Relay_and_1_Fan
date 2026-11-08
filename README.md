# ArduinoNano-4ch_Relay_and_1_Fan
4ch Relay Board with Leading Edge AC Dimmer Using Arduino Nano for 3 Lights and 1 Fan

Libraries Needed

EEPROM.h          Already present

IRremote.h        Available in Arduino Library Manager

AceButton.h       Available in Arduino Library Manager

arduino-timer.h   Available in Arduino Library Manager

atmega328_16mhz_ac_phase_control.h Library : https://github.com/ashloverscn/atmega328-16mhz-ac-phase-control

Related Blog by Tech StudyCell : https://iotcircuithub.com/arduino-iot-project-with-google-assistant-alexa/

pcbs:

https://github.com/ashloverscn/4ch-relay-1fan-arduino-nano-pcb

https://github.com/ashloverscn/4ch-spdt-relay-1fan-arduino-nano-pcb

https://github.com/ashloverscn/4ch-relay-1fan-pcb

https://github.com/ashloverscn/4ch-spdt-relay-1fan-pcb

https://github.com/ashloverscn/dimmer-pcb

https://github.com/ashloverscn/4ch-relay-pcb

https://github.com/ashloverscn/4ch-spdt-relay-pcb

 # ESP32 / ESP8266 Master & ATmega328P Slave Hybrid Home Automation Controller

![ESP32/ESP8266](https://img.shields.io/badge/Master-ESP32%2FESP8266-blue?logo=espressif)
![ATmega328P](https://img.shields.io/badge/Slave-ATmega328P-orange?logo=arduino)
![C++](https://img.shields.io/badge/C%2B%2B-Arduino-green?logo=arduino)
![MQTT](https://img.shields.io/badge/MQTT-TLS%2FSSL-success?logo=mqtt)
![License](https://img.shields.io/badge/License-MIT-yellow.svg)

A professional, feature-rich hybrid home automation firmware suite built around a robust **Master-Slave architecture**. An **ESP32 or ESP8266 microcontroller** acts as the intelligent master node (hosting responsive web dashboards and a secure cloud MQTT bridge), while an **ATmega328P microcontroller (Arduino Nano)** operates as the real-time slave node managing digital relays, an AC phase-cut fan dimmer, physical wall switches, and infrared remote controls.

---

## 🚀 Key Features

### 1. Master-Slave Distributed Architecture
* **Intelligent Offloading:** The ESP master handles high-level connectivity (Wi-Fi, WebSockets/AJAX web servers, TLS-secured MQTT cloud telemetry), while the ATmega328P slave handles time-critical, deterministic low-level operations.
* **Serial UART Bridge:** Bidirectional command and telemetry exchange over serial UART (`115200 baud`) utilizing robust 50ms silence-timeout batch buffering and JSON payload formatting.

### 2. Multi-Channel Relay & AC Phase-Cut Fan Dimmer
* **4-Channel Appliance Switching:** Controls 4 independent relay channels supporting active-LOW relay modules.
* **AC Phase-Cut Fan Dimmer:** Features a 10-stage dimmer (levels 0–9) utilizing a Zero-Crossing Detection (ZCD) interrupt (`INT0`) and hardware timers to trigger a TRIAC gate driver at precise microsecond phase delays.
* **EEPROM State Persistence:** Automatically saves relay states and dimmer values to the ATmega328P's internal non-volatile EEPROM memory so settings survive power cycles.

### 3. Multi-Modal Input & Control Interfaces
* **Embedded Web Dashboard:** Hosts a responsive, modern web UI with real-time state synchronization, live visual feedback, and a built-in serial debugging console.
* **Flexible Wall Switch Support:** Operates with either traditional latching SPDT toggle switches or momentary tactile push-buttons (with built-in software debouncing via custom handlers or `AceButton`).
* **Infrared (IR) Remote Control:** Decodes NEC-protocol infrared signals to toggle individual relays, step fan speeds up or down, or trigger master all-on/all-off actions.

### 4. Cloud Integration & Secure MQTT Telemetry
* **TLS-Secured MQTT Client:** Connects securely over SSL/TLS (`WiFiClientSecure` on port `8883`) to cloud MQTT brokers (e.g., EMQX Cloud).
* **Smart Home Ecosystem Ready:** Seamlessly integrates with automation platforms like **Home Assistant** or **Node-RED** via dedicated command and state topics.

### 5. Robust Configuration & Fallback Mechanism
* **Soft-AP Configuration Portal:** If Wi-Fi connection fails for longer than 10 minutes, or if you long-press the onboard Boot button (`GPIO 0`) for 3 seconds, the ESP master automatically enters **Access Point (AP) Mode**.
* **Web-Based Config Panel:** Connect directly to the AP (`ESP32-Setup-AP` / `ESP8266-Setup-AP`) and navigate to `/config` to update Wi-Fi credentials and MQTT broker parameters without re-flashing.

---

## 📌 Hardware Pin Mapping & Reference

### ATmega328P / Arduino Nano Slave Pinout

| Function / Peripheral | Arduino Pin Name | ATmega328P Physical Pin (DIP-28) | Description / Connection Details |
| :--- | :--- | :--- | :--- |
| **Relay 1** | `Digital 5` | Pin 11 (PD5) | Active-LOW control trigger for Relay 1 |
| **Relay 2** | `Digital 6` | Pin 12 (PD6) | Active-LOW control trigger for Relay 2 |
| **Relay 3** | `Digital 7` | Pin 13 (PD7) | Active-LOW control trigger for Relay 3 |
| **Relay 4** | `Digital 8` | Pin 14 (PB0) | Active-LOW control trigger for Relay 4 |
| **TRIAC Gate** | `Digital 9` | Pin 15 (PB1) | AC Phase Dimmer Optocoupler Trigger (MOC3021 / BT136) |
| **Zero-Cross Detect** | `Digital 2` | Pin 4 (PD2) | AC Zero-Crossing Optocoupler Interrupt (`INT0`) |
| **IR Receiver** | `Digital 3` | Pin 5 (PD3) | TSOP1838 / VS1838B IR Demodulator Input (`INT1`) |
| **Switch 1** | `Analog 0` (`A0`) | Pin 23 (PC0) | Wall Switch 1 (Internal pull-up enabled) |
| **Switch 2** | `Analog 1` (`A1`) | Pin 24 (PC1) | Wall Switch 2 (Internal pull-up enabled) |
| **Switch 3** | `Analog 2` (`A2`) | Pin 25 (PC2) | Wall Switch 3 (Internal pull-up enabled) |
| **Switch 4** | `Analog 3` (`A3`) | Pin 26 (PC3) | Wall Switch 4 (Internal pull-up enabled) |
| **Switch 5 / Fan Dn** | `Analog 4` (`A4`) | Pin 27 (PC4) | Fan Dimmer Decrease / Switch 5 Momentary Input |
| **Switch 6 / Fan Up** | `Analog 5` (`A5`) | Pin 28 (PC5) | Fan Dimmer Increase / Switch 6 Momentary Input |
| **UART RX** | `RX (Digital 0)`| Pin 2 (PD0) | Connected to Master (ESP32/ESP8266) TX pin |
| **UART TX** | `TX (Digital 1)`| Pin 3 (PD1) | Connected to Master (ESP32/ESP8266) RX pin |

---

## 🔌 Serial Command & Protocol Reference

* **Baud Rate:** `115200`
* **Master ➔ Slave Command Syntax:**
  * Relay control: `POWER1 ON`, `POWER1 OFF`, `POWER2 ON`, etc.
  * Dimmer control: `DIMMER 0` through `DIMMER 9`, `DIMM_UP`, `DIMM_DN`
  * Global actions: `ALL_ON`, `ALL_OFF`, `STATUS`
* **Slave ➔ Master Telemetry JSON Packet:**
  ```json
  {"POWER1":"ON", "POWER2":"OFF", "POWER3":"OFF", "POWER4":"OFF", "Dimmer":5}
