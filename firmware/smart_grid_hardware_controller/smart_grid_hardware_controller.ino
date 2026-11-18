/*
 * ======================================================================================
 * PART 1: HARDWARE CONTROLLER & PROTECTION FIRMWARE (ESP32 SINGLE-CONTROLLER)
 * ======================================================================================
 * Architecture:
 *   - 100% Single Microcontroller (ESP32-WROOM-32) — No external Uno, LCD, BJT, or 10k SIP.
 *   - ADC1 Only (GPIOs 32, 33, 34, 35) — 100% Wi-Fi SoftAP compatible.
 *   - Direct Active-HIGH CMOS GPIO drive for Relay, Buzzer, and Triangle Signal Head.
 *   - Sanitized Credentials: Replace XXX / placeholder values before deployment.
 * ======================================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <HardwareSerial.h>

// ======================================================================================
// 1. DEDICATED PIN DEFINITIONS (ZERO CONFLICTS)
// ======================================================================================
// Analog Inputs (ADC1 Only)
#define PIN_ACS712_CURRENT    34   // ADC1_CH6: ACS712-05B Current Sensor OUT
#define PIN_POLE1_VOLT        35   // ADC1_CH7: Pole 1 Tap (15.0 cm = 1.00 km)
#define PIN_POLE2_VOLT        32   // ADC1_CH4: Pole 2 Tap (30.0 cm = 2.00 km)
#define PIN_POLE3_VOLT        33   // ADC1_CH5: Pole 3 Tap (45.0 cm = 3.00 km)

// Substation Actuators (Direct Active-HIGH Drive)
#define PIN_RELAY_GRID_TRIP   19   // Direct to 5V Relay Module IN (HIGH=Closed, LOW=Open)
#define PIN_BUZZER_ALARM      18   // Direct to Piezo Buzzer (+)

// Status & Line Indicator LEDs (via 330 ohm Resistors to GND)
#define PIN_LED_GRID_WHITE     5   // Grid Energized Indicator
#define PIN_LED_SYS_WHITE      2   // System Heartbeat (Onboard LED)
#define PIN_LED_SYS_GREEN      4   // Healthy Line Indicator
#define PIN_LED_EMERG_RED     21   // Emergency Fault Blinker
#define PIN_LED_LOAD_GREEN    22   // Consumer Power Active
#define PIN_LED_LOAD_RED      13   // Consumer Blackout Indicator

// Triangle Signal Head LEDs (via 330 ohm Resistors to GND)
#define PIN_SIG_BAR_2GREEN    25   // Base 2x Green LEDs (Healthy)
#define PIN_SIG_BAR_YELLOW    26   // Middle Yellow LED (Zone 2 Solid / Zone 3 Flash)
#define PIN_SIG_BAR_RED       27   // Apex Red LED (Zone 1 / Trip / Overload)

// Hardware UART1: GSM Modem
#define PIN_GSM_RX1           16   // ESP32 RX1 <- Modem TXD
#define PIN_GSM_TX1           17   // ESP32 TX1 -> Modem RXD

// ======================================================================================
// 2. SANITIZED NETWORK & CALIBRATION CONSTANTS (DEVICE DETAILS REDACTED)
// ======================================================================================
const char* AP_SSID            = "SmartGrid-XXX";        // Replace with your AP SSID
const char* AP_PASS            = "XXXXXXXX";             // Replace with 8+ char password
const char* ALERT_PHONE_NUMBER = "+91XXXXXXXXXX";        // Replace with target mobile number

// Random private IP configuration (replace with your desired local subnet if needed)
IPAddress AP_IP(10, 84, 27, 1);
IPAddress AP_GATEWAY(10, 84, 27, 1);
IPAddress AP_SUBNET(255, 255, 255, 0);

// 4-Resistor Impedance Ladder & ACS712 Calibration
const float ACS712_SENSITIVITY = 0.185f;                 // 185 mV/A for ACS712-05B
const float OVERCURRENT_THRESH = 35.0f;                  // mA trip threshold
const float FAULT_VOLT_LOW     = 0.35f;                  // Wire-break collapse threshold (V)

float ACS712_ZERO_OFFSET       = 2.50f;                  // Auto-calibrated at boot
unsigned long lastPowerToggleMs = 0;                     // 400ms relay contact settling timer

// ======================================================================================
// 3. GLOBAL STATE & TELEMETRY VARIABLES
// ======================================================================================
HardwareSerial GsmSerial(1);
WebServer server(80);

bool digitalGridPower = true;
bool breakerTripped   = false;
bool isHealthy        = true;

float current_mA      = 8.50f;
float v1              = 3.00f;
float v2              = 2.00f;
float v3              = 1.00f;
float faultDistCm     = 0.0f;
float faultDistKm     = 0.00f;

String faultCode      = "HEALTHY";
String faultDesc      = "GRID HEALTHY — FULL POWER AT 45.0 CM (3.00 KM)";
String signalBarState = "NORMAL_2G";
String lastSmsStatus  = "GSM Ready (Standby)";
String lastSmsCode    = "NONE";
int totalTrips        = 0;
int simFaultMode      = 0;

unsigned long lastSampleMs    = 0;
unsigned long lastHeartbeatMs = 0;
bool heartbeatState           = false;

// ======================================================================================
// 4. WEB PART REFERENCE (SERVED FROM PART 2: smart_grid_scada_dashboard.html)
// ======================================================================================
// Note: Paste the contents of smart_grid_scada_dashboard.html inside R"rawliteral(...)rawliteral"
// or include it via a header file `scada_dashboard.h` when compiling in Arduino IDE.
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Grid SCADA</title></head><body>
<h2>Smart Grid SCADA Controller Active</h2>
<p>Replace this minimal placeholder with the full contents of smart_grid_scada_dashboard.html.</p>
</body></html>
)rawliteral";

// ======================================================================================
// 5. GSM ALERT DISPATCHER & 32-SAMPLE ADC FILTERING
// ======================================================================================
void sendGsmAlert(const String& code, const String& details) {
  if (code == lastSmsCode && code != "CLEARED") return;
  lastSmsCode = code;

  GsmSerial.println("AT");
  delay(100);
  GsmSerial.println("AT+CMGF=1");
  delay(100);
  GsmSerial.print("AT+CMGS=\"");
  GsmSerial.print(ALERT_PHONE_NUMBER);
  GsmSerial.println("\"");
  delay(100);
  GsmSerial.print("[SMART GRID ALERT] ");
  GsmSerial.print(code);
  GsmSerial.print(": ");
  GsmSerial.print(details);
  GsmSerial.write(26);

  lastSmsStatus = "SMS Sent (" + code + ")";
}

float readAveragedVoltage(int pin) {
  uint32_t rawSum = 0;
  for (int i = 0; i < 32; i++) {
    rawSum += analogRead(pin);
    delayMicroseconds(50);
  }
  return ((float)rawSum / 32.0f / 4095.0f) * 3.3f;
}

void sampleSensors() {
  if (simFaultMode == 0) {
    float vAcs = readAveragedVoltage(PIN_ACS712_CURRENT);
    v1 = readAveragedVoltage(PIN_POLE1_VOLT);
    v2 = readAveragedVoltage(PIN_POLE2_VOLT);
    v3 = readAveragedVoltage(PIN_POLE3_VOLT);

    float deltaV = fabs(vAcs - ACS712_ZERO_OFFSET);
    if (deltaV < 0.020f) {
      current_mA = (digitalGridPower && !breakerTripped) ? 8.50f : 0.00f;
    } else {
      current_mA = (deltaV / ACS712_SENSITIVITY) * 1000.0f;
    }
  } else if (simFaultMode == 1) {
    v1 = 0.05f; v2 = 0.00f; v3 = 0.00f; current_mA = 24.5f;
  } else if (simFaultMode == 2) {
    v1 = 3.00f; v2 = 0.05f; v3 = 0.00f; current_mA = 21.0f;
  } else if (simFaultMode == 3) {
    v1 = 3.00f; v2 = 2.00f; v3 = 0.05f; current_mA = 19.5f;
  } else if (simFaultMode == 4) {
    v1 = 2.20f; v2 = 1.50f; v3 = 0.80f; current_mA = 42.0f;
  }
}

// ======================================================================================
// 6. FAULT-LATCHING STATE MACHINE & HARDWARE OUTPUTS
// ======================================================================================
void evaluateGridCondition() {
  if (!digitalGridPower) {
    breakerTripped = true;
    isHealthy = false;
    faultCode = "PWR_OFF";
    faultDesc = "GRID DE-ENERGIZED VIA SCADA DASHBOARD";
    faultDistCm = 0.0f;
    faultDistKm = 0.00f;
    signalBarState = "OFF";
    return;
  }

  if (millis() - lastPowerToggleMs < 400) return;
  if (breakerTripped) return;

  if (current_mA > OVERCURRENT_THRESH) {
    breakerTripped = true;
    totalTrips++;
    isHealthy = false;
    if (v1 < FAULT_VOLT_LOW) {
      faultCode = "FAULT_P1"; faultDistCm = 15.0f; faultDistKm = 1.00f;
      faultDesc = "SHORT-CIRCUIT AT POLE 1 (15.0 CM = 1.00 KM)";
      signalBarState = "ZONE1_RED";
    } else if (v2 < FAULT_VOLT_LOW) {
      faultCode = "FAULT_P2"; faultDistCm = 30.0f; faultDistKm = 2.00f;
      faultDesc = "SHORT-CIRCUIT AT POLE 2 (30.0 CM = 2.00 KM)";
      signalBarState = "ZONE2_YEL";
    } else if (v3 < FAULT_VOLT_LOW) {
      faultCode = "FAULT_P3"; faultDistCm = 45.0f; faultDistKm = 3.00f;
      faultDesc = "SHORT-CIRCUIT AT POLE 3 (45.0 CM = 3.00 KM)";
      signalBarState = "ZONE3_YEL";
    } else {
      faultCode = "OVERLOAD"; faultDistCm = 45.0f; faultDistKm = 3.00f;
      faultDesc = "OVERCURRENT TRIP (" + String(current_mA, 1) + " mA)";
      signalBarState = "ZONE1_RED";
    }
    sendGsmAlert(faultCode, faultDesc);
    return;
  }

  if (v1 < FAULT_VOLT_LOW) {
    breakerTripped = true; totalTrips++; isHealthy = false;
    faultCode = "SNAP_P1"; faultDistCm = 15.0f; faultDistKm = 1.00f;
    faultDesc = "LINE BREAK AT POLE 1 (15.0 CM = 1.00 KM)";
    signalBarState = "ZONE1_RED";
    sendGsmAlert(faultCode, faultDesc);
    return;
  }

  if (v2 < FAULT_VOLT_LOW) {
    breakerTripped = true; totalTrips++; isHealthy = false;
    faultCode = "SNAP_P2"; faultDistCm = 30.0f; faultDistKm = 2.00f;
    faultDesc = "LINE BREAK AT POLE 2 (30.0 CM = 2.00 KM)";
    signalBarState = "ZONE2_YEL";
    sendGsmAlert(faultCode, faultDesc);
    return;
  }

  if (v3 < FAULT_VOLT_LOW) {
    breakerTripped = true; totalTrips++; isHealthy = false;
    faultCode = "SNAP_P3"; faultDistCm = 45.0f; faultDistKm = 3.00f;
    faultDesc = "LINE BREAK AT POLE 3 (45.0 CM = 3.00 KM)";
    signalBarState = "ZONE3_YEL";
    sendGsmAlert(faultCode, faultDesc);
    return;
  }

  isHealthy = true;
  breakerTripped = false;
  faultCode = "HEALTHY";
  faultDesc = "GRID HEALTHY — FULL POWER AT 45.0 CM (3.00 KM)";
  faultDistCm = 0.0f;
  faultDistKm = 0.00f;
  signalBarState = "NORMAL_2G";
}

void updateOutputs() {
  if (digitalGridPower && !breakerTripped) {
    digitalWrite(PIN_RELAY_GRID_TRIP, HIGH);
    digitalWrite(PIN_BUZZER_ALARM, LOW);
  } else {
    digitalWrite(PIN_RELAY_GRID_TRIP, LOW);
    digitalWrite(PIN_BUZZER_ALARM, (!isHealthy && digitalGridPower && heartbeatState) ? HIGH : LOW);
  }

  digitalWrite(PIN_LED_GRID_WHITE, (digitalGridPower && !breakerTripped) ? HIGH : LOW);
  digitalWrite(PIN_LED_SYS_GREEN, isHealthy ? HIGH : LOW);
  digitalWrite(PIN_LED_EMERG_RED, (!isHealthy && heartbeatState) ? HIGH : LOW);
  digitalWrite(PIN_LED_LOAD_GREEN, (isHealthy && digitalGridPower && !breakerTripped) ? HIGH : LOW);
  digitalWrite(PIN_LED_LOAD_RED, (isHealthy && digitalGridPower && !breakerTripped) ? LOW : HIGH);

  if (signalBarState == "NORMAL_2G") {
    digitalWrite(PIN_SIG_BAR_2GREEN, HIGH);
    digitalWrite(PIN_SIG_BAR_YELLOW, LOW);
    digitalWrite(PIN_SIG_BAR_RED, LOW);
  } else if (signalBarState == "ZONE3_YEL") {
    digitalWrite(PIN_SIG_BAR_2GREEN, LOW);
    digitalWrite(PIN_SIG_BAR_YELLOW, heartbeatState ? HIGH : LOW);
    digitalWrite(PIN_SIG_BAR_RED, LOW);
  } else if (signalBarState == "ZONE2_YEL") {
    digitalWrite(PIN_SIG_BAR_2GREEN, LOW);
    digitalWrite(PIN_SIG_BAR_YELLOW, HIGH);
    digitalWrite(PIN_SIG_BAR_RED, LOW);
  } else if (signalBarState == "ZONE1_RED") {
    digitalWrite(PIN_SIG_BAR_2GREEN, LOW);
    digitalWrite(PIN_SIG_BAR_YELLOW, LOW);
    digitalWrite(PIN_SIG_BAR_RED, HIGH);
  } else {
    digitalWrite(PIN_SIG_BAR_2GREEN, LOW);
    digitalWrite(PIN_SIG_BAR_YELLOW, LOW);
    digitalWrite(PIN_SIG_BAR_RED, LOW);
  }
}

// ======================================================================================
// 7. REST API HANDLERS (UNIVERSAL ESP32 CORE COMPATIBLE)
// ======================================================================================
void handleRoot() { server.send(200, "text/html", INDEX_HTML); }

void handleData() {
  String json = "{";
  json += "\"healthy\":" + String(isHealthy ? "true" : "false") + ",";
  json += "\"grid_pwr\":" + String(digitalGridPower ? "1" : "0") + ",";
  json += "\"tripped\":" + String(breakerTripped ? "1" : "0") + ",";
  json += "\"code\":\"" + faultCode + "\",";
  json += "\"desc\":\"" + faultDesc + "\",";
  json += "\"dist_cm\":" + String(faultDistCm, 1) + ",";
  json += "\"dist_km\":" + String(faultDistKm, 2) + ",";
  json += "\"current_ma\":" + String(current_mA, 2) + ",";
  json += "\"v1\":" + String(v1, 2) + ",";
  json += "\"v2\":" + String(v2, 2) + ",";
  json += "\"v3\":" + String(v3, 2) + ",";
  json += "\"sig_bar\":\"" + signalBarState + "\",";
  json += "\"sms_code\":\"" + lastSmsCode + "\",";
  json += "\"sms_status\":\"" + lastSmsStatus + "\",";
  json += "\"trips\":" + String(totalTrips);
  json += "}";
  server.send(200, "application/json", json);
}

void handlePower() {
  if (server.hasArg("state")) {
    digitalGridPower = (server.arg("state").toInt() == 1);
    lastPowerToggleMs = millis();
    if (digitalGridPower) { breakerTripped = false; simFaultMode = 0; }
  }
  server.send(200, "text/plain", digitalGridPower ? "POWER_ON" : "POWER_OFF");
}

void handleTrip() {
  breakerTripped = true; isHealthy = false; totalTrips++;
  faultCode = "TRIPPED_MANUAL";
  faultDesc = "OPERATOR MANUAL TRIP VIA SCADA DASHBOARD";
  signalBarState = "ZONE1_RED";
  server.send(200, "text/plain", "BREAKER_TRIPPED");
}

void handleReset() {
  breakerTripped = false; digitalGridPower = true; simFaultMode = 0;
  lastPowerToggleMs = millis();
  isHealthy = true; faultCode = "HEALTHY";
  faultDesc = "GRID HEALTHY — FULL POWER AT 45.0 CM (3.00 KM)";
  faultDistCm = 0.0f; faultDistKm = 0.00f;
  signalBarState = "NORMAL_2G"; lastSmsCode = "CLEARED";
  server.send(200, "text/plain", "BREAKER_RESET_SUCCESS");
}

void handleSim() {
  if (server.hasArg("mode")) {
    simFaultMode = server.arg("mode").toInt();
    breakerTripped = false;
    isHealthy = true;
    if (simFaultMode == 0) {
      faultCode = "HEALTHY";
      faultDesc = "GRID HEALTHY — FULL POWER AT 45.0 CM (3.00 KM)";
      signalBarState = "NORMAL_2G";
      lastSmsCode = "CLEARED";
    }
  }
  server.send(200, "text/plain", "SIM_MODE_" + String(simFaultMode));
}

// ======================================================================================
// 8. SETUP & MAIN LOOP
// ======================================================================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_ACS712_CURRENT, INPUT);
  pinMode(PIN_POLE1_VOLT, INPUT);
  pinMode(PIN_POLE2_VOLT, INPUT);
  pinMode(PIN_POLE3_VOLT, INPUT);

  pinMode(PIN_RELAY_GRID_TRIP, OUTPUT);
  pinMode(PIN_BUZZER_ALARM, OUTPUT);
  pinMode(PIN_LED_GRID_WHITE, OUTPUT);
  pinMode(PIN_LED_SYS_WHITE, OUTPUT);
  pinMode(PIN_LED_SYS_GREEN, OUTPUT);
  pinMode(PIN_LED_EMERG_RED, OUTPUT);
  pinMode(PIN_LED_LOAD_GREEN, OUTPUT);
  pinMode(PIN_LED_LOAD_RED, OUTPUT);
  pinMode(PIN_SIG_BAR_2GREEN, OUTPUT);
  pinMode(PIN_SIG_BAR_YELLOW, OUTPUT);
  pinMode(PIN_SIG_BAR_RED, OUTPUT);

  analogSetAttenuation(ADC_11db);
  delay(100);
  float zeroSum = 0;
  for (int i = 0; i < 32; i++) {
    zeroSum += readAveragedVoltage(PIN_ACS712_CURRENT);
    delay(10);
  }
  ACS712_ZERO_OFFSET = zeroSum / 32.0f;
  lastPowerToggleMs = millis();

  digitalWrite(PIN_RELAY_GRID_TRIP, HIGH);
  digitalWrite(PIN_BUZZER_ALARM, LOW);
  digitalWrite(PIN_LED_SYS_GREEN, HIGH);
  digitalWrite(PIN_LED_EMERG_RED, LOW);

  GsmSerial.begin(9600, SERIAL_8N1, PIN_GSM_RX1, PIN_GSM_TX1);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  WiFi.softAP(AP_SSID, AP_PASS);

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/power", handlePower);
  server.on("/trip", handleTrip);
  server.on("/reset", handleReset);
  server.on("/sim", handleSim);
  server.begin();
}

void loop() {
  server.handleClient();
  unsigned long currentMs = millis();

  if (currentMs - lastHeartbeatMs >= 500) {
    lastHeartbeatMs = currentMs;
    heartbeatState = !heartbeatState;
    digitalWrite(PIN_LED_SYS_WHITE, heartbeatState ? HIGH : LOW);
  }

  if (currentMs - lastSampleMs >= 100) {
    lastSampleMs = currentMs;
    sampleSensors();
    evaluateGridCondition();
    updateOutputs();
  }
}