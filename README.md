# ⚡ Multi-CPU Transformer Control, Telemetry & Command System

**An industrial embedded systems ecosystem for transformer monitoring, automated cooling, real-time power analytics, and distributed diagnostics.**

Designed and developed by **Ubamadu Possible**, this project integrates ESP32-S3 industrial controllers, Ethernet-based telemetry, a secondary thermocouple subsystem, and a Python desktop control center into a distributed monitoring and command infrastructure.

The system is designed to acquire transformer operating parameters, automate dual-stage cooling, record time-series data, stream telemetry to remote hosts, and provide centralized diagnostics and control for multiple transformer units.

---

## 📑 Table of Contents

- [Overview](#-overview)
- [System Architecture](#-system-architecture)
- [Key Features](#-key-features)
- [Hardware Architecture](#-hardware-architecture)
- [Repository Structure](#-repository-structure)
- [Hardware Configuration & Pinout](#-hardware-configuration--pinout)
- [UDP Telemetry Protocol](#-udp-telemetry-protocol)
- [Installation & Setup](#-installation--setup)
- [System Communication](#-system-communication)
- [Safety & Fault Handling](#-safety--fault-handling)
- [Technology Stack](#-technology-stack)
- [Future Development](#-future-development)
- [Author](#-author)

---

## 🚀 Overview

The **Multi-CPU Transformer Control System** combines embedded hardware, industrial sensing, networking, data acquisition, and desktop software to deliver a centralized transformer monitoring and control platform.

The system consists of three major components:

| Component | Responsibility |
|---|---|
| Main ESP32-S3 Controller | Sensor acquisition, power analytics, cooling control, data logging, Ethernet networking, and web dashboard |
| Secondary ESP32 Node | Ambient temperature acquisition using a MAX31855 thermocouple interface and UART communication |
| Python Test Center | Multi-unit telemetry monitoring, UDP server services, packet inspection, serial communication, and remote command management |

The architecture supports monitoring and management of up to **five transformer controller units** through the desktop server infrastructure.

### Core Capabilities

- Multi-sensor transformer telemetry acquisition
- Three-phase AC voltage and RMS current monitoring
- Real, reactive, and apparent power calculations
- Automated two-stage cooling control
- Ethernet-based telemetry and time synchronization
- MicroSD data logging with CSV export
- Embedded HTTP dashboard and log viewer
- Binary UDP communication with CRC32 verification
- Centralized multi-unit diagnostics and command handling

---

## 🏗 System Architecture

```text
                   ┌──────────────────────────────┐
                   │  Secondary ESP32 Node        │
                   │                              │
                   │  MAX31855 Thermocouple       │
                   │  Ambient Temperature         │
                   │  UART Packet Transmission    │
                   └──────────────┬───────────────┘
                                  │
                             UART │
                                  ▼
┌───────────────────────────────────────────────────────────────┐
│                  MAIN TRANSFORMER CONTROLLER                  │
│                         ESP32-S3                               │
│                                                               │
│  ┌──────────────────┐       ┌─────────────────────────────┐  │
│  │ Temperature      │       │ Pressure & Electrical       │  │
│  │ Acquisition      │       │ Measurement                  │  │
│  │ MAX31855         │       │ ADC / ADS1115 / CT / Voltage │  │
│  └──────────────────┘       └─────────────────────────────┘  │
│                                                               │
│  ┌──────────────────┐       ┌─────────────────────────────┐  │
│  │ Dual-Stage       │       │ RTC, EEPROM & MicroSD       │  │
│  │ Cooling Control  │       │ Persistent Data Logging     │  │
│  └──────────────────┘       └─────────────────────────────┘  │
│                                                               │
│  ┌─────────────────────────────────────────────────────────┐  │
│  │ W5500 Ethernet · HTTP Dashboard · UDP Telemetry         │  │
│  └─────────────────────────────────────────────────────────┘  │
└──────────────────────────────┬────────────────────────────────┘
                               │
                 ┌─────────────┴─────────────┐
                 │                           │
            UDP Telemetry               HTTP / Web
            Port 5000                    Port 80
                 │                           │
                 ▼                           ▼
┌───────────────────────────────────────────────────────────────┐
│              KODION ENERGY — PYTHON TEST CENTER                │
│                                                               │
│  ┌──────────────────────┐  ┌──────────────────────────────┐  │
│  │ UDP Telemetry Server │  │ Time Synchronization Server  │  │
│  │ Binary Packet Decode │  │ UDP Port 5007                │  │
│  └──────────────────────┘  └──────────────────────────────┘  │
│                                                               │
│  ┌──────────────────────┐  ┌──────────────────────────────┐  │
│  │ Packet Sniffer       │  │ Desktop Control Interface    │  │
│  │ HEX / ASCII / Binary │  │ Serial / Commands / Cluster  │  │
│  └──────────────────────┘  └──────────────────────────────┘  │
└───────────────────────────────────────────────────────────────┘
```

### Communication Interfaces

| Interface | Purpose |
|---|---|
| SPI | Thermocouple interfaces, MicroSD, and W5500 Ethernet |
| I²C | ADS1115 ADC and DS3231 RTC |
| UART | Communication between the primary and secondary ESP32 controllers |
| UDP | Binary telemetry streaming, time synchronization, and command communication |
| HTTP | Embedded dashboard, configuration, and log viewing |

---

## 🌟 Key Features

### 1. Main ESP32-S3 Transformer Controller

#### 🌡️ Multi-Sensor Telemetry

The controller acquires operating data from multiple sensor categories.

**Temperature monitoring**
- Transformer oil temperature
- Winding temperature
- Tank surface temperature
- Ambient temperature from the secondary ESP32 node
- Stage A and Stage B discharge temperatures, where configured

**Pressure monitoring**
- Dual discharge pressure channels
- Built-in ESP32 ADC and external ADS1115 I²C ADC
- Intended measurement range: 0–100 PSI

**Electrical monitoring**
- Three-phase AC voltage sensing
- Three-phase RMS current measurement
- Current transformer input configuration: 3000 A : 5 A
- Real power (kW)
- Reactive power (kVAR)
- Apparent power (kVA)
- Power factor

> **Measurement note:** Accurate electrical measurements depend on appropriate analog signal conditioning, ADC calibration, sampling methodology, phase alignment, and sensor scaling.

#### ❄️ Automated Dual-Stage Cooling

The controller implements temperature-driven cooling control using two independent stages.

- Stage A and Stage B relay outputs
- Hysteresis-based switching logic
- Configurable temperature thresholds and timing parameters
- Sensor-fault and over-temperature fallback logic
- Manual relay overrides through the control interface, where enabled

The intended control strategy is to prevent excessive switching while maintaining transformer cooling under changing operating conditions.

#### 🌐 Ethernet & Embedded Web Interface

The primary controller uses a W5500 Ethernet interface with a static IP configuration.

The embedded HTTP dashboard provides functionality for:

- Real-time telemetry visualization
- Cooling relay status and manual overrides
- Timer configuration
- Firmware update functionality in Short Mode
- CSV log browsing and viewing in Full Mode

Actual availability depends on the firmware configuration and enabled features.

#### 💾 Data Logging & Timekeeping

- MicroSD-based telemetry logging
- CSV-format data records
- Configurable sampling intervals from 1 second to 24 hours
- Persistent configuration storage using EEPROM
- DS3231 real-time clock integration
- UDP-based time synchronization with server failover support

#### 📡 Binary UDP Telemetry

The controller streams a fixed-size, 64-byte binary telemetry packet to a remote server at a nominal rate of **2 Hz**.

The packet includes a device identifier, CRC32 checksum, sensor readings, electrical measurements, and power metrics.

---

### 2. Secondary ESP32 Temperature Subsystem

The secondary ESP32 acts as a dedicated temperature acquisition node.

**Responsibilities**

- Read thermocouple data through a MAX31855 SPI interface.
- Acquire ambient temperature measurements.
- Package temperature readings into a framed UART message.
- Transmit data to the primary controller using hardware UART.
- Support error checking and non-blocking communication.

This modular architecture separates selected sensing tasks from the main controller, improving subsystem organization and simplifying diagnostics.

---

### 3. Python Multi-CPU Test Center

The Python desktop application serves as the centralized diagnostic and communications environment.

#### 🖥️ Desktop GUI

Built with Python and Tkinter, the application combines several engineering tools into one interface.

**Cluster monitoring**
- Monitor and manage up to five transformer controller units.
- Receive and decode telemetry from multiple network nodes.
- Display incoming data for diagnostics.

**UDP server services**
- Port 5000: receive and decode binary telemetry packets.
- Port 5007: handle hardware time-synchronization requests using the `0xAA 0x01` request signature.

**Packet inspector**
- Raw packet inspection
- Hexadecimal representation
- ASCII decoding
- Decimal representation
- Binary representation
- Base64 representation

**Serial terminal**
- Live serial communication
- Hardware debugging
- UART traffic inspection

**Command and control**
- Remote reboot requests
- Manual cooling relay overrides
- Compressor timer updates
- Power-factor-related configuration or adjustment commands, where supported by the firmware

> Remote command execution depends on the implemented command protocol, network connectivity, and the permissions and validation enforced by the receiving controller.

---

## 🔌 Hardware Architecture

### Main Controller

The main controller is built around an ESP32-S3 and interfaces with the following subsystems:

| Subsystem | Hardware / Interface |
|---|---|
| Main processor | ESP32-S3 |
| Ethernet | W5500 Ethernet controller |
| Local storage | MicroSD card |
| Temperature sensing | MAX31855 thermocouple interfaces |
| Pressure acquisition | Internal ADC and ADS1115 |
| Timekeeping | DS3231 RTC |
| Electrical measurement | Voltage sensing and current transformers |
| Inter-controller link | Hardware UART |
| Cooling control | Stage A and Stage B relay drivers |

### Secondary Controller

| Subsystem | Hardware / Interface |
|---|---|
| Processor | ESP32 |
| Thermocouple interface | MAX31855 |
| Main controller link | Hardware UART |

---

## 📂 Repository Structure

```text
.
├── traxcode_18.ino
│   └── Main ESP32-S3 transformer controller firmware
│       ├── Sensor acquisition
│       ├── Ethernet and HTTP dashboard
│       ├── Cooling control
│       ├── Power calculations
│       ├── MicroSD logging
│       └── UDP telemetry
│
├── cpu2_extension.ino
│   └── Secondary ESP32 temperature subsystem
│       ├── MAX31855 interface
│       └── UART data transmission
│
├── main.py
│   └── Python desktop Test Center
│       ├── Tkinter GUI
│       ├── UDP telemetry server
│       ├── Time synchronization service
│       ├── Packet inspector
│       └── Serial terminal
│
└── README.md
    └── Project documentation
```

---

## 📍 Hardware Configuration & Pinout

### Main ESP32-S3 Controller

The following table documents the current firmware pin assignments.

| Peripheral | Signal | ESP32-S3 GPIO |
|---|---|---:|
| W5500 Ethernet | SCK | 12 |
| W5500 Ethernet | MISO | 13 |
| W5500 Ethernet | MOSI | 11 |
| W5500 Ethernet | CS | 21 |
| MicroSD | CS | 48 |
| MicroSD | Card Detect | 47 |
| Tank Thermocouple | CS | 3 |
| Winding Thermocouple | CS | 10 |
| Oil Thermocouple | CS | 9 |
| I²C | SDA | 35 |
| I²C | SCL | 36 |
| AC Voltage | Phase A | 6 |
| AC Voltage | Phase B | 7 |
| AC Voltage | Phase C | 8 |
| AC Current | Phase 1 | 2 |
| AC Current | Phase 2 | 5 |
| AC Current | Phase 3 | 4 |
| UART Link | RX | 42 |
| UART Link | TX | 41 |
| Cooling Relay | Stage A | 39 |
| Cooling Relay | Stage B | 40 |

**Important:** Verify the pin assignments against the actual PCB schematic, ESP32-S3 module variant, and complete firmware before manufacturing or deployment. GPIO availability and peripheral conflicts must be checked for the selected board.

---

## 📊 UDP Telemetry Protocol

### Packet Overview

The primary controller transmits a fixed-length **64-byte telemetry frame** over UDP port `5000`.

The frame contains a device header, a CRC32 checksum, and scaled sensor and electrical measurement values.

| Byte Offset | Field | Data Type | Scale |
|---|---|---|---|
| `0x00–0x03` | Device Header | `uint32_t` | Example: `0xA0A0A0A0` |
| `0x04–0x07` | CRC32 Checksum | `uint32_t` | Protocol-defined |
| `0x08–0x09` | Oil Temperature | `int16_t` | ×100 |
| `0x0A–0x0B` | Winding Temperature | `int16_t` | ×100 |
| `0x0C–0x0D` | Tank Surface Temperature | `int16_t` | ×100 |
| `0x0E–0x0F` | Ambient Temperature | `int16_t` | ×100 |
| `0x10–0x13` | Stage A/B Discharge Temperatures | `2 × int16_t` | ×100 |
| `0x14–0x17` | Stage A/B Discharge Pressures | `2 × int16_t` | ×100 |
| `0x18–0x1D` | Three-Phase AC Voltages | `3 × int16_t` | ×100 |
| `0x1E–0x23` | Three-Phase RMS Currents | `3 × int16_t` | ×100 |
| `0x24–0x29` | Real, Reactive, Apparent Power | `3 × int16_t` | ×100 |
| `0x2A–0x2B` | Power Factor | `int16_t` | ×100 |
| `0x2C–0x3F` | Reserved / Additional Fields | 20 bytes | Firmware-defined |

### Decoding Scaled Values

For a measurement encoded as an `int16_t` with a scale factor of 100:

\[
\text{Engineering Value} = \frac{\text{Raw Integer}}{100}
\]

For example, a raw temperature value of `2534` represents `25.34` in the corresponding engineering unit.

### Sensor Fault Representation

The firmware uses the following sentinel for disconnected or faulty sensor readings:

```text
Raw value: -32768
Hex value: 0x8000
```

Receivers should interpret this value as a fault indicator rather than a valid measurement.

### Protocol Implementation Notes

For reliable interoperability, the transmitter and receiver must agree on:

- Byte order (endianness)
- Signed integer representation
- CRC32 algorithm and covered byte range
- Whether the checksum field is zeroed or excluded during CRC calculation
- Reserved-byte behavior
- Units and scaling for each measurement
- Device identification and packet validation rules

The offsets above describe the documented fields. The final wire format should be verified against the actual firmware serializer and Python decoder.

---

## 🛠 Installation & Setup

### 1. ESP32 Firmware

#### Requirements

- Arduino IDE or PlatformIO
- ESP32 board support package
- USB programming connection
- Configured W5500 Ethernet hardware
- Required sensor and storage peripherals

#### Dependencies

Install the libraries required by the firmware, including:

- `Adafruit ADS1X15`
- `RTClib`
- `ETH` and `AsyncUDP` support appropriate to the selected ESP32 framework
- Any additional libraries included by the source code

Library names and installation methods may vary with the Arduino core version.

#### Flashing Procedure

1. Open `cpu2_extension.ino` in the Arduino IDE or import it into your configured PlatformIO project.
2. Select the appropriate ESP32 board and serial port.
3. Compile and flash the secondary ESP32 firmware.
4. Open `traxcode_18.ino`.
5. Configure the network parameters and any hardware-specific settings.
6. Compile and flash the main ESP32-S3 controller.
7. Verify sensor communication, UART exchange, Ethernet connectivity, and relay outputs.

### 2. Python Desktop Test Center

#### Requirements

- Python 3.9 or later
- Desktop environment with Tkinter support
- Network access to the transformer controller
- Serial access when hardware debugging is required

#### Install Dependencies

```bash
pip install pyserial scapy
```

If the application imports additional third-party packages, install those dependencies as well.

#### Launch the Application

From the repository directory:

```bash
python main.py
```

The desktop application should start with its GUI and the server services implemented in the current source.

### 3. Network Configuration

Before testing, verify:

- The controller and desktop computer are on compatible networks.
- The configured static IP address is unique.
- UDP ports `5000` and `5007` are permitted by the host firewall.
- TCP port `80` is reachable when the HTTP dashboard is enabled.
- The Python application is bound to the correct network interface.
- The telemetry receiver and controller agree on the binary protocol.

---

## 🔄 System Communication

The system follows a distributed communication workflow:

1. **Sensor acquisition:** The main controller reads temperature, pressure, and electrical inputs.
2. **Secondary data transfer:** The secondary ESP32 sends its temperature reading to the main controller over UART.
3. **Control processing:** The main controller evaluates the measurements and cooling-control conditions.
4. **Local persistence:** Telemetry records and configuration data are stored using MicroSD and non-volatile memory, respectively.
5. **Network transmission:** The controller transmits binary telemetry packets over UDP.
6. **Centralized monitoring:** The Python application receives and decodes packets from connected controller units.
7. **Diagnostics and commands:** The desktop tools support packet inspection, serial debugging, and configured remote commands.
8. **Time synchronization:** The controller exchanges time-sync messages with the server using the implemented UDP protocol.

---

## 🛡 Safety & Fault Handling

Industrial transformer monitoring involves potentially hazardous electrical equipment. The design incorporates software-level fault-handling concepts, but the complete installation must also include appropriate hardware protection and independent safety measures.

Key considerations include:

- Invalid and disconnected sensor detection
- Temperature threshold monitoring
- Hysteresis-based cooling control
- Safe-state behavior during sensor failures
- Network loss and stale-data detection
- CRC32 packet validation
- Watchdog and recovery strategies, where implemented
- Proper isolation and protection for voltage and current sensing
- Relay-driver protection and correctly rated switching hardware
- Independent over-temperature protection where required by the application

**Safety disclaimer:** This project documentation does not establish compliance with transformer protection standards or certify the system for unattended industrial operation. Validate the control logic, electrical isolation, fault responses, and switching hardware before connecting the system to operational equipment. Network commands should not replace independent safety interlocks.

---

## 🧰 Technology Stack

| Category | Technologies |
|---|---|
| Embedded processors | ESP32-S3, ESP32 |
| Firmware | Arduino C/C++ |
| Industrial networking | W5500 Ethernet, UDP, HTTP |
| Sensor interfaces | SPI, I²C, ADC, UART |
| Temperature acquisition | MAX31855 |
| Pressure acquisition | ADS1115, internal ADC |
| Timekeeping | DS3231 RTC |
| Local storage | MicroSD, EEPROM |
| Desktop application | Python, Tkinter |
| Serial communication | PySerial |
| Packet analysis | Scapy and custom protocol decoding |
| Telemetry format | Binary frames, CRC32, CSV logs |

---

## 🔭 Future Development

Potential extensions to the platform include:

- Historical telemetry dashboards and trend analysis
- Long-term data storage and automated reporting
- Alarm notifications and event-driven fault reporting
- Improved multi-controller device discovery
- Secure authentication and command authorization
- OTA firmware update management
- Protocol versioning and backward compatibility
- Enhanced packet-loss and network-health diagnostics
- Automated hardware-in-the-loop testing
- Integration with industrial SCADA or IoT platforms

These are possible development directions and are not necessarily implemented in the current source.

---

## 👨‍💻 Author

**Ubamadu Possible Sopuruchi**

Embedded Systems Engineer | PCB Design Engineer | Industrial Control & Telemetry Systems

This project demonstrates the integration of custom embedded hardware, firmware architecture, sensor acquisition, industrial communications, power analytics, data logging, and desktop diagnostic software.

**Focus areas**
- PCB design and embedded hardware development
- ESP32-based industrial controllers
- Sensor interfaces and data acquisition
- Ethernet-connected embedded systems
- Hardware diagnostics and test automation
- Industrial monitoring and control

---

## 📜 License

A license has not been specified in this documentation. Add a `LICENSE` file before distributing or permitting reuse of the source code.

---

*Built to bring embedded hardware, industrial telemetry, and intelligent control together in one distributed system.* ⚡
