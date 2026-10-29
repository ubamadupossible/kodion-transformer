/*
  Transformer Control & Monitoring System (With Configurable SD Data Logging Rate & Auto Time Sync Protocol)
  Features: Dual-Stage Cooling Logic, Dual Pressure Sensing, SPI Thermocouples, 
            UART Ambient & Discharge Temps Feed, 3-Phase AC Voltage/Current Monitoring,
            DS3231 Real-Time Clock, W5500 Ethernet, Web Interface (Short/Full Modes), 
            Power Calculations (Real, Apparent, Reactive), EEPROM Persistence & SD Card Logging,
            In-Browser SD Card Log Viewer, UDP Data Streaming to FPGA/PC, 
            and Automatic/Manual Time Sync Protocol ( Default ON with guaranteed single re-trigger on ON when connected to Server/FPGA -).
*/

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <RTClib.h>
#include <ETH.h>
#include <Network.h>
#include <WebServer.h>
#include <Update.h>
#include <SD.h>
#include <AsyncUDP.h>
#include <EEPROM.h>
#include <cmath>

// ==========================================
// EEPROM MEMORY MAP
// ==========================================
#define EEPROM_SIZE 64
#define EEPROM_ADDR_PF 0           // float (4 bytes)
#define EEPROM_ADDR_ONTIME 4       // uint16_t (2 bytes)
#define EEPROM_ADDR_OFFTIME 6      // uint16_t (2 bytes)
#define EEPROM_ADDR_SD_INTERVAL 8  // uint32_t (4 bytes)
#define EEPROM_MAGIC_ADDR 12       // uint16_t flag to check first boot
#define EEPROM_ADDR_AUTO_TIME 14   // uint8_t (1 byte - Auto Time Sync Mode)
#define EEPROM_MAGIC_VAL 0xABCD

// ==========================================
// HARDWARE PIN DEFINITIONS
// ==========================================
#define SD_CS 48      // SD Card Chip Select
#define SD_DETECT 47  // SD Card Detect Pin

#define PIN_T_TANK 3   // CS Pin 3: Tank surface temp
#define PIN_T_WIND 10  // CS Pin 10: Winding temp
#define PIN_T_OIL 9    // CS Pin 9: Oil temp

#define pressure_1 1          // Internal ADC Stage A Discharge Pressure Sensor Pin
#define I2C_SDA_PIN 35        // ESP32 SDA Pin
#define I2C_SCL_PIN 36        // ESP32 SCL Pin
#define ADS1115_ADDRESS 0x49  // ADS1115 I2C address for Stage B Discharge Pressure Sensor
#define DS3231_ADDRESS 0x68   // DS3231 RTC I2C address

#define PIN_PHASE_A 6  // AC VOLTAGE SENSOR PHASE 1 - PIN 6
#define PIN_PHASE_B 7  // AC VOLTAGE SENSOR PHASE 2 - PIN 7
#define PIN_PHASE_C 8  // AC VOLTAGE SENSOR PHASE 3 - PIN 8

#define PIN_CUR1 2  // CURRENT SENSOR PHASE 1 - PIN 2
#define PIN_CUR2 5  // CURRENT SENSOR PHASE 2 - PIN 5
#define PIN_CUR3 4  // CURRENT SENSOR PHASE 3 - PIN 4

#define MIN_VOLTAGE 0.45f
#define MAX_VOLTAGE 2.97f
#define MAX_PRESSURE 100.0f
#define DISCONNECT_THRESH 0.20f
#define DISCONNECT_THRESH_P2 0.45f
#define AC_DISCONNECT_THRESH 20.0f
#define CURRENT_DISCONNECT_THRESH 0.50f

#define ETH_CS 21
#define ETH_SCK 12
#define ETH_MISO 13
#define ETH_MOSI 11
#define ETH_INT -1
#define ETH_RST -1
#define ETH_PHY_TYPE ETH_PHY_W5500

#define PIN_UART_RX 42  // Receive Pin  <- Sender ESP32 TX (Pin 42)
#define PIN_UART_TX 41  // Transmit Pin -> Sender ESP32 RX (Pin 41)

#define PIN_STAGE_A 39  // Relay Output Stage A is PIN 39
#define PIN_STAGE_B 40  // Relay Output Stage B is PIN 40

#define SENSOR_DISCONNECTED -999.0f
#define PACKET_SENSOR_ERROR -32768

bool overideCoolingLogic = false;
bool Stage_A_overide = false;
bool Stage_B_overide = false;

// ==========================================
// VOLTAGE SENSOR CONSTANTS
// ==========================================
constexpr float AC_DIVIDER_RATIO = (10.0f + 5.0f) / 5.0f;
constexpr float AC_SENSOR_RATIO = 500.0f / 10.0f;
constexpr float AC_TOTAL_MULTIPLIER = AC_DIVIDER_RATIO * AC_SENSOR_RATIO;
constexpr int AC_OVERSAMPLES = 16;

// ==========================================
// CURRENT SENSOR CONSTANTS
// ==========================================
constexpr float ADC_VREF = 3.3f;
constexpr float ADC_RESOLUTION = 4095.0f;
constexpr float BURDEN_RESISTOR = 0.2f;
constexpr float CT_RATIO = 3000.0f / 5.0f;
constexpr float AMPS_PER_VOLT = CT_RATIO / BURDEN_RESISTOR;
constexpr uint16_t CT_SAMPLES = 200;
constexpr uint32_t CT_SAMPLE_INT_US = 200;

const float T_OIL_START = 60.0f;
const float T_WIND_START = 90.0f;
const float T_TANK_START = 55.0f;

const float T_OIL_HI = 70.0f;
const float T_WIND_HI = 100.0f;

const float T_OIL_STOP = 55.0f;
const float T_WIND_STOP = 85.0f;
const float T_TANK_STOP = 50.0f;
const float HYS = 3.0f;

// Global Dynamic Variables (EEPROM Backed)
float powerFactor = 0.85f;
uint16_t onTime = 15;             // Time for relay to remain ON (minutes)
uint16_t offTime = 10;            // Time for relay to remain OFF (minutes)
uint32_t sdLogInterval = 1;       // SD Logging interval in SECONDS (Default: 1s)
bool autoTimeSyncEnabled = true;  // Time Sync from Server/FPGA Enabled ON by default

// Time Sync Protocol Variables
unsigned long lastTimeRequestTime = 0;
bool timeSyncAchieved = false;
DateTime pendingSyncTime(2026, 1, 1, 0, 0, 0);
bool pendingSyncFlag = false;
IPAddress timeServer1(192, 168, 1, 10);
IPAddress timeServer2(192, 168, 1, 20);
const uint16_t TIME_SERVER_PORT = 5007;

unsigned long T_RUN_MIN = onTime * 60 * 1000UL;
unsigned long T_OFF_MIN = offTime * 60 * 1000UL;
const unsigned long UART_TIMEOUT = 5000UL;

IPAddress local_IP(192, 168, 1, 101);  //-----------------------------> CHANGE IP FOR A NEW TRANX
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(8, 8, 8, 8);
#define CPU_NAME "Kodion Transformer 1"  //-----------------------------> CHANGE TITLE

// Stream to FPGA Parameters
const uint32_t UNIQ_DEVICE_HEADER = 0xA0A0A0A0;  //-----------------------------> CHANGE UNIQ DEVICE HEADER FOR NEW TRANSX

const uint8_t packetSize = 64;
uint8_t packet[packetSize];

unsigned long streamTimer = 0;
AsyncUDP udp;
IPAddress FPGA_IP(192, 168, 1, 10);
const uint16_t FPGA_PORT = 5000;

IPAddress SERVER_IP(192, 168, 1, 20);
const uint16_t SERVER_PORT = 5006;

// ==========================================
// DATA STRUCTURES & GLOBAL INSTANCES
// ==========================================
struct PhaseVoltages {
  float phaseA, phaseB, phaseC;
  bool faultA, faultB, faultC;
};

struct PhaseCurrents {
  float cur1_rms, cur2_rms, cur3_rms;
  bool faultCur1, faultCur2, faultCur3;
};

struct PowerTelemetry {
  float realPower;      // kW
  float reactivePower;  // kVAR
  float apparentPower;  // kVA
};

enum SystemState {
  STATE_OFF,
  STATE_STAGE_A,
  STATE_STAGE_AB,
  STATE_SAFE_MODE
};

const char *stateNames[] = {
  "STAGE_A+B RELAY NOT ACTIVE",
  "STAGE_A RELAY ACTIVE",
  "STAGE_A+B RELAY ACTIVE",
  "SAFE_MODE"
};

// Peripheral Instances
Adafruit_ADS1115 pressure_2;
RTC_DS3231 rtc;
HardwareSerial SerialSender(2);
WebServer server(80);

// Global State Flags & Variables
SystemState currentState = STATE_OFF;
unsigned long lastStateChangeTime = 0;
bool ignoreTimer = false;
bool rtcPresent = false;
bool adsPresent = false;
bool sdMounted = false;
bool serverStarted = false;

// Telemetry Storage
float tempOil = 0, tempWinding = 0, tempTank = 0, t_amb = 0, t_stageA = 0, t_stageB = 0;
float p_stageA = 0, p_stageB = 0;
PhaseVoltages acVoltages = { 0.0f, 0.0f, 0.0f, true, true, true };
PhaseCurrents acCurrents = { 0.0f, 0.0f, 0.0f, true, true, true };
PowerTelemetry powerData = { 0.0f, 0.0f, 0.0f };

// Sensor Fault Flags
bool faultOil = false, faultWind = false, faultTank = false;
bool faultAmb = true, faultStageA = true, faultStageB = true;
bool faultPressStageA = true, faultPressStageB = false;

// UART Buffer Settings
const size_t BUFFER_SIZE = 64;
char rxBuffer[BUFFER_SIZE];
bool newDataReceived = false;
unsigned long lastUartPacketTime = 0;

// Function Declarations
void readUARTData();
void parseData(char *data);
float readTemperature(int temPin, bool &faultFlag);
float readPressure_1(uint8_t pin);
float readPressure_2(uint8_t channel);
PhaseVoltages readThreePhaseVoltage();
float measureCurrentRMS(uint8_t pin, bool &faultFlag);
PhaseCurrents readThreePhaseCurrent();
PowerTelemetry calculatePower(const PhaseVoltages &v, const PhaseCurrents &c, float pf);
void updateSensors();
void coolingLogic();
void applyOutputs(SystemState state);
void logTelemetry(unsigned long now);
String getFormattedDate();
String getFormattedTime();
void setupWebServer();
void initSDCard();
void appendFile(const char *path, const String &message);
void buildPacket(uint32_t DEVICE_HEADER, float t1, float t2, float t3, float t4, float stA, float stB, float p1, float p2, float v1, float v2, float v3, float c1, float c2, float c3, float aP, float rP, float recP, float pf);
void stream_to_Ethernet();
void coolingLogicControl();
void loadEEPROMSettings();
void requestNetworkTime();

// Helper inline function for Celsius to Fahrenheit conversion
inline float cToF(float c) {
  return (c * 1.8f) + 32.0f;
}

// ==========================================
// HTML DASHBOARD RESOURCE (SHORT & FULL MODES)
// ==========================================
const char *indexPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>)rawliteral" CPU_NAME R"rawliteral(</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; background: #121212; color: white; text-align: center; padding: 15px; margin: 0; }
        .top-bar { display: flex; justify-content: space-between; align-items: center; background: #1e1e1e; padding: 10px 20px; border-radius: 8px; border: 1px solid #333; margin-bottom: 10px; font-size: 14px; flex-wrap: wrap; gap: 10px; }
        .page-title { color: #f39c12; font-size: 22px; font-weight: bold; margin-bottom: 15px; text-transform: uppercase; letter-spacing: 1px; }
        .nav-btn { background: #333; color: white; border: 1px solid #555; padding: 6px 12px; cursor: pointer; border-radius: 4px; font-weight: bold; margin-left: 5px; }
        .nav-btn.active { background: #f39c12; color: black; border-color: #f39c12; }
        .btn-reset { background: #e74c3c; color: white; border: 1px solid #c0392b; padding: 10px 15px; cursor: pointer; border-radius: 5px; font-weight: bold; width: 100%; margin-top: 10px; }
        .btn-reset:hover { background: #c0392b; }
        .sd-indicator { display: inline-block; width: 12px; height: 12px; border-radius: 50%; background: #e74c3c; margin-left: 5px; vertical-align: middle; }
        .sd-active { background: #2ecc71 !important; }
        .container { display: flex; flex-wrap: wrap; justify-content: center; gap: 15px; }
        .card { background: #1e1e1e; padding: 20px; border-radius: 10px; border: 1px solid #333; width: 360px; text-align: left; box-sizing: border-box; }
        h3 { color: #f39c12; text-align: center; margin-top: 0; border-bottom: 1px solid #333; padding-bottom: 8px; font-size: 16px; }
        .metric { margin: 8px 0; font-size: 13px; border-bottom: 1px dashed #2a2a2a; padding-bottom: 6px; display: flex; justify-content: space-between; align-items: center; }
        .val { font-weight: bold; color: #2ecc71; }
        .val-red { color: #e74c3c !important; }
        .val-green { color: #2ecc71 !important; }
        input[type=datetime-local], input[type=file], input[type=number], input[type=text], select { margin: 6px 0; background: #333; color: white; padding: 6px; border-radius: 5px; width: 95%; border: 1px solid #555; box-sizing: border-box; }
        input[type=submit], input[type=button], button { background: #f39c12; color: black; border: none; padding: 8px 12px; font-weight: bold; cursor: pointer; border-radius: 5px; width: 100%; margin-top: 8px; }
        input[type=submit]:hover, input[type=button]:hover, button:hover { background: #e67e22; }
        .btn-action { background: #3498db; color: white; }
        .btn-action:hover { background: #2980b9; }
        .btn-active-on { background: #2ecc71 !important; color: black !important; }
        .btn-active-off { background: #e74c3c !important; color: white !important; }
        .btn-group { display: flex; gap: 5px; }
        .btn-view { background: #3498db; color: white; padding: 4px 8px; border-radius: 4px; text-decoration: none; font-weight: bold; font-size: 11px; }
        .btn-view:hover { background: #2980b9; }
        .btn-dl { background: #2ecc71; color: black; padding: 4px 8px; border-radius: 4px; text-decoration: none; font-weight: bold; font-size: 11px; }
        .btn-dl:hover { background: #27ae60; }
        .progress-wrapper { width: 100%; background-color: #333; border-radius: 5px; margin-top: 10px; display: none; }
        .progress-bar { width: 0%; height: 18px; background-color: #2ecc71; text-align: center; line-height: 18px; color: black; font-weight: bold; border-radius: 5px; }
        #status { margin-top: 8px; color: #f39c12; font-weight: bold; text-align: center; font-size: 12px; }
        .hidden { display: none !important; }
        .manual-controls { margin-top: 10px; border-top: 1px dashed #444; padding-top: 10px; }
        .form-status { margin-top: 8px; font-weight: bold; font-size: 12px; text-align: center; min-height: 16px; }
        .status-success { color: #2ecc71; }
        .status-error { color: #e74c3c; }
        .switch-container { display: flex; align-items: center; justify-content: space-between; margin: 10px 0; font-size: 13px; color: #ccc; }
        .switch { position: relative; display: inline-block; width: 44px; height: 22px; }
        .switch input { opacity: 0; width: 0; height: 0; }
        .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: #333; transition: .4s; border-radius: 22px; border: 1px solid #555; }
        .slider:before { position: absolute; content: ""; height: 16px; width: 16px; left: 3px; bottom: 2px; background-color: white; transition: .4s; border-radius: 50%; }
        input:checked + .slider { background-color: #2ecc71; }
        input:checked + .slider:before { transform: translateX(21px); }
    </style>
</head>
<body>
    <div class="top-bar">
        <div>
            Time: <span id="rtc_time" style="color:#2ecc71;">--:--:--</span> | 
            Date: <span id="rtc_date" style="color:#2ecc71;">----/--/--</span>
        </div>

        <div class="page-title">)rawliteral" CPU_NAME R"rawliteral(</div>

        <div>
            <button class="nav-btn" id="btn_short" onclick="setMode('short')">SHORT MODE</button>
            <button class="nav-btn" id="btn_full" onclick="setMode('full')">FULL MODE</button>
        </div>
    </div>

    <!-- SHORT MODE VIEW -->
    <div id="view_short" class="container">
        <div class="card" style="width: 420px;">
            <h3>FIRMWARE FLASH</h3>
            <form id="upload_form" enctype="multipart/form-data">
                <input type="file" name="update" id="file_select" accept=".bin"><br>
                <input type="button" value="Upload & Flash Firmware" onclick="uploadFile()">
            </form>
            <div class="progress-wrapper" id="prg_wrap">
                <div class="progress-bar" id="prg_bar">0%</div>
            </div>
            <div id="status"></div>
        </div>
    </div>

    <!-- FULL MODE VIEW -->
    <div id="view_full" class="container hidden">
        <!-- Card 1: Temperature Values (C & F Distinct) -->
        <div class="card">
            <h3> Temperature Values</h3>
            <div class="metric"><span>Oil Temp:</span> <span class="val" id="t_oil">--</span></div>
            <div class="metric"><span>Winding Temp:</span> <span class="val" id="t_wind">--</span></div>
            <div class="metric"><span>Tank Temp:</span> <span class="val" id="t_tank">--</span></div>
            <div class="metric"><span>Ambient Temp:</span> <span class="val" id="t_amb">--</span></div>
            <div class="metric"><span>Stage A Discharge Temp:</span> <span class="val" id="t_stageA">--</span></div>
            <div class="metric"><span>Stage B Discharge Temp:</span> <span class="val" id="t_stageB">--</span></div>
        </div>

        <!-- Card 2: Cooling Logic -->
        <div class="card">
            <h3>COOLING LOGIC</h3>
            <div class="metric"><span>COOLING STATE:</span> <span class="val" id="cool_state">--</span></div>
            <div class="metric"><span>COOLING MODE:</span> <span class="val" id="cool_mode">--</span></div>
            <div class="metric"><span>STAGE A:</span> <span class="val" id="stage_a">--</span></div>
            <div class="metric"><span>STAGE B:</span> <span class="val" id="stage_b">--</span></div>
            
            <button id="btn_mode_toggle" class="btn-action" onclick="toggleCoolingMode()">Switch Mode</button>
            
            <div id="manual_controls_panel" class="manual-controls hidden">
                <button id="btn_stage_a" class="btn-action" onclick="toggleStage('A')">Stage A: OFF</button>
                <button id="btn_stage_b" class="btn-action" onclick="toggleStage('B')">Stage B: OFF</button>
            </div>
        </div>

        <!-- Card 3: Pressure Values (Stage A & B Discharge) -->
        <div class="card">
            <h3>Pressure Values</h3>
            <div class="metric"><span>Stage A Discharge Pressure (PSI):</span> <span class="val" id="p_stageA">--</span></div>
            <div class="metric"><span>Stage B Discharge Pressure (PSI):</span> <span class="val" id="p_stageB">--</span></div>
        </div>

        <!-- Card 4: Power Telemetry & PF Settings -->
        <div class="card">
            <h3>POWER CALCULATIONS</h3>
            <div class="metric"><span>CURRENT POWER FACTOR:</span> <span class="val" id="curr_pf">--</span></div>
            <div class="metric"><span>REAL POWER (kW):</span> <span class="val" id="real_pwr">--</span></div>
            <div class="metric"><span>REACTIVE POWER (kVAR):</span> <span class="val" id="react_pwr">--</span></div>
            <div class="metric"><span>APPARENT POWER (kVA):</span> <span class="val" id="app_pwr">--</span></div>
            <form onsubmit="submitForm(event, '/set_pf', 'pf_status')" style="margin-top: 10px;">
                <label style="font-size: 12px;">Set Power Factor (0.0 - 1.0):</label>
                <input type="number" step="0.01" min="0" max="1" name="pf" required>
                <input type="submit" value="Save Power Factor">
            </form>
            <div id="pf_status" class="form-status"></div>
        </div>

        <!-- Card 5: Set Date & Time -->
        <div class="card">
            <h3>SET DATE & TIME</h3>
            <div class="switch-container">
                <span>Auto Time & Date Sync From Server/FPGA:</span>
                <label class="switch">
                    <input type="checkbox" id="auto_sync_toggle" onchange="toggleAutoSync(this)">
                    <span class="slider"></span>
                </label>
            </div>
            <div id="manual_time_container" style="margin-top: 30px;">
                <div style="font-size: 13px; font-weight: bold; color: #f39c12; margin-bottom: 8px; text-align: left;">Manual Set</div>
                <form onsubmit="submitForm(event, '/set_time', 'time_status')">
                    <input type="datetime-local" id="datetime_input" name="datetime" required><br>
                    <input type="submit" id="submit_time_btn" value="Sync Date & Time">
                </form>
                <div id="time_status" class="form-status"></div>
            </div>
        </div>

        <!-- Card 6: AC Voltage -->
        <div class="card">
            <h3>AC VOLTAGE</h3>
            <div class="metric"><span>Phase 1 AC Voltage (Volts):</span> <span class="val" id="v_a">--</span></div>
            <div class="metric"><span>Phase 2 AC Voltage (Volts):</span> <span class="val" id="v_b">--</span></div>
            <div class="metric"><span>Phase 3 AC Voltage (Volts):</span> <span class="val" id="v_c">--</span></div>
        </div>

        <!-- Card 7: AC Current -->
        <div class="card">
            <h3>AC Current</h3>
            <div class="metric"><span>Phase 1 Current (Amps):</span> <span class="val" id="c_1">--</span></div>
            <div class="metric"><span>Phase 2 Current (Amps):</span> <span class="val" id="c_2">--</span></div>
            <div class="metric"><span>Phase 3 Current (Amps):</span> <span class="val" id="c_3">--</span></div>
        </div>

        <!-- Card 8: Compressor Timers -->
        <div class="card">
            <h3>COMPRESSOR TIMERS</h3>
            <div class="metric"><span>Compressor Time ON (Minutes):</span> <span class="val" id="time_on">--</span></div>
            <div class="metric"><span>Compressor Time OFF (Minutes):</span> <span class="val" id="time_off">--</span></div>
            <form onsubmit="submitForm(event, '/set_timers', 'timer_status')" style="margin-top: 10px;">
                <input type="number" name="ontime" placeholder="Set ON Time (Minutes)" required>
                <input type="number" name="offtime" placeholder="Set OFF Time (Minutes)" required>
                <input type="submit" value="Save Timers">
            </form>
            <div id="timer_status" class="form-status"></div>
        </div>

        <!-- Card 9: Datalogging (SD Card) -->
        <div class="card">
            <h3>DATALOGGING</h3>
            <div class="metric"><span>EXT. MEMORY STATUS:</span> <div><span class="val" id="sd_status_text">--</span> <span id="sd_dot" class="sd-indicator"></span></div></div>
            <div class="metric"><span>LOG RATE:</span> <span class="val" id="sd_log_rate">--</span></div>
            
            <form onsubmit="submitForm(event, '/set_sd_interval', 'sd_rate_status')" style="margin-top: 10px;">
                <label style="font-size: 12px; color: #aaa;">Set Ext. Memory Logging Rate:</label>
                <select name="interval" required>
                    <option value="1">1 Second (Default)</option>
                    <option value="5">5 Seconds</option>
                    <option value="10">10 Seconds</option>
                    <option value="30">30 Seconds</option>
                    <option value="60">1 Minute</option>
                    <option value="300">5 Minutes</option>
                    <option value="600">10 Minutes</option>
                    <option value="1800">30 Minutes</option>
                    <option value="3600">1 Hour</option>
                    <option value="21600">6 Hours</option>
                    <option value="43200">12 Hours</option>
                    <option value="86400">24 Hours</option>
                </select>
                <input type="submit" value="Save Ext. Memory Log Rate">
            </form>
            <div id="sd_rate_status" class="form-status"></div>

            <div style="margin-top:10px; font-weight:bold; font-size:12px; color:#f39c12;">LOG FILES:</div>
            <div id="sd_files_list" style="margin-top:5px; max-height:160px; overflow-y:auto;">
                <!-- Files populated dynamically -->
            </div>
        </div>

        <!-- Card 10: System Control (Reset CPU) -->
        <div class="card">
            <h3>SYSTEM CONTROL</h3>
            <p style="font-size:12px; color:#aaa; margin:5px 0 15px 0;">Reboot system hardware controller immediately.</p>
            <button class="btn-reset" onclick="resetCPU()">Reset CPU</button>
        </div>
    </div>

    <script>
    let currentMode = 'short';
    let isManualMode = false;
    let stageAState = false;
    let stageBState = false;

    function formatInterval(sec) {
        if (!sec || sec <= 0) return "--";
        if (sec < 60) return sec + " Sec";
        if (sec < 3600) return Math.floor(sec / 60) + " Min";
        return Math.floor(sec / 3600) + " Hr";
    }

    function setMode(mode) {
        currentMode = mode;
        if(mode === 'short') {
            document.getElementById('view_short').classList.remove('hidden');
            document.getElementById('view_full').classList.add('hidden');
            document.getElementById('btn_short').classList.add('active');
            document.getElementById('btn_full').classList.remove('active');
        } else {
            document.getElementById('view_full').classList.remove('hidden');
            document.getElementById('view_short').classList.add('hidden');
            document.getElementById('btn_full').classList.add('active');
            document.getElementById('btn_short').classList.remove('active');
            fetchSDFiles();
        }
    }

    function formatTemp(obj) {
        if (!obj || obj.c === "FAULT" || obj.f === "FAULT") return "FAULT";
        return obj.c + " \u00B0C | " + obj.f + " \u00B0F";
    }

    function setValColor(elementId, value) {
        let el = document.getElementById(elementId);
        if(!el) return;
        el.innerText = value;
        if (value === "FAULT" || value === "OFF" || value === "OFFLINE") {
            el.className = "val val-red";
        } else {
            el.className = "val val-green";
        }
    }

    function showNotification(elementId, message, isSuccess) {
        let statusEl = document.getElementById(elementId);
        if(!statusEl) return;
        statusEl.innerText = message;
        statusEl.className = "form-status " + (isSuccess ? "status-success" : "status-error");
        setTimeout(() => {
            statusEl.innerText = "";
        }, 4000);
    }

    function submitForm(event, endpoint, statusDivId) {
        event.preventDefault();
        const form = event.target;
        const formData = new FormData(form);
        const params = new URLSearchParams(formData).toString();

        fetch(endpoint + '?' + params, { method: 'GET' })
        .then(r => r.text())
        .then(msg => {
            showNotification(statusDivId, msg, true);
            form.reset();
            updateMetrics();
        })
        .catch(err => {
            showNotification(statusDivId, "Failed to update!", false);
        });
    }

    function toggleAutoSync(checkbox) {
        const enabled = checkbox.checked ? "1" : "0";
        fetch('/api/set_time_mode?enabled=' + enabled, { method: 'POST' })
        .then(r => r.json())
        .then(d => {
            updateTimeModeUI(d.auto_time_sync);
        });
    }

    function updateTimeModeUI(isAuto) {
        document.getElementById('auto_sync_toggle').checked = isAuto;
        let manualContainer = document.getElementById('manual_time_container');
        if (isAuto) {
            manualContainer.style.opacity = "0.4";
            document.getElementById('datetime_input').disabled = true;
            document.getElementById('submit_time_btn').disabled = true;
        } else {
            manualContainer.style.opacity = "1";
            document.getElementById('datetime_input').disabled = false;
            document.getElementById('submit_time_btn').disabled = false;
        }
    }

    function resetCPU() {
        if(confirm("Are you sure you want to reset the ESP32 CPU?")) {
            fetch('/api/reset', { method: 'POST' })
            .then(r => r.text())
            .then(msg => {
                alert(msg);
                setTimeout(() => { location.reload(); }, 3000);
            });
        }
    }

    function toggleCoolingMode() {
        let newMode = isManualMode ? 'auto' : 'manual';
        fetch('/api/set_cooling_mode?mode=' + newMode, { method: 'POST' })
        .then(r => r.json())
        .then(d => {
            updateMetrics();
        });
    }

    function toggleStage(stage) {
        let newState = false;
        if(stage === 'A') newState = !stageAState;
        if(stage === 'B') newState = !stageBState;

        fetch('/api/set_stage_override?stage=' + stage + '&state=' + (newState ? '1' : '0'), { method: 'POST' })
        .then(r => r.json())
        .then(d => {
            updateMetrics();
        });
    }

    function fetchSDFiles() {
        fetch('/api/sd/list')
        .then(r => r.json())
        .then(files => {
            let container = document.getElementById('sd_files_list');
            if(!files || files.length === 0) {
                container.innerHTML = "<div style='font-size:12px; color:#aaa;'>No log files available</div>";
                return;
            }
            let html = "";
            files.forEach(f => {
                html += `<div class="metric">
                            <span>${f}</span>
                            <div class="btn-group">
                                <a class="btn-view" href="/view?file=${f}">View</a>
                                <a class="btn-dl" href="/download?file=${f}">Download</a>
                            </div>
                         </div>`;
            });
            container.innerHTML = html;
        }).catch(e => {
            document.getElementById('sd_files_list').innerHTML = "<div style='font-size:12px; color:#aaa;'>Failed to load files</div>";
        });
    }

    function updateMetrics() {
        fetch('/api/status').then(r => r.json()).then(d => {
            // Header Info
            document.getElementById('rtc_time').innerText = d.time;
            document.getElementById('rtc_date').innerText = d.date;
            
            if(d.auto_time_sync !== undefined) {
                updateTimeModeUI(d.auto_time_sync);
            }

            let sdDot = document.getElementById('sd_dot');
            let sdTxt = document.getElementById('sd_status_text');
            if(d.sd_mounted) {
                sdDot.classList.add('sd-active');
                setValColor('sd_status_text', "ONLINE");
            } else {
                sdDot.classList.remove('sd-active');
                setValColor('sd_status_text', "OFFLINE");
            }

            if(currentMode === 'full') {
                setValColor('t_oil', formatTemp(d.oil_temp));
                setValColor('t_wind', formatTemp(d.wind_temp));
                setValColor('t_tank', formatTemp(d.tank_temp));
                setValColor('t_amb', formatTemp(d.amb_temp));
                setValColor('t_stageA', formatTemp(d.stageA_temp));
                setValColor('t_stageB', formatTemp(d.stageB_temp));

                setValColor('cool_state', d.cool_state);
                setValColor('cool_mode', d.cool_mode);
                setValColor('stage_a', d.stage_a);
                setValColor('stage_b', d.stage_b);

                isManualMode = d.override_cooling_logic;
                stageAState = d.stage_a_override;
                stageBState = d.stage_b_override;

                let btnToggle = document.getElementById('btn_mode_toggle');
                let manualPanel = document.getElementById('manual_controls_panel');

                if(isManualMode) {
                    btnToggle.innerText = "Switch to AUTOMATIC";
                    manualPanel.classList.remove('hidden');

                    let btnA = document.getElementById('btn_stage_a');
                    let btnB = document.getElementById('btn_stage_b');

                    btnA.innerText = "Stage A: " + (stageAState ? "ON" : "OFF");
                    btnA.className = stageAState ? "btn-active-on" : "btn-active-off";

                    btnB.innerText = "Stage B: " + (stageBState ? "ON" : "OFF");
                    btnB.className = stageBState ? "btn-active-on" : "btn-active-off";
                } else {
                    btnToggle.innerText = "Switch to MANUAL";
                    manualPanel.classList.add('hidden');
                }

                setValColor('p_stageA', d.stageA_press);
                setValColor('p_stageB', d.stageB_press);

                setValColor('curr_pf', d.power_factor);
                setValColor('real_pwr', d.real_power);
                setValColor('react_pwr', d.reactive_power);
                setValColor('app_pwr', d.apparent_power);

                setValColor('v_a', d.v_a);
                setValColor('v_b', d.v_b);
                setValColor('v_c', d.v_c);
                setValColor('c_1', d.c_1);
                setValColor('c_2', d.c_2);
                setValColor('c_3', d.c_3);

                setValColor('time_on', d.on_time);
                setValColor('time_off', d.off_time);

                setValColor('sd_log_rate', formatInterval(d.sd_interval));
            }
        });
    }

    setMode('short');
    setInterval(updateMetrics, 2000);
    updateMetrics();

    function uploadFile() {
        var fileInput = document.getElementById('file_select');
        if(fileInput.files.length === 0) { alert('Select a .bin file!'); return; }
        var formData = new FormData();
        formData.append("update", fileInput.files[0]);
        var xhr = new XMLHttpRequest();
        document.getElementById('prg_wrap').style.display = 'block';
        document.getElementById('status').innerHTML = "Flashing firmware...";
        xhr.upload.addEventListener("progress", function(evt) {
            if (evt.lengthComputable) {
                var p = Math.round((evt.loaded / evt.total) * 100);
                document.getElementById('prg_bar').style.width = p + '%';
                document.getElementById('prg_bar').innerHTML = p + '%';
            }
        }, false);
        xhr.onload = function() {
            if (xhr.status == 200) {
                document.getElementById('status').innerHTML = "Firmware Upload Complete! Rebooting device...";
                setTimeout(function() { 
                    alert("Upload complete. Page reloaded.");
                    location.reload(); 
                }, 4000);
            } else {
                document.getElementById('status').innerHTML = "Flash failed!";
            }
        };
        xhr.open("POST", "/update");
        xhr.send(formData);
    }
    </script>
</body>
</html>
)rawliteral";

// ==========================================
// HTML FILE VIEWER PAGE
// ==========================================
const char *viewPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>Log Viewer - )rawliteral" CPU_NAME R"rawliteral(</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; background: #121212; color: white; padding: 20px; margin: 0; }
        .header { display: flex; justify-content: space-between; align-items: center; background: #1e1e1e; padding: 15px 20px; border-radius: 8px; border: 1px solid #333; margin-bottom: 20px; flex-wrap: wrap; gap: 10px; }
        .page-title { color: #f39c12; font-size: 20px; font-weight: bold; margin: 0; }
        .btn-group { display: flex; gap: 10px; }
        .btn { padding: 8px 16px; border-radius: 5px; font-weight: bold; cursor: pointer; text-decoration: none; border: none; display: inline-block; font-size: 13px; text-align: center; }
        .btn-back { background: #333; color: white; border: 1px solid #555; }
        .btn-back:hover { background: #444; }
        .btn-dl { background: #2ecc71; color: black; }
        .btn-dl:hover { background: #27ae60; }
        .table-container { background: #1e1e1e; border: 1px solid #333; border-radius: 8px; padding: 15px; overflow-x: auto; max-height: calc(100vh - 160px); }
        table { width: 100%; border-collapse: collapse; font-size: 13px; text-align: left; }
        th { background: #2a2a2a; color: #f39c12; padding: 10px 12px; border-bottom: 2px solid #444; position: sticky; top: 0; z-index: 10; white-space: nowrap; }
        td { padding: 8px 12px; border-bottom: 1px solid #2a2a2a; color: #2ecc71; white-space: nowrap; }
        tr:nth-child(even) { background: #181818; }
        tr:hover { background: #252525; }
        .status-msg { text-align: center; color: #aaa; padding: 20px; font-size: 14px; }
    </style>
</head>
<body>
    <div class="header">
        <div class="page-title">LOG FILE: <span id="file_title" style="color:white;">--</span></div>
        <div class="btn-group">
            <a href="/" class="btn btn-back">Back to Dashboard</a>
            <a id="download_link" href="#" class="btn btn-dl">Download File</a>
        </div>
    </div>

    <div class="table-container">
        <div id="loading" class="status-msg">Loading file contents...</div>
        <table id="data_table" class="hidden">
            <thead id="table_head"></thead>
            <tbody id="table_body"></tbody>
        </table>
    </div>

    <script>
        const urlParams = new URLSearchParams(window.location.search);
        const filename = urlParams.get('file');

        if (!filename) {
            document.getElementById('loading').innerText = "No file specified.";
        } else {
            document.getElementById('file_title').innerText = filename;
            document.getElementById('download_link').href = "/download?file=" + encodeURIComponent(filename);

            fetch('/api/view?file=' + encodeURIComponent(filename))
                .then(r => {
                    if (!r.ok) throw new Error("File not found or empty");
                    return r.text();
                })
                .then(csv => {
                    document.getElementById('loading').classList.add('hidden');
                    document.getElementById('data_table').classList.remove('hidden');

                    const lines = csv.trim().split('\n');
                    if (lines.length === 0 || lines[0].trim() === "") {
                        document.getElementById('loading').innerText = "File is empty.";
                        document.getElementById('loading').classList.remove('hidden');
                        document.getElementById('data_table').classList.add('hidden');
                        return;
                    }

                    const headers = lines[0].split(',');
                    let theadHtml = "<tr>";
                    headers.forEach(h => {
                        theadHtml += `<th>${h.trim()}</th>`;
                    });
                    theadHtml += "</tr>";
                    document.getElementById('table_head').innerHTML = theadHtml;

                    let tbodyHtml = "";
                    for (let i = 1; i < lines.length; i++) {
                        if (!lines[i].trim()) continue;
                        const row = lines[i].split(',');
                        tbodyHtml += "<tr>";
                        row.forEach(cell => {
                            tbodyHtml += `<td>${cell.trim()}</td>`;
                        });
                        tbodyHtml += "</tr>";
                    }
                    document.getElementById('table_body').innerHTML = tbodyHtml;
                })
                .catch(err => {
                    document.getElementById('loading').innerText = "Failed to load log file: " + err.message;
                });
        }
    </script>
</body>
</html>
)rawliteral";

// ==========================================
// EEPROM INITIALIZATION & LOAD
// ==========================================
void loadEEPROMSettings() {
  EEPROM.begin(EEPROM_SIZE);
  uint16_t magic = 0;
  EEPROM.get(EEPROM_MAGIC_ADDR, magic);

  if (magic != EEPROM_MAGIC_VAL) {
    Serial.println("EEPROM First Boot: Initializing default parameters...");
    EEPROM.put(EEPROM_ADDR_PF, powerFactor);
    EEPROM.put(EEPROM_ADDR_ONTIME, onTime);
    EEPROM.put(EEPROM_ADDR_OFFTIME, offTime);
    EEPROM.put(EEPROM_ADDR_SD_INTERVAL, sdLogInterval);
    EEPROM.put(EEPROM_ADDR_AUTO_TIME, (uint8_t)1);  // Default ON
    EEPROM.put(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
    EEPROM.commit();
    autoTimeSyncEnabled = true;
  } else {
    EEPROM.get(EEPROM_ADDR_PF, powerFactor);
    EEPROM.get(EEPROM_ADDR_ONTIME, onTime);
    EEPROM.get(EEPROM_ADDR_OFFTIME, offTime);
    EEPROM.get(EEPROM_ADDR_SD_INTERVAL, sdLogInterval);
    uint8_t autoTimeVal = 1;
    EEPROM.get(EEPROM_ADDR_AUTO_TIME, autoTimeVal);
    autoTimeSyncEnabled = (autoTimeVal == 1);

    if (sdLogInterval < 1 || sdLogInterval > 86400) {
      sdLogInterval = 1;
    }

    T_RUN_MIN = onTime * 60 * 1000UL;
    T_OFF_MIN = offTime * 60 * 1000UL;

    Serial.printf("EEPROM Loaded: PF=%.2f, ON_Time=%u min, OFF_Time=%u min, SD_Interval=%u sec, AutoTimeSync=%s\n",
                  powerFactor, onTime, offTime, sdLogInterval, autoTimeSyncEnabled ? "true" : "false");
  }
}

// ==========================================
// ETHERNET EVENT HANDLER
// ==========================================
void WiFiEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("ETH Started");
      ETH.setHostname("transformer-monitor");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("ETH Link Connected");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.print("ETH Static IP Assigned: ");
      Serial.println(ETH.localIP());
      setupWebServer();
      // Trigger immediate time request upon link/IP establishment if enabled
      if (autoTimeSyncEnabled) {
        requestNetworkTime();
      }
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("ETH Hardware Disconnected.");
      serverStarted = false;
      break;
    default:
      break;
  }
}

// ==========================================
// SD CARD HELPER FUNCTIONS
// ==========================================
void initSDCard() {
  pinMode(SD_DETECT, INPUT_PULLUP);

  if (digitalRead(SD_DETECT) == HIGH) {
    Serial.println("SD Card: Not inserted!");
    sdMounted = false;
    return;
  }

  if (!SD.begin(SD_CS)) {
    Serial.println("SD Card: Initialization failed!");
    sdMounted = false;
    return;
  }

  sdMounted = true;
  Serial.println("SD Card: Initialized successfully.");

  if (!SD.exists("/gen.csv")) {
    appendFile("/gen.csv", "Date Time,State,Oil_Temp,Wind_Temp,Tank_Temp,Amb_Temp,StageA_Temp,StageB_Temp,StageA_Press,StageB_Press,Volt_A,Volt_B,Volt_C,Cur_1,Cur_2,Cur_3,Real_kW,React_kVAR,App_kVA\n");
  }
  if (!SD.exists("/temp.csv")) {
    appendFile("/temp.csv", "Date Time,Oil_Temp,Wind_Temp,Tank_Temp,Amb_Temp,StageA_Temp,StageB_Temp\n");
  }
  if (!SD.exists("/pres.csv")) {
    appendFile("/pres.csv", "Date Time,StageA_Discharge_Pressure,StageB_Discharge_Pressure\n");
  }
  if (!SD.exists("/volt.csv")) {
    appendFile("/volt.csv", "Date Time,Phase_A_Volt,Phase_B_Volt,Phase_C_Volt\n");
  }
  if (!SD.exists("/cur.csv")) {
    appendFile("/cur.csv", "Date Time,Phase_1_Cur,Phase_2_Cur,Phase_3_Cur\n");
  }
  if (!SD.exists("/power.csv")) {
    appendFile("/power.csv", "Date Time,Real_Power_kW,Reactive_Power_kVAR,Apparent_Power_kVA,Power_Factor\n");
  }
}

void appendFile(const char *path, const String &message) {
  if (!sdMounted) return;
  File file = SD.open(path, FILE_APPEND);
  if (!file) {
    Serial.printf("SD Card: Failed to open %s for appending\n", path);
    return;
  }
  file.print(message);
  file.close();
}

// ==========================================
// CRC32 Calculation
// ==========================================
uint32_t computeCRC32(uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= ((uint32_t)data[i] << 24);
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x80000000)
        crc = (crc << 1) ^ 0x04C11DB7;
      else
        crc <<= 1;
    }
  }
  return crc;
}

// ==========================================
// Packet Factory & Serialization
// ==========================================
void buildPacket(uint32_t DEVICE_HEADER, float t1, float t2, float t3, float t4, float stA, float stB, float p1, float p2, float v1, float v2, float v3, float c1, float c2, float c3, float aP, float rP, float recP, float pf) {
  memset(packet, 0, packetSize);

  packet[0] = (DEVICE_HEADER >> 24) & 0xFF;
  packet[1] = (DEVICE_HEADER >> 16) & 0xFF;
  packet[2] = (DEVICE_HEADER >> 8) & 0xFF;
  packet[3] = (DEVICE_HEADER)&0xFF;

  int16_t scaled_toil = (t1 == SENSOR_DISCONNECTED || faultOil) ? PACKET_SENSOR_ERROR : (int16_t)(t1 * 100.0f);
  int16_t scaled_twind = (t2 == SENSOR_DISCONNECTED || faultWind) ? PACKET_SENSOR_ERROR : (int16_t)(t2 * 100.0f);
  int16_t scaled_tank = (t3 == SENSOR_DISCONNECTED || faultTank) ? PACKET_SENSOR_ERROR : (int16_t)(t3 * 100.0f);
  int16_t scaled_amb = (t4 == SENSOR_DISCONNECTED || faultAmb) ? PACKET_SENSOR_ERROR : (int16_t)(t4 * 100.0f);
  int16_t scaled_tStageA = (stA == SENSOR_DISCONNECTED || faultStageA) ? PACKET_SENSOR_ERROR : (int16_t)(stA * 100.0f);
  int16_t scaled_tStageB = (stB == SENSOR_DISCONNECTED || faultStageB) ? PACKET_SENSOR_ERROR : (int16_t)(stB * 100.0f);
  int16_t scaled_pStageA = (p1 == SENSOR_DISCONNECTED || faultPressStageA) ? PACKET_SENSOR_ERROR : (int16_t)(p1 * 100.0f);
  int16_t scaled_pStageB = (p2 == SENSOR_DISCONNECTED || faultPressStageB) ? PACKET_SENSOR_ERROR : (int16_t)(p2 * 100.0f);
  int16_t scaled_volt1 = (acVoltages.faultA) ? PACKET_SENSOR_ERROR : (int16_t)(v1 * 100.0f);
  int16_t scaled_volt2 = (acVoltages.faultB) ? PACKET_SENSOR_ERROR : (int16_t)(v2 * 100.0f);
  int16_t scaled_volt3 = (acVoltages.faultC) ? PACKET_SENSOR_ERROR : (int16_t)(v3 * 100.0f);
  int16_t scaled_cur1 = (acCurrents.faultCur1) ? PACKET_SENSOR_ERROR : (int16_t)(c1 * 100.0f);
  int16_t scaled_cur2 = (acCurrents.faultCur2) ? PACKET_SENSOR_ERROR : (int16_t)(c2 * 100.0f);
  int16_t scaled_cur3 = (acCurrents.faultCur3) ? PACKET_SENSOR_ERROR : (int16_t)(c3 * 100.0f);
  int16_t scaled_aP = (int16_t)(aP * 100.0f);
  int16_t scaled_rP = (int16_t)(rP * 100.0f);
  int16_t scaled_recP = (int16_t)(recP * 100.0f);
  int16_t scaled_pf = (int16_t)(pf * 100.0f);

  packet[8] = (scaled_toil >> 8) & 0xFF;
  packet[9] = scaled_toil & 0xFF;
  packet[10] = (scaled_twind >> 8) & 0xFF;
  packet[11] = scaled_twind & 0xFF;
  packet[12] = (scaled_tank >> 8) & 0xFF;
  packet[13] = scaled_tank & 0xFF;

  packet[14] = (scaled_amb >> 8) & 0xFF;
  packet[15] = scaled_amb & 0xFF;

  packet[16] = (scaled_tStageA >> 8) & 0xFF;
  packet[17] = scaled_tStageA & 0xFF;

  packet[18] = (scaled_tStageB >> 8) & 0xFF;
  packet[19] = scaled_tStageB & 0xFF;

  packet[20] = (scaled_pStageA >> 8) & 0xFF;
  packet[21] = scaled_pStageA & 0xFF;

  packet[22] = (scaled_pStageB >> 8) & 0xFF;
  packet[23] = scaled_pStageB & 0xFF;
  packet[24] = (scaled_volt1 >> 8) & 0xFF;
  packet[25] = scaled_volt1 & 0xFF;
  packet[26] = (scaled_volt2 >> 8) & 0xFF;
  packet[27] = scaled_volt2 & 0xFF;

  packet[28] = (scaled_volt3 >> 8) & 0xFF;
  packet[29] = scaled_volt3 & 0xFF;
  packet[30] = (scaled_cur1 >> 8) & 0xFF;
  packet[31] = scaled_cur1 & 0xFF;

  packet[32] = (scaled_cur2 >> 8) & 0xFF;
  packet[33] = scaled_cur2 & 0xFF;

  packet[34] = (scaled_cur3 >> 8) & 0xFF;
  packet[35] = scaled_cur3 & 0xFF;

  packet[36] = (scaled_aP >> 8) & 0xFF;
  packet[37] = scaled_aP & 0xFF;

  packet[38] = (scaled_rP >> 8) & 0xFF;
  packet[39] = scaled_rP & 0xFF;

  packet[40] = (scaled_recP >> 8) & 0xFF;
  packet[41] = scaled_recP & 0xFF;

  packet[42] = (scaled_pf >> 8) & 0xFF;
  packet[43] = scaled_pf & 0xFF;

  uint32_t crc = computeCRC32(&packet[8], 24);
  packet[4] = (crc >> 24) & 0xFF;
  packet[5] = (crc >> 16) & 0xFF;
  packet[6] = (crc >> 8) & 0xFF;
  packet[7] = crc & 0xFF;
}

// ==========================================
// TIME SYNC PROTOCOL FROM SERVER/FPGA
// ==========================================
void requestNetworkTime() {
  if (!autoTimeSyncEnabled) return;
  uint8_t reqPacket[2] = { 0xAA, 0x01 };
  udp.writeTo(reqPacket, 2, timeServer1, TIME_SERVER_PORT);
  udp.writeTo(reqPacket, 2, timeServer2, TIME_SERVER_PORT);
  Serial.println("[TIME SYNC] Sent time request (AA 01) to servers.");
}

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // Initialize EEPROM
  loadEEPROMSettings();

  // Initialize I2C Bus
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // Initialize ADS1115
  if (!pressure_2.begin(ADS1115_ADDRESS, &Wire)) {
    Serial.println("Error: ADS1115 Stage B Discharge Pressure Sensor not detected at 0x49!");
    adsPresent = false;
  } else {
    pressure_2.setGain(GAIN_TWOTHIRDS);
    adsPresent = true;
    Serial.println("ADS1115 Pressure Sensor successfully initialized.");
  }

  // Initialize DS3231 RTC
  if (!rtc.begin(&Wire)) {
    Serial.println("Error: DS3231 RTC not detected at 0x68!");
    rtcPresent = false;
  } else {
    rtcPresent = true;
    Serial.println("DS3231 RTC successfully initialized.");
    if (rtc.lostPower()) {
      Serial.println("RTC lost power, resetting to compile time.");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }

  SerialSender.begin(115200, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);

  pinMode(PIN_STAGE_A, OUTPUT);
  pinMode(PIN_STAGE_B, OUTPUT);
  digitalWrite(PIN_STAGE_A, LOW);
  digitalWrite(PIN_STAGE_B, LOW);

  pinMode(PIN_T_TANK, OUTPUT);
  pinMode(PIN_T_WIND, OUTPUT);
  pinMode(PIN_T_OIL, OUTPUT);
  pinMode(ETH_CS, OUTPUT);
  pinMode(SD_CS, OUTPUT);

  digitalWrite(PIN_T_TANK, HIGH);
  digitalWrite(PIN_T_WIND, HIGH);
  digitalWrite(PIN_T_OIL, HIGH);
  digitalWrite(ETH_CS, HIGH);
  digitalWrite(SD_CS, HIGH);

  pinMode(PIN_PHASE_A, INPUT);
  pinMode(PIN_PHASE_B, INPUT);
  pinMode(PIN_PHASE_C, INPUT);

  analogSetPinAttenuation(PIN_CUR1, ADC_11db);
  analogSetPinAttenuation(PIN_CUR2, ADC_11db);
  analogSetPinAttenuation(PIN_CUR3, ADC_11db);

  SPI.begin(ETH_SCK, ETH_MISO, ETH_MOSI);

  // Initialize SD Card
  initSDCard();

  Network.onEvent(WiFiEvent);
  ETH.begin(ETH_PHY_TYPE, 1, ETH_CS, ETH_INT, ETH_RST, SPI);
  ETH.config(local_IP, gateway, subnet, dns);

  applyOutputs(STATE_OFF);

  Serial.println("\n------------------------------------------------");
  Serial.println("ESP32-S3 TRANSFORMER MONITOR & COOLING SYSTEM");
  Serial.println("------------------------------------------------\n");

  if (udp.listen(1234)) {
    Serial.println("UDP Listener setup locally on port 1234");

    udp.onPacket([](AsyncUDPPacket packet) {
      size_t len = packet.length();
      if (len == 9 && packet.data()[0] == 0xAA && packet.data()[1] == 0x00) {
        // Server/FPGA: Handle time sync response frame (9 bytes: AA 00 Year(2) Month Day Hour Minute Second)
        if (autoTimeSyncEnabled) {
          uint8_t *d = packet.data();
          uint16_t yr = ((uint16_t)d[2] << 8) | d[3];
          uint8_t mo = d[4];
          uint8_t da = d[5];
          uint8_t hr = d[6];
          uint8_t mn = d[7];
          uint8_t sc = d[8];

          // Validate ranges according to Server/FPGA protocol rules
          if (yr >= 2020 && yr <= 2099 && mo >= 1 && mo <= 12 && da >= 1 && da <= 31 && hr <= 23 && mn <= 59 && sc <= 59) {
            pendingSyncTime = DateTime(yr, mo, da, hr, mn, sc);
            pendingSyncFlag = true;
            timeSyncAchieved = true;
            Serial.printf("[TIME SYNC PROTOCOL] Valid time frame received: %04d-%02d-%02d %02d:%02d:%02d\n", yr, mo, da, hr, mn, sc);
          } else {
            Serial.println("[TIME SYNC PROTOCOL] Invalid value ranges in time frame packet. Dropped.");
          }
        }
        return;
      }

      // Handle standard UDP commands
      if (len > 0) {
        char *msg = (char *)malloc(len + 1);
        if (msg != nullptr) {
          memcpy(msg, packet.data(), len);
          msg[len] = '\0';

          Serial.print("UDP Command Received: ");
          Serial.println(msg);

          String command = String(msg);
          command.trim();

          if (command.equalsIgnoreCase("000001")) {
            Serial.println("Remote Reset Command Issued. Rebooting CPU...");
            delay(500);
            ESP.restart();
          } else if (command.equalsIgnoreCase("FFFFFF")) {
            overideCoolingLogic = true;
          } else if (command.equalsIgnoreCase("000000")) {
            overideCoolingLogic = false;
          } else if (command.equalsIgnoreCase("000002")) {
            Stage_A_overide = true;
          } else if (command.equalsIgnoreCase("000003")) {
            Stage_A_overide = false;
          } else if (command.equalsIgnoreCase("000004")) {
            Stage_B_overide = true;
          } else if (command.equalsIgnoreCase("000005")) {
            Stage_B_overide = false;
          } else {
            Serial.printf("Unknown command parsed: %s\n", msg);
          }
          free(msg);
        }
      }
    });
  }
}

// ==========================================
// MAIN LOOP
// ==========================================
void loop() {
  if (serverStarted) {
    server.handleClient();
  }

  // Server/FPGA sync rule: Do not touch I2C inside UDP callback; apply rtc adjustment safely here in loop()
  if (pendingSyncFlag && rtcPresent) {
    rtc.adjust(pendingSyncTime);
    pendingSyncFlag = false;
    Serial.println("[RTC] Date & Time successfully synchronized via Network Protocol A.");
  }

  // Manage automatic time synchronization polling intervals (Server/FPGA Sync rules)
  if (autoTimeSyncEnabled) {
    unsigned long nowMs = millis();
    // Retry every 5 seconds until first answer, then every 1 hour (3600000 ms)
    unsigned long interval = timeSyncAchieved ? 3600000UL : 5000UL;
    if (lastTimeRequestTime == 0 || (nowMs - lastTimeRequestTime >= interval)) {
      lastTimeRequestTime = nowMs;
      requestNetworkTime();
    }
  }

  updateSensors();
  coolingLogicControl();
  stream_to_Ethernet();
  delay(10);
}

// ==========================================
// RTC HELPER & WEB SERVER HANDLERS
// ==========================================
String getFormattedDate() {
  if (!rtcPresent) return "DISCONNECTED";
  DateTime now = rtc.now();
  char buf[16];
  snprintf(buf, sizeof(buf), "%04d/%02d/%02d", now.year(), now.month(), now.day());
  return String(buf);
}

String getFormattedTime() {
  if (!rtcPresent) return "DISCONNECTED";
  DateTime now = rtc.now();
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  return String(buf);
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", indexPage);
  });

  server.on("/view", HTTP_GET, []() {
    server.send(200, "text/html", viewPage);
  });

  server.on("/api/view", HTTP_GET, []() {
    if (!sdMounted || !server.hasArg("file")) {
      server.send(400, "text/plain", "Bad Request or SD Not Mounted");
      return;
    }
    String path = "/" + server.arg("file");
    if (!SD.exists(path)) {
      server.send(404, "text/plain", "File Not Found");
      return;
    }
    File file = SD.open(path, FILE_READ);
    server.streamFile(file, "text/plain");
    file.close();
  });

  server.on("/api/reset", HTTP_POST, []() {
    server.send(200, "text/plain", "Resetting ESP32 CPU...");
    delay(500);
    ESP.restart();
  });

  server.on("/api/sd/list", HTTP_GET, []() {
    if (!sdMounted) {
      server.send(500, "application/json", "[]");
      return;
    }
    File root = SD.open("/");
    String json = "[";
    bool first = true;
    File file = root.openNextFile();
    while (file) {
      if (!file.isDirectory()) {
        if (!first) json += ",";
        json += "\"" + String(file.name()) + "\"";
        first = false;
      }
      file = root.openNextFile();
    }
    json += "]";
    server.send(200, "application/json", json);
  });

  server.on("/download", HTTP_GET, []() {
    if (!sdMounted || !server.hasArg("file")) {
      server.send(400, "text/plain", "Bad Request or SD Not Mounted");
      return;
    }
    String path = "/" + server.arg("file");
    if (!SD.exists(path)) {
      server.send(404, "text/plain", "File Not Found");
      return;
    }
    File file = SD.open(path, FILE_READ);
    server.streamFile(file, "text/csv");
    file.close();
  });

  // Toggle Auto Time Sync (Server/FPGA vs Manual)
  server.on("/api/set_time_mode", HTTP_POST, []() {
    if (server.hasArg("enabled")) {
      String val = server.arg("enabled");
      bool prevState = autoTimeSyncEnabled;
      autoTimeSyncEnabled = (val == "1" || val.equalsIgnoreCase("true"));

      EEPROM.put(EEPROM_ADDR_AUTO_TIME, (uint8_t)(autoTimeSyncEnabled ? 1 : 0));
      EEPROM.commit();

      // Whenever switched from OFF to ON, call requestNetworkTime() once immediately
      if (!prevState && autoTimeSyncEnabled) {
        lastTimeRequestTime = 0;   // Clear timer to allow instant dispatch
        timeSyncAchieved = false;  // Reset sync status so retry mechanism operates correctly
        requestNetworkTime();      // Called once immediately
        Serial.println("[TIME SYNC MODE] Switched ON: requestNetworkTime() called once.");
      }

      Serial.printf("[TIME SYNC MODE] Auto Time Sync from Server/FPGA is now %s\n", autoTimeSyncEnabled ? "ENABLED" : "SUSPENDED (Manual)");
      server.send(200, "application/json", "{\"status\":\"ok\",\"auto_time_sync\":" + String(autoTimeSyncEnabled ? "true" : "false") + "}");
      return;
    }
    server.send(400, "text/plain", "Missing enabled argument");
  });

  server.on("/api/set_cooling_mode", HTTP_POST, []() {
    if (server.hasArg("mode")) {
      String mode = server.arg("mode");
      if (mode.equalsIgnoreCase("manual")) {
        overideCoolingLogic = true;
      } else if (mode.equalsIgnoreCase("auto")) {
        overideCoolingLogic = false;
      }
      server.send(200, "application/json", "{\"status\":\"ok\",\"overrideCoolingLogic\":" + String(overideCoolingLogic ? "true" : "false") + "}");
      return;
    }
    server.send(400, "text/plain", "Missing mode argument");
  });

  server.on("/api/set_stage_override", HTTP_POST, []() {
    if (server.hasArg("stage") && server.hasArg("state")) {
      String stage = server.arg("stage");
      bool state = (server.arg("state") == "1" || server.arg("state").equalsIgnoreCase("true"));

      if (stage.equalsIgnoreCase("A")) {
        Stage_A_overide = state;
      } else if (stage.equalsIgnoreCase("B")) {
        Stage_B_overide = state;
      }

      server.send(200, "application/json", "{\"status\":\"ok\",\"Stage_A_overide\":" + String(Stage_A_overide ? "true" : "false") + ",\"Stage_B_overide\":" + String(Stage_B_overide ? "true" : "false") + "}");
      return;
    }
    server.send(400, "text/plain", "Missing arguments");
  });

  server.on("/set_time", HTTP_GET, []() {
    if (autoTimeSyncEnabled) {
      server.send(400, "text/plain", "Manual time setting is suspended while Auto Time Sync from Server/FPGA is active.");
      return;
    }
    if (server.hasArg("datetime")) {
      String dtParam = server.arg("datetime");

      int year = dtParam.substring(0, 4).toInt();
      int month = dtParam.substring(5, 7).toInt();
      int day = dtParam.substring(8, 10).toInt();
      int hour = dtParam.substring(11, 13).toInt();
      int minute = dtParam.substring(14, 16).toInt();

      if (year > 2000 && month > 0 && day > 0 && rtcPresent) {
        rtc.adjust(DateTime(year, month, day, hour, minute, 0));
        Serial.printf("[MANUAL TIME UPDATE] New Time Set: %04d-%02d-%02d %02d:%02d:00\n", year, month, day, hour, minute);
        server.send(200, "text/plain", "Date & Time Updated Successfully (Manual Mode)!");
        return;
      }
    }
    server.send(400, "text/plain", "Invalid Request or RTC Not Found.");
  });

  server.on("/set_pf", HTTP_GET, []() {
    if (server.hasArg("pf")) {
      float pfVal = server.arg("pf").toFloat();
      if (pfVal >= 0.0f && pfVal <= 1.0f) {
        powerFactor = pfVal;
        EEPROM.put(EEPROM_ADDR_PF, powerFactor);
        EEPROM.commit();
        Serial.printf("[EEPROM WRITE] Power Factor Updated: %.2f\n", powerFactor);
        server.send(200, "text/plain", "Power Factor Updated Successfully!");
        return;
      }
    }
    server.send(400, "text/plain", "Invalid Power Factor Value.");
  });

  server.on("/set_timers", HTTP_GET, []() {
    if (server.hasArg("ontime") && server.hasArg("offtime")) {
      int onVal = server.arg("ontime").toInt();
      int offVal = server.arg("offtime").toInt();
      if (onVal > 0 && offVal > 0) {
        onTime = static_cast<uint16_t>(onVal);
        offTime = static_cast<uint16_t>(offVal);
        T_RUN_MIN = onTime * 60 * 1000UL;
        T_OFF_MIN = offTime * 60 * 1000UL;

        EEPROM.put(EEPROM_ADDR_ONTIME, onTime);
        EEPROM.put(EEPROM_ADDR_OFFTIME, offTime);
        EEPROM.commit();

        Serial.printf("[EEPROM WRITE] Compressor Timers Updated: ON=%u Min, OFF=%u Min\n", onTime, offTime);
        server.send(200, "text/plain", "Timers Updated Successfully!");
        return;
      }
    }
    server.send(400, "text/plain", "Invalid Timer Values.");
  });

  server.on("/set_sd_interval", HTTP_GET, []() {
    if (server.hasArg("interval")) {
      long intervalVal = server.arg("interval").toInt();
      if (intervalVal >= 1 && intervalVal <= 86400) {
        sdLogInterval = static_cast<uint32_t>(intervalVal);
        EEPROM.put(EEPROM_ADDR_SD_INTERVAL, sdLogInterval);
        EEPROM.commit();
        Serial.printf("[EEPROM WRITE] SD Logging Interval Updated: %u seconds\n", sdLogInterval);
        server.send(200, "text/plain", "SD Logging Interval Updated Successfully!");
        return;
      }
    }
    server.send(400, "text/plain", "Invalid Interval Value (1 to 86400 sec).");
  });

  server.on("/api/status", HTTP_GET, []() {
    String json = "{";
    json += "\"date\":\"" + getFormattedDate() + "\",";
    json += "\"time\":\"" + getFormattedTime() + "\",";
    json += "\"auto_time_sync\":" + String(autoTimeSyncEnabled ? "true" : "false") + ",";
    json += "\"sd_mounted\":" + String(sdMounted ? "true" : "false") + ",";
    json += "\"sd_interval\":" + String(sdLogInterval) + ",";

    json += "\"cool_state\":\"" + String(stateNames[currentState]) + "\",";
    json += "\"cool_mode\":\"" + String(overideCoolingLogic ? "MANUAL" : "AUTOMATIC") + "\",";
    json += "\"override_cooling_logic\":" + String(overideCoolingLogic ? "true" : "false") + ",";
    json += "\"stage_a_override\":" + String(Stage_A_overide ? "true" : "false") + ",";
    json += "\"stage_b_override\":" + String(Stage_B_overide ? "true" : "false") + ",";
    json += "\"stage_a\":\"" + String((digitalRead(PIN_STAGE_A) == HIGH) ? "ON" : "OFF") + "\",";
    json += "\"stage_b\":\"" + String((digitalRead(PIN_STAGE_B) == HIGH) ? "ON" : "OFF") + "\",";

    json += "\"oil_temp\":" + String(faultOil ? "{\"c\":\"FAULT\",\"f\":\"FAULT\"}" : "{\"c\":\"" + String(tempOil, 1) + "\",\"f\":\"" + String(cToF(tempOil), 1) + "\"}") + ",";
    json += "\"wind_temp\":" + String(faultWind ? "{\"c\":\"FAULT\",\"f\":\"FAULT\"}" : "{\"c\":\"" + String(tempWinding, 1) + "\",\"f\":\"" + String(cToF(tempWinding), 1) + "\"}") + ",";
    json += "\"tank_temp\":" + String(faultTank ? "{\"c\":\"FAULT\",\"f\":\"FAULT\"}" : "{\"c\":\"" + String(tempTank, 1) + "\",\"f\":\"" + String(cToF(tempTank), 1) + "\"}") + ",";
    json += "\"amb_temp\":" + String(faultAmb ? "{\"c\":\"FAULT\",\"f\":\"FAULT\"}" : "{\"c\":\"" + String(t_amb, 1) + "\",\"f\":\"" + String(cToF(t_amb), 1) + "\"}") + ",";
    json += "\"stageA_temp\":" + String(faultStageA ? "{\"c\":\"FAULT\",\"f\":\"FAULT\"}" : "{\"c\":\"" + String(t_stageA, 1) + "\",\"f\":\"" + String(cToF(t_stageA), 1) + "\"}") + ",";
    json += "\"stageB_temp\":" + String(faultStageB ? "{\"c\":\"FAULT\",\"f\":\"FAULT\"}" : "{\"c\":\"" + String(t_stageB, 1) + "\",\"f\":\"" + String(cToF(t_stageB), 1) + "\"}") + ",";

    json += "\"stageA_press\":\"" + (faultPressStageA ? "FAULT" : String(p_stageA, 1)) + "\",";
    json += "\"stageB_press\":\"" + ((faultPressStageB || isnan(p_stageB)) ? "FAULT" : String(p_stageB, 1)) + "\",";

    json += "\"v_a\":\"" + (acVoltages.faultA ? "FAULT" : String(acVoltages.phaseA, 1)) + "\",";
    json += "\"v_b\":\"" + (acVoltages.faultB ? "FAULT" : String(acVoltages.phaseB, 1)) + "\",";
    json += "\"v_c\":\"" + (acVoltages.faultC ? "FAULT" : String(acVoltages.phaseC, 1)) + "\",";

    json += "\"c_1\":\"" + (acCurrents.faultCur1 ? "FAULT" : String(acCurrents.cur1_rms, 1)) + "\",";
    json += "\"c_2\":\"" + (acCurrents.faultCur2 ? "FAULT" : String(acCurrents.cur2_rms, 1)) + "\",";
    json += "\"c_3\":\"" + (acCurrents.faultCur3 ? "FAULT" : String(acCurrents.cur3_rms, 1)) + "\",";

    json += "\"power_factor\":" + String(powerFactor, 2) + ",";
    json += "\"real_power\":" + String(powerData.realPower, 2) + ",";
    json += "\"reactive_power\":" + String(powerData.reactivePower, 2) + ",";
    json += "\"apparent_power\":" + String(powerData.apparentPower, 2) + ",";
    json += "\"on_time\":" + String(onTime) + ",";
    json += "\"off_time\":" + String(offTime);

    json += "}";
    server.send(200, "application/json", json);
  });

  server.on(
    "/update", HTTP_POST, []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", (Update.hasError()) ? "FLASH FAILED" : "SUCCESS! Device is rebooting...");
      delay(1000);
      ESP.restart();
    },
    []() {
      HTTPUpload &upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("Update started: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
          Serial.printf("Update Success: %u bytes\nRebooting...\n", upload.totalSize);
        } else {
          Update.printError(Serial);
        }
      }
    });

  server.begin();
  serverStarted = true;
  Serial.println("OTA Web Server Operational on Port 80.");
}

// ==========================================
// POWER CALCULATION FUNCTION
// ==========================================
PowerTelemetry calculatePower(const PhaseVoltages &v, const PhaseCurrents &c, float pf) {
  PowerTelemetry p = { 0.0f, 0.0f, 0.0f };

  float va = v.faultA ? 0.0f : v.phaseA;
  float vb = v.faultB ? 0.0f : v.phaseB;
  float vc = v.faultC ? 0.0f : v.phaseC;

  float ia = c.faultCur1 ? 0.0f : c.cur1_rms;
  float ib = c.faultCur2 ? 0.0f : c.cur2_rms;
  float ic = c.faultCur3 ? 0.0f : c.cur3_rms;

  float appA = (va * ia) / 1000.0f;
  float appB = (vb * ib) / 1000.0f;
  float appC = (vc * ic) / 1000.0f;
  p.apparentPower = appA + appB + appC;

  p.realPower = p.apparentPower * pf;

  if (p.apparentPower >= p.realPower) {
    p.reactivePower = sqrtf((p.apparentPower * p.apparentPower) - (p.realPower * p.realPower));
  } else {
    p.reactivePower = 0.0f;
  }

  return p;
}

// ==========================================
// SENSING & SAMPLING FUNCTIONS
// ==========================================
PhaseVoltages readThreePhaseVoltage() {
  PhaseVoltages v;
  uint32_t mvA = 0, mvB = 0, mvC = 0;

  for (int i = 0; i < AC_OVERSAMPLES; i++) {
    mvA += analogReadMilliVolts(PIN_PHASE_A);
    mvB += analogReadMilliVolts(PIN_PHASE_B);
    mvC += analogReadMilliVolts(PIN_PHASE_C);
    delayMicroseconds(50);
  }

  float vAdcA = (mvA / (float)AC_OVERSAMPLES) / 1000.0f;
  float vAdcB = (mvB / (float)AC_OVERSAMPLES) / 1000.0f;
  float vAdcC = (mvC / (float)AC_OVERSAMPLES) / 1000.0f;

  v.phaseA = vAdcA * AC_TOTAL_MULTIPLIER;
  v.phaseB = vAdcB * AC_TOTAL_MULTIPLIER;
  v.phaseC = vAdcC * AC_TOTAL_MULTIPLIER;

  v.faultA = (v.phaseA < AC_DISCONNECT_THRESH);
  v.faultB = (v.phaseB < AC_DISCONNECT_THRESH);
  v.faultC = (v.phaseC < AC_DISCONNECT_THRESH);

  return v;
}

float measureCurrentRMS(uint8_t pin, bool &faultFlag) {
  uint32_t sumSamples = 0;
  float sumSquaredVolts = 0.0f;

  uint32_t startTime = micros();
  for (uint16_t i = 0; i < CT_SAMPLES; i++) {
    sumSamples += analogRead(pin);
    while ((micros() - startTime) < (i * CT_SAMPLE_INT_US))
      ;
  }
  float dcBiasADC = static_cast<float>(sumSamples) / CT_SAMPLES;

  startTime = micros();
  for (uint16_t i = 0; i < CT_SAMPLES; i++) {
    float rawADC = static_cast<float>(analogRead(pin));

    float offsetADC = rawADC - dcBiasADC;
    float volts = (offsetADC / ADC_RESOLUTION) * ADC_VREF;
    sumSquaredVolts += (volts * volts);

    while ((micros() - startTime) < (i * CT_SAMPLE_INT_US))
      ;
  }

  float vRMS = sqrtf(sumSquaredVolts / static_cast<float>(CT_SAMPLES));
  float iRMS = vRMS * AMPS_PER_VOLT;

  if (iRMS < CURRENT_DISCONNECT_THRESH) {
    faultFlag = true;
    return SENSOR_DISCONNECTED;
  }

  faultFlag = false;
  return iRMS;
}

PhaseCurrents readThreePhaseCurrent() {
  PhaseCurrents c;
  c.cur1_rms = measureCurrentRMS(PIN_CUR1, c.faultCur1);
  c.cur2_rms = measureCurrentRMS(PIN_CUR2, c.faultCur2);
  c.cur3_rms = measureCurrentRMS(PIN_CUR3, c.faultCur3);
  return c;
}

float readTemperature(int temPin, bool &faultFlag) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(temPin, LOW);

  uint32_t raw = 0;
  for (int i = 0; i < 4; i++) {
    raw <<= 8;
    raw |= SPI.transfer(0x00);
  }

  digitalWrite(temPin, HIGH);
  SPI.endTransaction();

  if ((raw & 0x00010000) || raw == 0x00000000 || raw == 0xFFFFFFFF) {
    faultFlag = true;
    return SENSOR_DISCONNECTED;
  }

  int16_t value = (raw >> 18) & 0x3FFF;
  if (raw & 0x80000000) value |= 0xC000;

  faultFlag = false;
  return (value * 0.25f) - 3.0f;
}

float readPressure_1(uint8_t pin) {
  uint32_t mv = analogReadMilliVolts(pin);
  float voltage = mv / 1000.0f;

  if (voltage < DISCONNECT_THRESH) {
    faultPressStageA = true;
    return SENSOR_DISCONNECTED;
  }

  float pressure = ((voltage - MIN_VOLTAGE) / (MAX_VOLTAGE - MIN_VOLTAGE)) * MAX_PRESSURE;
  faultPressStageA = false;
  return (pressure < 0.0f) ? 0.0f : pressure;
}

float readPressure_2(uint8_t channel) {
  if (!adsPresent || channel > 3) {
    faultPressStageB = true;
    return SENSOR_DISCONNECTED;
  }

  int16_t rawAdc = pressure_2.readADC_SingleEnded(channel);
  float voltage = pressure_2.computeVolts(rawAdc);

  if (voltage < DISCONNECT_THRESH_P2) {
    faultPressStageB = true;
    return SENSOR_DISCONNECTED;
  }

  float pressure2 = ((voltage - MIN_VOLTAGE) / (MAX_VOLTAGE - MIN_VOLTAGE)) * MAX_PRESSURE;
  float pressure = pressure2 - 0.45f;
  faultPressStageB = false;
  return (pressure < 0.0f) ? 0.0f : pressure;
}

void updateSensors() {
  tempOil = readTemperature(PIN_T_OIL, faultOil);
  tempWinding = readTemperature(PIN_T_WIND, faultWind);
  tempTank = readTemperature(PIN_T_TANK, faultTank);
  p_stageA = readPressure_1(pressure_1);
  p_stageB = readPressure_2(2);

  acVoltages = readThreePhaseVoltage();
  acCurrents = readThreePhaseCurrent();

  //begin Remove these code block when Current sensor and voltage sensor is connected
  acCurrents.faultCur1 = true;  //Remove this line completely when Current sensor is now connected
  acCurrents.faultCur2 = true;   //Remove this line completely when Current sensor is now connected
  acCurrents.faultCur3 = true;   //Remove this line completely when Current sensor is now connected
  acVoltages.faultA = true;   //Remove this line completely when Voltage sensor is now connected
  acVoltages.faultB = true;   //Remove this line completely when Voltage sensor is now connected
  acVoltages.faultC = true;   //Remove this line completely when Voltage sensor is now connected
  //end

  powerData = calculatePower(acVoltages, acCurrents, powerFactor);

  readUARTData();
  if (newDataReceived) {
    parseData(rxBuffer);
    newDataReceived = false;
  }

  if (millis() - lastUartPacketTime > UART_TIMEOUT) {
    faultAmb = true;
    faultStageA = true;
    faultStageB = true;
    t_amb = SENSOR_DISCONNECTED;
    t_stageA = SENSOR_DISCONNECTED;
    t_stageB = SENSOR_DISCONNECTED;
  }
}

// ==========================================
// COMMUNICATIONS & PARSER
// ==========================================
void readUARTData() {
  static bool recvInProgress = false;
  static byte index = 0;
  char startMarker = '<';
  char endMarker = '>';

  while (SerialSender.available() > 0 && !newDataReceived) {
    char rc = SerialSender.read();

    if (recvInProgress) {
      if (rc != endMarker) {
        if (rc == startMarker) {
          index = 0;
        } else if (index < BUFFER_SIZE - 1) {
          rxBuffer[index++] = rc;
        }
      } else {
        rxBuffer[index] = '\0';
        recvInProgress = false;
        index = 0;
        newDataReceived = true;
      }
    } else if (rc == startMarker) {
      recvInProgress = true;
      index = 0;
    }
  }
}

void parseData(char *data) {
  char *token = strtok(data, ",");
  if (token != NULL) {
    float vAmb = atof(token);
    if (vAmb <= -990.0f) {
      faultAmb = true;
      t_amb = SENSOR_DISCONNECTED;
    } else {
      faultAmb = false;
      t_amb = vAmb;
    }

    token = strtok(NULL, ",");
    if (token != NULL) {
      float vStageA = atof(token);
      if (vStageA <= -990.0f) {
        faultStageA = true;
        t_stageA = SENSOR_DISCONNECTED;
      } else {
        faultStageA = false;
        t_stageA = vStageA;
      }

      token = strtok(NULL, ",");
      if (token != NULL) {
        float vStageB = atof(token);
        if (vStageB <= -990.0f) {
          faultStageB = true;
          t_stageB = SENSOR_DISCONNECTED;
        } else {
          faultStageB = false;
          t_stageB = vStageB;
        }
      }
    }
    lastUartPacketTime = millis();
  }
}

// ==========================================
// CONTROL & LOGIC
// ==========================================
void applyOutputs(SystemState state) {
  switch (state) {
    case STATE_OFF:
      digitalWrite(PIN_STAGE_A, LOW);
      digitalWrite(PIN_STAGE_B, LOW);
      break;
    case STATE_STAGE_A:
      digitalWrite(PIN_STAGE_A, HIGH);
      digitalWrite(PIN_STAGE_B, LOW);
      break;
    case STATE_STAGE_AB:
    case STATE_SAFE_MODE:
      digitalWrite(PIN_STAGE_A, HIGH);
      digitalWrite(PIN_STAGE_B, HIGH);
      break;
  }
}

void coolingLogic() {
  unsigned long now = millis();

  bool criticalFault = faultOil || faultWind || faultTank;

  if (criticalFault) {
    if (currentState != STATE_SAFE_MODE) {
      currentState = STATE_SAFE_MODE;
      lastStateChangeTime = now;
      applyOutputs(currentState);
      Serial.println(">>> CRITICAL FAULT DETECTED: Safe Mode Activated (Stage A+B Forced ON) <<<");
    }
    logTelemetry(now);
    return;
  }

  if (currentState == STATE_SAFE_MODE) {
    Serial.println(">>> ALL FAULTS CLEARED: Re-evaluating Thermal Demands <<<");

    if (tempOil >= T_OIL_HI || tempWinding >= T_WIND_HI) {
      currentState = STATE_STAGE_AB;
    } else if (tempOil >= T_OIL_START || tempWinding >= T_WIND_START || tempTank >= T_TANK_START) {
      currentState = STATE_STAGE_A;
    } else {
      currentState = STATE_OFF;
    }

    lastStateChangeTime = now;
    ignoreTimer = true;
    applyOutputs(currentState);
  }

  switch (currentState) {
    case STATE_OFF:
      if (ignoreTimer || (now - lastStateChangeTime >= T_OFF_MIN)) {
        if (tempOil >= T_OIL_START || tempWinding >= T_WIND_START || tempTank >= T_TANK_START) {
          currentState = STATE_STAGE_A;
          lastStateChangeTime = now;
          ignoreTimer = false;
          Serial.println(">>> STATE CHANGE: STAGE_A <<<");
        }
      }
      break;

    case STATE_STAGE_A:
      if (tempOil >= T_OIL_HI || tempWinding >= T_WIND_HI) {
        currentState = STATE_STAGE_AB;
        lastStateChangeTime = now;
        Serial.println(">>> STATE CHANGE: STAGE_A+B (Escalated) <<<");
      } else if (now - lastStateChangeTime >= T_RUN_MIN) {
        if (tempOil <= T_OIL_STOP && tempWinding <= T_WIND_STOP && tempTank <= T_TANK_STOP) {
          currentState = STATE_OFF;
          lastStateChangeTime = now;
          Serial.println(">>> STATE CHANGE: OFF (Cooling Complete) <<<");
        }
      }
      break;

    case STATE_STAGE_AB:
      if (tempOil <= (T_OIL_HI - HYS) && tempWinding <= (T_WIND_HI - HYS)) {
        currentState = STATE_STAGE_A;
        lastStateChangeTime = now;
        Serial.println(">>> STATE CHANGE: STAGE_A (De-escalated) <<<");
      }
      break;

    default:
      currentState = STATE_OFF;
      break;
  }

  applyOutputs(currentState);
  logTelemetry(now);
}

// ==========================================
// TELEMETRY LOGGING
// ==========================================
void logTelemetry(unsigned long now) {
  static unsigned long lastSerialLog = 0;
  static unsigned long lastSDLog = 0;

  String dateStr = getFormattedDate();
  String timeStr = getFormattedTime();

  if (now - lastSerialLog >= 1000) {
    lastSerialLog = now;

    Serial.printf("[%s %s] LOG | State: %s", dateStr.c_str(), timeStr.c_str(), stateNames[currentState]);

    Serial.print(" | Oil: ");
    if (faultOil) Serial.print("FAULT");
    else Serial.printf("%.2fC / %.2fF", tempOil, cToF(tempOil));

    Serial.print(" | Wind: ");
    if (faultWind) Serial.print("FAULT");
    else Serial.printf("%.2fC / %.2fF", tempWinding, cToF(tempWinding));

    Serial.print(" | Tank: ");
    if (faultTank) Serial.print("FAULT");
    else Serial.printf("%.2fC / %.2fF", tempTank, cToF(tempTank));

    Serial.print(" | Amb: ");
    if (faultAmb) Serial.print("FAULT");
    else Serial.printf("%.2fC / %.2fF", t_amb, cToF(t_amb));

    Serial.print(" | StageA: ");
    if (faultStageA) Serial.print("FAULT");
    else Serial.printf("%.2fC / %.2fF", t_stageA, cToF(t_stageA));

    Serial.print(" | StageB: ");
    if (faultStageB) Serial.print("FAULT");
    else Serial.printf("%.2fC / %.2fF", t_stageB, cToF(t_stageB));

    Serial.print(" | StageA_P: ");
    if (faultPressStageA) Serial.print("FAULT");
    else Serial.printf("%.2fPSI", p_stageA);

    Serial.print(" | StageB_P: ");
    if (faultPressStageB || isnan(p_stageB)) Serial.print("FAULT");
    else Serial.printf("%.2fPSI", p_stageB);

    Serial.print(" | VAC [A: ");
    if (acVoltages.faultA) Serial.print("FAULT");
    else Serial.printf("%.1fV", acVoltages.phaseA);
    Serial.print(", B: ");
    if (acVoltages.faultB) Serial.print("FAULT");
    else Serial.printf("%.1fV", acVoltages.phaseB);
    Serial.print(", C: ");
    if (acVoltages.faultC) Serial.print("FAULT");
    else Serial.printf("%.1fV", acVoltages.phaseC);
    Serial.print("]");

    Serial.print(" | IAC [I1: ");
    if (acCurrents.faultCur1) Serial.print("FAULT");
    else Serial.printf("%.1fA", acCurrents.cur1_rms);
    Serial.print(", I2: ");
    if (acCurrents.faultCur2) Serial.print("FAULT");
    else Serial.printf("%.1fA", acCurrents.cur2_rms);
    Serial.print(", C: ");
    if (acCurrents.faultCur3) Serial.print("FAULT");
    else Serial.printf("%.1fA", acCurrents.cur3_rms);
    Serial.print("]");

    Serial.printf(" | PWR [P: %.2fkW, Q: %.2fkVAR, S: %.2fkVA, PF: %.2f]\n",
                  powerData.realPower, powerData.reactivePower, powerData.apparentPower, powerFactor);

    Serial.println("================================================================================================");
    Serial.flush();
  }

  unsigned long sdIntervalMs = sdLogInterval * 1000UL;
  if (now - lastSDLog >= sdIntervalMs) {
    lastSDLog = now;

    if (digitalRead(SD_DETECT) == HIGH) {
      sdMounted = false;
      return;
    } else if (!sdMounted) {
      initSDCard();
    }

    String dateTimeStr = dateStr + " " + timeStr;
    String strOilTemp = faultOil ? "FAULT" : String(tempOil, 2);
    String strWindTemp = faultWind ? "FAULT" : String(tempWinding, 2);
    String strTankTemp = faultTank ? "FAULT" : String(tempTank, 2);
    String strAmbTemp = faultAmb ? "FAULT" : String(t_amb, 2);
    String strStageATemp = faultStageA ? "FAULT" : String(t_stageA, 2);
    String strStageBTemp = faultStageB ? "FAULT" : String(t_stageB, 2);

    String strStageAPress = faultPressStageA ? "FAULT" : String(p_stageA, 2);
    String strStageBPress = (faultPressStageB || isnan(p_stageB)) ? "FAULT" : String(p_stageB, 2);

    String strVoltA = acVoltages.faultA ? "FAULT" : String(acVoltages.phaseA, 1);
    String strVoltB = acVoltages.faultB ? "FAULT" : String(acVoltages.phaseB, 1);
    String strVoltC = acVoltages.faultC ? "FAULT" : String(acVoltages.phaseC, 1);

    String strCur1 = acCurrents.faultCur1 ? "FAULT" : String(acCurrents.cur1_rms, 2);
    String strCur2 = acCurrents.faultCur2 ? "FAULT" : String(acCurrents.cur2_rms, 2);
    String strCur3 = acCurrents.faultCur3 ? "FAULT" : String(acCurrents.cur3_rms, 2);

    String genRow = dateTimeStr + "," + String(stateNames[currentState]) + "," + strOilTemp + "," + strWindTemp + "," + strTankTemp + "," + strAmbTemp + "," + strStageATemp + "," + strStageBTemp + "," + strStageAPress + "," + strStageBPress + "," + strVoltA + "," + strVoltB + "," + strVoltC + "," + strCur1 + "," + strCur2 + "," + strCur3 + "," + String(powerData.realPower, 2) + "," + String(powerData.reactivePower, 2) + "," + String(powerData.apparentPower, 2) + "\n";
    appendFile("/gen.csv", genRow);

    String tempRow = dateTimeStr + "," + strOilTemp + "," + strWindTemp + "," + strTankTemp + "," + strAmbTemp + "," + strStageATemp + "," + strStageBTemp + "\n";
    appendFile("/temp.csv", tempRow);

    String presRow = dateTimeStr + "," + strStageAPress + "," + strStageBPress + "\n";
    appendFile("/pres.csv", presRow);

    String voltRow = dateTimeStr + "," + strVoltA + "," + strVoltB + "," + strVoltC + "\n";
    appendFile("/volt.csv", voltRow);

    String curRow = dateTimeStr + "," + strCur1 + "," + strCur2 + "," + strCur3 + "\n";
    appendFile("/cur.csv", curRow);

    String powerRow = dateTimeStr + "," + String(powerData.realPower, 2) + "," + String(powerData.reactivePower, 2) + "," + String(powerData.apparentPower, 2) + "," + String(powerFactor, 2) + "\n";
    appendFile("/power.csv", powerRow);
  }
}

void stream_to_Ethernet() {
  if (millis() - streamTimer >= 500) {
    streamTimer = millis();
    buildPacket(UNIQ_DEVICE_HEADER, tempOil, tempWinding, tempTank, t_amb, t_stageA, t_stageB, p_stageA, p_stageB, acVoltages.phaseA, acVoltages.phaseB, acVoltages.phaseC, acCurrents.cur1_rms, acCurrents.cur2_rms, acCurrents.cur3_rms, powerData.realPower, powerData.reactivePower, powerData.apparentPower, powerFactor);

    udp.writeTo(packet, packetSize, FPGA_IP, FPGA_PORT);
    udp.writeTo(packet, packetSize, SERVER_IP, SERVER_PORT);
  }
}

void coolingLogicControl() {
  if (overideCoolingLogic) {
    if (Stage_A_overide) {
      digitalWrite(PIN_STAGE_A, HIGH);
    } else {
      digitalWrite(PIN_STAGE_A, LOW);
    }

    if (Stage_B_overide) {
      digitalWrite(PIN_STAGE_B, HIGH);
    } else {
      digitalWrite(PIN_STAGE_B, LOW);
    }
  } else {
    coolingLogic();
  }
}