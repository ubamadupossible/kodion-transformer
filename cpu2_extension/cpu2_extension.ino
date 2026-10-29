


#include <Arduino.h>
#include <SPI.h>

// ==========================================
// PIN DEFINITIONS
// ==========================================
// MAX31855 SPI Thermocouple CS Pin for Ambient
#define PIN_T_AMB_CS 9  // Adjust CS pin to your hardware setup

// UART Serial Pins to Main ESP32
#define PIN_UART_TX 42  // Transmit Pin -> Main ESP32 RX (Pin 42)
#define PIN_UART_RX 41  // Receive Pin  <- Main ESP32 TX (Pin 41)

// Standard SPI Pins
#define SPI_SCK 12
#define SPI_MISO 13
#define SPI_MOSI 11

#define SENSOR_DISCONNECTED -999.0

// Hardware Serial Instance 2 for Main Board Communication
HardwareSerial SerialMain(2);

bool faultAmb = false;

// Non-blocking timer variables
unsigned long previousMillis = 0;
const unsigned long interval = 1000;  // 1 second interval

// Function Declarations
void pushTempValue();
float readTemperatureSPI(int csPin, bool &faultFlag);

// ==========================================
// MAX31855 SPI READING
// ==========================================
float readTemperatureSPI(int csPin, bool &faultFlag) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(csPin, LOW);

  uint32_t raw = 0;
  for (int i = 0; i < 4; i++) {
    raw <<= 8;
    raw |= SPI.transfer(0x00);
  }

  digitalWrite(csPin, HIGH);
  SPI.endTransaction();

  // Fault check: bit 16, or bus disconnected state (all 0s or all 1s)
  if ((raw & 0x00010000) || raw == 0x00000000 || raw == 0xFFFFFFFF) {
    faultFlag = true;
    return SENSOR_DISCONNECTED;
  }

  int16_t value = (raw >> 18) & 0x3FFF;
  if (raw & 0x80000000) {
    value |= 0xC000;  // Sign extension
  }

  float temp = value * 0.25;
  faultFlag = false;
  return temp - 3.0;  // Calibration offset
}

// ==========================================
// SETUP & LOOP
// ==========================================


void setup() {
  Serial.begin(115200);

  // Initialize pins and SPI
  pinMode(PIN_T_AMB_CS, OUTPUT);
  digitalWrite(PIN_T_AMB_CS, HIGH);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

  // Initialize UART2
  SerialMain.begin(115200, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);

  Serial.println("AMBIENT TEMPERATURE SENDER BOOTING...");

  // Delay to allow main ESP32 setup() to finish booting
  delay(3500); 

  // --- CLEAN SERIAL BUFFER CLEARING (NO RESTART) ---
  SerialMain.flush(); // Wait for any outgoing bytes to finish
  while (SerialMain.available() > 0) {
    SerialMain.read(); // Discard any garbage incoming bytes in RX buffer
  }

  Serial.println("AMBIENT TEMPERATURE SENDER READY & STREAMING.");
}

void loop() {
  pushTempValue();
}

void pushTempValue() {
  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;

    float ambientTemp = readTemperatureSPI(PIN_T_AMB_CS, faultAmb);

    // Format and send framed packet over UART: <VALUE>
    SerialMain.print('<');
    if (faultAmb || ambientTemp == SENSOR_DISCONNECTED) {
      SerialMain.print("-999.00");
      Serial.println("[TX -> Main] Ambient Temp: FAULT (-999)");
    } else {
      SerialMain.print(ambientTemp, 2);
      Serial.print("[TX -> Main] Ambient Temp: ");
      Serial.print(ambientTemp, 2);
      Serial.println(" C");
    }
    SerialMain.print('>'); // Clean delimiter end (without trailing \r or \n inside parser logic)
    SerialMain.flush();
  }
}