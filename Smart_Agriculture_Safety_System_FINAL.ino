#include <WiFi.h>
#include <WebSocketsServer.h>

// SMART SOLAR AGRICULTURAL WORKER SAFETY SYSTEM
// ESP32 + MQ135 + Proximity + Relay/2N2222 + Buzzer
// + Battery/Solar voltage monitoring + Local WebSocket dashboard

const char* WIFI_SSID = "iot2";
const char* WIFI_PASSWORD = "123456789";

WebSocketsServer webSocket(81);

// GPIO
#define MQ135_PIN       34
#define MASK_PIN        27
#define RELAY_DRIVER    26
#define BUZZER_PIN      25
#define BATTERY_PIN     35
#define SOLAR_PIN       32
#define STATUS_LED_PIN  2

// Proximity sensor: LOW = mask detected.
// Change to HIGH if your sensor works opposite.
#define MASK_DETECTED_LEVEL LOW

// User setting: 1500 and above is unsafe.
const int GAS_LIMIT = 1500;

// ADC
const float ADC_REFERENCE = 3.30f;
const float ADC_MAX = 4095.0f;

// Voltage sensor ratio.
// Common 0-25V modules are approximately 5:1.
// Calibrate with a multimeter if necessary.
const float BATTERY_SENSOR_RATIO = 5.00f;
const float SOLAR_SENSOR_RATIO   = 5.00f;

// User requested battery correction: add +3.50 V.
const float BATTERY_OFFSET = 3.50f;
const float SOLAR_OFFSET = 0.00f;

// Battery percentage map requested by user:
// <=3.30V = 0%
// 3.50V = 25%
// 3.70V = 50%
// 4.00V = 75%
// >=4.50V = 100%
const float BAT_0V   = 3.30f;
const float BAT_25V  = 3.50f;
const float BAT_50V  = 3.70f;
const float BAT_75V  = 4.00f;
const float BAT_100V = 4.50f;

bool maskWorn = false;
bool systemState = false;
bool gasAlarm = false;

int gasRaw = 0;
int maskRaw = 0;

float batteryVoltage = 0.0f;
float solarVoltage = 0.0f;
int batteryPercent = 0;

unsigned long lastRead = 0;
unsigned long lastSend = 0;

unsigned long totalRuntimeMs = 0;
unsigned long systemStartMs = 0;
bool previousSystemState = false;

unsigned long lastBuzzerToggle = 0;
bool buzzerState = false;

float readADCVoltage(int pin) {
  uint32_t total = 0;
  const int samples = 10;

  for (int i = 0; i < samples; i++) {
    total += analogRead(pin);
    delayMicroseconds(300);
  }

  float raw = (float)total / samples;
  return raw * ADC_REFERENCE / ADC_MAX;
}

float readBatteryVoltage() {
  float adcVoltage = readADCVoltage(BATTERY_PIN);
  return (adcVoltage * BATTERY_SENSOR_RATIO) + BATTERY_OFFSET;
}

float readSolarVoltage() {
  float adcVoltage = readADCVoltage(SOLAR_PIN);
  return (adcVoltage * SOLAR_SENSOR_RATIO) + SOLAR_OFFSET;
}

int calculateBatteryPercent(float v) {
  if (v <= BAT_0V) return 0;
  if (v >= BAT_100V) return 100;

  if (v < BAT_25V)
    return (int)((v - BAT_0V) * 25.0f / (BAT_25V - BAT_0V));

  if (v < BAT_50V)
    return 25 + (int)((v - BAT_25V) * 25.0f / (BAT_50V - BAT_25V));

  if (v < BAT_75V)
    return 50 + (int)((v - BAT_50V) * 25.0f / (BAT_75V - BAT_50V));

  return 75 + (int)((v - BAT_75V) * 25.0f / (BAT_100V - BAT_75V));
}

String runtimeString() {
  unsigned long ms = totalRuntimeMs;

  if (systemState)
    ms += millis() - systemStartMs;

  unsigned long sec = ms / 1000UL;
  unsigned long h = sec / 3600UL;
  unsigned long m = (sec % 3600UL) / 60UL;
  unsigned long s = sec % 60UL;

  char buf[20];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", h, m, s);
  return String(buf);
}

void updateRuntime() {
  if (systemState && !previousSystemState)
    systemStartMs = millis();

  if (!systemState && previousSystemState)
    totalRuntimeMs += millis() - systemStartMs;

  previousSystemState = systemState;
}

void updateBuzzer() {
  if (!gasAlarm) {
    buzzerState = false;
    digitalWrite(BUZZER_PIN, LOW);
    return;
  }

  if (millis() - lastBuzzerToggle >= 250) {
    lastBuzzerToggle = millis();
    buzzerState = !buzzerState;
    digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
  }
}

void updateOutputs() {
  // 2N2222 base is driven by GPIO26.
  // HIGH -> transistor ON -> relay IN pulled LOW -> typical active-LOW relay ON.
  digitalWrite(RELAY_DRIVER, systemState ? HIGH : LOW);
  digitalWrite(STATUS_LED_PIN, systemState ? HIGH : LOW);
}

String getAlert() {
  if (gasAlarm) return "GAS HIGH! SYSTEM STOPPED";
  if (!maskWorn) return "WEAR SAFETY MASK";
  return "SYSTEM SAFE";
}

void sendData() {
  String json = "{";

  json += "\"co2\":";
  json += String(gasRaw);
  json += ",";

  json += "\"mask\":";
  json += maskWorn ? "true" : "false";
  json += ",";

  json += "\"system\":";
  json += systemState ? "true" : "false";
  json += ",";

  json += "\"motor\":";
  json += systemState ? "true" : "false";
  json += ",";

  json += "\"gasAlarm\":";
  json += gasAlarm ? "true" : "false";
  json += ",";

  json += "\"battery\":";
  json += String(batteryPercent);
  json += ",";

  json += "\"voltage\":";
  json += String(batteryVoltage, 2);
  json += ",";

  json += "\"solar\":";
  json += String(solarVoltage, 2);
  json += ",";

  json += "\"runtime\":\"";
  json += runtimeString();
  json += "\",";

  json += "\"alert\":\"";
  json += getAlert();
  json += "\"";

  json += "}";

  webSocket.broadcastTXT(json);
  Serial.println(json);
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    Serial.print("WebSocket client connected: ");
    Serial.println(num);
    sendData();
  }

  if (type == WStype_DISCONNECTED) {
    Serial.print("WebSocket client disconnected: ");
    Serial.println(num);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(MASK_PIN, INPUT);
  pinMode(RELAY_DRIVER, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);

  digitalWrite(RELAY_DRIVER, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(STATUS_LED_PIN, LOW);

  analogReadResolution(12);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.println();
  Serial.println("====================================");
  Serial.println("SMART AGRICULTURE SAFETY SYSTEM");
  Serial.println("====================================");
  Serial.print("Connecting to WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected");
  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());
  Serial.println("WebSocket port: 81");

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  gasRaw = analogRead(MQ135_PIN);
  maskRaw = digitalRead(MASK_PIN);

  maskWorn = (maskRaw == MASK_DETECTED_LEVEL);
  gasAlarm = (gasRaw >= GAS_LIMIT);

  batteryVoltage = readBatteryVoltage();
  solarVoltage = readSolarVoltage();
  batteryPercent = calculateBatteryPercent(batteryVoltage);

  systemState = maskWorn && !gasAlarm;
  previousSystemState = false;

  updateRuntime();
  updateOutputs();

  Serial.println("System ready.");
}

void loop() {
  webSocket.loop();

  if (millis() - lastRead >= 200) {
    lastRead = millis();

    gasRaw = analogRead(MQ135_PIN);
    maskRaw = digitalRead(MASK_PIN);

    maskWorn = (maskRaw == MASK_DETECTED_LEVEL);
    gasAlarm = (gasRaw >= GAS_LIMIT);

    // System can run only when mask is worn AND gas is safe.
    systemState = maskWorn && !gasAlarm;

    batteryVoltage = readBatteryVoltage();
    solarVoltage = readSolarVoltage();
    batteryPercent = calculateBatteryPercent(batteryVoltage);

    updateRuntime();
    updateOutputs();
  }

  updateBuzzer();

  if (millis() - lastSend >= 1000) {
    lastSend = millis();
    sendData();
  }
}
