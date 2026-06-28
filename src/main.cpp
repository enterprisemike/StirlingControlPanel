#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_SSD1306.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"

namespace {

WebServer server(80);
SPIClass sdSpi(FSPI);
#if STIRLING_OLED_DRIVER == STIRLING_OLED_DRIVER_SH1106
Adafruit_SH1106G oled(kOledWidth, kOledHeight, &Wire, -1);
#else
Adafruit_SSD1306 oled(kOledWidth, kOledHeight, &Wire, -1);
#endif
const char kMotorControlMode[] = "io_expander_planned";

volatile uint32_t hallPulseCount = 0;
volatile unsigned long hallLastPulseUs = 0;
uint32_t hallPulseCountSnapshot = 0;
uint32_t lastHeartbeatPulseCount = 0;
unsigned long lastHallSampleMs = 0;
float hallPulseHz = 0.0f;
float hallShaftRpm = 0.0f;
float hallSpeedKmh = 0.0f;
unsigned long lastBatterySampleMs = 0;
float batteryVoltage = 0.0f;
bool sdCardReady = false;
String sdCardStatus = "not_initialized";

unsigned long lastHeartbeatMs = 0;
bool ledState = false;
bool oledReady = false;

void IRAM_ATTR onHallPulse() {
  const unsigned long nowUs = micros();
  if ((nowUs - hallLastPulseUs) < kHallMinPulseGapUs) {
    return;
  }

  hallLastPulseUs = nowUs;
  ++hallPulseCount;
}

void scanI2cBus() {
  Serial.println("I2C scan start");

  uint8_t deviceCount = 0;
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    const uint8_t error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("I2C device found at 0x");
      if (address < 16) {
        Serial.print('0');
      }
      Serial.println(address, HEX);
      ++deviceCount;
    }
  }

  if (deviceCount == 0) {
    Serial.println("I2C scan found no devices");
  } else {
    Serial.print("I2C scan found devices: ");
    Serial.println(deviceCount);
  }
}

void runOledTestPattern() {
  if (!oledReady) {
    return;
  }

  oled.clearDisplay();
  oled.fillRect(0, 0, kOledWidth, kOledHeight, SH110X_WHITE);
  oled.display();
  delay(kOledTestPatternHoldMs);

  oled.clearDisplay();
  oled.drawRect(0, 0, kOledWidth, kOledHeight, SH110X_WHITE);
  oled.drawLine(0, 0, kOledWidth - 1, kOledHeight - 1, SH110X_WHITE);
  oled.drawLine(kOledWidth - 1, 0, 0, kOledHeight - 1, SH110X_WHITE);
  oled.display();
  delay(kOledTestPatternHoldMs);

  oled.clearDisplay();
  oled.display();
}

void renderOledStatus() {
  if (!oledReady) {
    return;
  }

  uint32_t pulseTotal = 0;
  noInterrupts();
  pulseTotal = hallPulseCount;
  interrupts();

  oled.clearDisplay();
  oled.setTextColor(SH110X_WHITE);

#if STIRLING_OLED_HEIGHT <= 32
  oled.setTextSize(2);
  oled.setCursor(0, 0);
  oled.print(F("LED "));
  oled.println(ledState ? F("ON") : F("OFF"));

  oled.setTextSize(1);
  oled.setCursor(0, 24);
  oled.print(F("IP "));
  oled.println(WiFi.softAPIP());
#else
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println(F("Stirling Panel"));
  oled.print(F("IP "));
  oled.println(WiFi.softAPIP());
  oled.print(F("BAT "));
  oled.print(batteryVoltage, 1);
  oled.print(F("V SD "));
  oled.println(sdCardReady ? F("OK") : F("ERR"));
  oled.print(F("SPD "));
  oled.print(hallSpeedKmh, 1);
  oled.println(F(" kmh"));
  oled.print(F("PLS "));
  oled.println(pulseTotal);
  oled.print(F("LED "));
  oled.println(ledState ? F("ON") : F("OFF"));
#endif

  oled.display();
}

void setStatusLed(bool enabled) {
  ledState = enabled;
  digitalWrite(kStatusLedPin, enabled ? HIGH : LOW);
  renderOledStatus();
}

void initOled() {
  Wire.begin(kOledSdaPin, kOledSclPin);
  scanI2cBus();

#if STIRLING_OLED_DRIVER == STIRLING_OLED_DRIVER_SH1106
  if (!oled.begin(kOledI2cAddress, true)) {
#else
  if (!oled.begin(SSD1306_SWITCHCAPVCC, kOledI2cAddress)) {
#endif
    Serial.println("OLED init failed");
    return;
  }

  oledReady = true;
  runOledTestPattern();
  renderOledStatus();
}

String formatUptime(unsigned long uptimeMs) {
  const unsigned long totalSeconds = uptimeMs / 1000;
  const unsigned long days = totalSeconds / 86400;
  const unsigned long hours = (totalSeconds % 86400) / 3600;
  const unsigned long minutes = (totalSeconds % 3600) / 60;
  const unsigned long seconds = totalSeconds % 60;

  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%lud %02lu:%02lu:%02lu", days, hours, minutes, seconds);
  return String(buffer);
}

String buildStatusJson() {
  uint32_t pulseTotal = 0;
  unsigned long lastPulseUs = 0;
  noInterrupts();
  pulseTotal = hallPulseCount;
  lastPulseUs = hallLastPulseUs;
  interrupts();

  unsigned long lastPulseMsAgo = 0;
  if (lastPulseUs > 0) {
    lastPulseMsAgo = (micros() - lastPulseUs) / 1000;
  }

  String json = "{";
  json += "\"firmware\":\"" + String(kFirmwareVersion) + "\",";
  json += "\"uptime_ms\":" + String(millis()) + ",";
  json += "\"uptime\":\"" + formatUptime(millis()) + "\",";
  json += "\"heartbeat_ms\":" + String(kHeartbeatIntervalMs) + ",";
  json += "\"led_state\":\"" + String(ledState ? "on" : "off") + "\",";
  json += "\"motor_control_mode\":\"" + String(kMotorControlMode) + "\",";
  json += "\"sd_ready\":" + String(sdCardReady ? "true" : "false") + ",";
  json += "\"sd_status\":\"" + sdCardStatus + "\",";
  json += "\"battery_voltage\":" + String(batteryVoltage, 2) + ",";
  json += "\"hall_total_pulses\":" + String(pulseTotal) + ",";
  json += "\"hall_last_pulse_ms_ago\":" + String(lastPulseMsAgo) + ",";
  json += "\"hall_pulse_hz\":" + String(hallPulseHz, 2) + ",";
  json += "\"hall_shaft_rpm\":" + String(hallShaftRpm, 2) + ",";
  json += "\"speed_kmh\":" + String(hallSpeedKmh, 2) + ",";
  json += "\"ap_ssid\":\"" + String(kApSsid) + "\",";
  json += "\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"station_count\":" + String(WiFi.softAPgetStationNum());
  json += "}";
  return json;
}

String buildDashboardPage() {
  String page;
  page.reserve(1600);
  auto appendItem = [&](const __FlashStringHelper *label, const String &value) {
    page += F("<div class='item'><span class='label'>");
    page += label;
    page += F("</span><span class='value'>");
    page += value;
    page += F("</span></div>");
  };

  page += F("<!doctype html><html lang='en'><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<meta http-equiv='refresh' content='5'>");
  page += F("<title>Stirling Control Panel</title>");
  page += F("<style>body{font-family:system-ui,sans-serif;background:#101318;color:#f3f6fb;margin:0;padding:24px;}");
  page += F(".card{max-width:720px;margin:0 auto;background:#171c24;border:1px solid #273142;border-radius:18px;padding:24px;box-shadow:0 12px 40px rgba(0,0,0,.35);}h1{margin:0 0 8px;font-size:1.8rem;}");
  page += F(".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin-top:20px;} .item{background:#222938;border-radius:14px;padding:14px;} .label{display:block;font-size:.78rem;opacity:.7;margin-bottom:6px;text-transform:uppercase;letter-spacing:.08em;} .value{font-size:1.1rem;font-weight:600;} a{color:#80c7ff;}</style></head><body><main class='card'>");
  page += F("<h1>Stirling Control Panel</h1>");
  page += F("<p>Access point dashboard for Stage 1 bring-up. This page is read-only for now.</p>");
  page += F("<div class='grid'>");
  appendItem(F("Firmware"), String(kFirmwareVersion));
  appendItem(F("Uptime"), formatUptime(millis()));
  appendItem(F("Heartbeat"), String(kHeartbeatIntervalMs) + F(" ms"));
  appendItem(F("LED State"), String(ledState ? "ON" : "OFF"));
  appendItem(F("Control Mode"), String(kMotorControlMode));
  appendItem(F("SD Card"), sdCardReady ? String(F("Ready")) : sdCardStatus);
  appendItem(F("Battery"), String(batteryVoltage, 2) + F(" V"));
  appendItem(F("Pulse Rate"), String(hallPulseHz, 2) + F(" Hz"));
  appendItem(F("Shaft Speed"), String(hallShaftRpm, 1) + F(" rpm"));
  appendItem(F("Estimated Speed"), String(hallSpeedKmh, 2) + F(" km/h"));
  appendItem(F("AP SSID"), String(kApSsid));
  appendItem(F("AP IP"), WiFi.softAPIP().toString());
  appendItem(F("Connected Clients"), String(WiFi.softAPgetStationNum()));
  page += F("<div class='item'><span class='label'>Status JSON</span><span class='value'><a href='/status.json'>/status.json</a></span></div>");
  page += F("</div><p style='margin-top:20px;opacity:.75'>Use this page to verify the ESP32-S3 is alive before sensors, display, and audio are added.</p></main></body></html>");
  return page;
}

void handleRoot() {
  server.send(200, "text/html", buildDashboardPage());
}

void handleStatusJson() {
  server.send(200, "application/json", buildStatusJson());
}

void logStartupBanner() {
  Serial.println();
  Serial.println("Stirling Control Panel - Stage 1");
  Serial.print("Firmware version: ");
  Serial.println(kFirmwareVersion);
  Serial.print("Debug baud: ");
  Serial.println(kSerialBaudRate);
  Serial.print("Status LED pin: ");
  Serial.println(kStatusLedPin);
  Serial.print("OLED SDA pin: ");
  Serial.println(kOledSdaPin);
  Serial.print("OLED SCL pin: ");
  Serial.println(kOledSclPin);
  Serial.print("OLED driver: ");
#if STIRLING_OLED_DRIVER == STIRLING_OLED_DRIVER_SH1106
  Serial.println("SH1106");
#else
  Serial.println("SSD1306");
#endif
  Serial.print("OLED I2C address: 0x");
  if (kOledI2cAddress < 16) {
    Serial.print('0');
  }
  Serial.println(kOledI2cAddress, HEX);
  Serial.print("OLED resolution: ");
  Serial.print(kOledWidth);
  Serial.print('x');
  Serial.println(kOledHeight);
  Serial.print("Hall pin: ");
  Serial.println(kHallSensorPin);
  Serial.print("Hall pulses per shaft rev: ");
  Serial.println(kHallPulsesPerRevolution);
  Serial.print("Hall shaft-to-wheel ratio: ");
  Serial.println(kHallShaftToWheelRatio, 4);
  Serial.print("Wheel diameter (in): ");
  Serial.println(kDriveWheelDiameterInches, 2);
  Serial.print("Battery ADC pin: ");
  Serial.println(kBatteryAdcPin);
  Serial.print("Battery divider ratio: ");
  const float dividerRatio =
    (kBatteryDividerTopOhms + kBatteryDividerBottomOhms) / kBatteryDividerBottomOhms;
  Serial.println(dividerRatio, 4);
  Serial.print("SD SPI pins (CS/SCK/MOSI/MISO): ");
  Serial.print(kSdCardCsPin);
  Serial.print('/');
  Serial.print(kSdCardSckPin);
  Serial.print('/');
  Serial.print(kSdCardMosiPin);
  Serial.print('/');
  Serial.println(kSdCardMisoPin);
}

bool runSdCardSelfTest() {
  File testFile = SD.open(kSdCardTestFile, FILE_WRITE);
  if (!testFile) {
    sdCardStatus = "test_open_failed";
    return false;
  }

  testFile.println("Stirling Control Panel SD test");
  testFile.close();

  testFile = SD.open(kSdCardTestFile, FILE_READ);
  if (!testFile) {
    sdCardStatus = "test_read_failed";
    return false;
  }

  const String content = testFile.readString();
  testFile.close();
  if (content.indexOf("SD test") < 0) {
    sdCardStatus = "test_verify_failed";
    return false;
  }

  if (!SD.exists(kSdCardDistanceFile)) {
    File distanceFile = SD.open(kSdCardDistanceFile, FILE_WRITE);
    if (!distanceFile) {
      sdCardStatus = "distance_file_failed";
      return false;
    }
    distanceFile.println("0.000");
    distanceFile.close();
  }

  sdCardStatus = "mounted_tested";
  return true;
}

void initSdCard() {
  sdSpi.begin(kSdCardSckPin, kSdCardMisoPin, kSdCardMosiPin, kSdCardCsPin);
  if (!SD.begin(kSdCardCsPin, sdSpi, kSdCardSpiFrequencyHz)) {
    sdCardReady = false;
    sdCardStatus = "mount_failed";
    Serial.println("SD card mount failed.");
    return;
  }

  sdCardReady = runSdCardSelfTest();
  Serial.print("SD card status: ");
  Serial.println(sdCardStatus);
}

void initBatterySense() {
  pinMode(kBatteryAdcPin, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(kBatteryAdcPin, ADC_11db);
  lastBatterySampleMs = millis();
  Serial.println("Battery voltage sensing initialized.");
}

void updateBatteryVoltage() {
  const unsigned long nowMs = millis();
  if ((nowMs - lastBatterySampleMs) < kBatterySampleIntervalMs) {
    return;
  }
  lastBatterySampleMs = nowMs;

  uint32_t rawSum = 0;
  for (uint8_t i = 0; i < kBatteryAdcSamples; ++i) {
    rawSum += static_cast<uint32_t>(analogRead(kBatteryAdcPin));
  }

  const float rawAvg = static_cast<float>(rawSum) / static_cast<float>(kBatteryAdcSamples);
  const float adcVolts = (rawAvg / 4095.0f) * kBatteryAdcReferenceVolts;
  const float dividerRatio =
    (kBatteryDividerTopOhms + kBatteryDividerBottomOhms) / kBatteryDividerBottomOhms;
  batteryVoltage = adcVolts * dividerRatio * kBatteryCalibrationFactor;

  renderOledStatus();
}

void initHallSensor() {
  pinMode(kHallSensorPin, kHallSensorUsePullup ? INPUT_PULLUP : INPUT);
  attachInterrupt(
    digitalPinToInterrupt(kHallSensorPin),
    onHallPulse,
    kHallSensorActiveLow ? FALLING : RISING);

  lastHallSampleMs = millis();
  Serial.println("Hall velocity sensor initialized.");
}

void updateHallVelocity() {
  const unsigned long nowMs = millis();
  const unsigned long elapsedMs = nowMs - lastHallSampleMs;
  if (elapsedMs < kHallSampleIntervalMs) {
    return;
  }

  noInterrupts();
  const uint32_t currentPulseCount = hallPulseCount;
  interrupts();

  const uint32_t deltaPulses = currentPulseCount - hallPulseCountSnapshot;
  hallPulseCountSnapshot = currentPulseCount;
  lastHallSampleMs = nowMs;

  if (elapsedMs == 0 || kHallPulsesPerRevolution == 0 || kHallShaftToWheelRatio <= 0.0f) {
    hallPulseHz = 0.0f;
    hallShaftRpm = 0.0f;
    hallSpeedKmh = 0.0f;
    return;
  }

  hallPulseHz = (static_cast<float>(deltaPulses) * 1000.0f) / static_cast<float>(elapsedMs);
  hallShaftRpm = (hallPulseHz * 60.0f) / static_cast<float>(kHallPulsesPerRevolution);

  const float wheelRpm = hallShaftRpm / kHallShaftToWheelRatio;
  const float speedMetersPerSecond = (wheelRpm * PI * kDriveWheelDiameterMeters) / 60.0f;
  hallSpeedKmh = speedMetersPerSecond * 3.6f;

  renderOledStatus();
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(kApSsid, kApPassword, kApChannel, kApHidden, kApMaxConnections);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status.json", HTTP_GET, handleStatusJson);
  server.begin();

  Serial.print("AP SSID: ");
  Serial.println(kApSsid);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  renderOledStatus();
}

}  // namespace

void setup() {
  pinMode(kStatusLedPin, OUTPUT);

  Serial.begin(kSerialBaudRate);
  delay(200);
  initOled();
  initHallSensor();
  initBatterySense();
  initSdCard();
  setStatusLed(false);
  logStartupBanner();
  startAccessPoint();
}

void loop() {
  server.handleClient();
  updateHallVelocity();
  updateBatteryVoltage();

  const unsigned long now = millis();

  if (now - lastHeartbeatMs >= kHeartbeatIntervalMs) {
    lastHeartbeatMs = now;
    setStatusLed(!ledState);

    noInterrupts();
    const uint32_t pulseTotal = hallPulseCount;
    interrupts();
    const uint32_t pulseDelta = pulseTotal - lastHeartbeatPulseCount;
    lastHeartbeatPulseCount = pulseTotal;

    Serial.print("Heartbeat ms=");
    Serial.print(now);
    Serial.print(", led=");
    Serial.print(ledState ? "ON" : "OFF");
    Serial.print(", speed_kmh=");
    Serial.print(hallSpeedKmh, 2);
    Serial.print(", battery_v=");
    Serial.print(batteryVoltage, 2);
    Serial.print(", sd=");
    Serial.print(sdCardStatus);
    Serial.print(", pulse_total=");
    Serial.print(pulseTotal);
    Serial.print(", pulse_delta=");
    Serial.println(pulseDelta);
  }
}
