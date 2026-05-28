#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_SSD1306.h>
#include <AccelStepper.h>
#include <Wire.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"

namespace {

WebServer server(80);
#if STIRLING_OLED_DRIVER == STIRLING_OLED_DRIVER_SH1106
Adafruit_SH1106G oled(kOledWidth, kOledHeight, &Wire, -1);
#else
Adafruit_SSD1306 oled(kOledWidth, kOledHeight, &Wire, -1);
#endif
AccelStepper gaugeStepper(
  AccelStepper::FULL4WIRE,
  kStepperIn1Pin,
  kStepperIn3Pin,
  kStepperIn2Pin,
  kStepperIn4Pin);

enum class StepperTestState {
  ForwardSweep,
  PauseAtMax,
  ReverseSweep,
  PauseAtMin
};

StepperTestState stepperState = StepperTestState::ForwardSweep;
unsigned long stepperStateStartMs = 0;
const long kSweepSteps = (kStepperStepsPerRevolution * kGaugeSweepDegrees) / 360;
const float kForwardSpeedStepsPerSec =
  static_cast<float>(kSweepSteps) / (static_cast<float>(kForwardSweepMs) / 1000.0f);
const float kReverseSpeedStepsPerSec =
  static_cast<float>(kSweepSteps) / (static_cast<float>(kReverseSweepMs) / 1000.0f);
const char kStepperPresenceStatus[] = "not_verifiable_without_feedback";

unsigned long lastHeartbeatMs = 0;
bool ledState = false;
bool oledReady = false;

const __FlashStringHelper *stepperStateLabel(StepperTestState state) {
  switch (state) {
    case StepperTestState::ForwardSweep:
      return F("forward_sweep");
    case StepperTestState::PauseAtMax:
      return F("pause_at_max");
    case StepperTestState::ReverseSweep:
      return F("reverse_sweep");
    case StepperTestState::PauseAtMin:
      return F("pause_at_min");
  }
  return F("unknown");
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
  oled.println();

  oled.setTextSize(2);
  oled.print(F("LED "));
  oled.println(ledState ? F("ON") : F("OFF"));

  oled.setTextSize(1);
  oled.println();
  oled.print(F("IP "));
  oled.println(WiFi.softAPIP());
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
  String json = "{";
  json += "\"firmware\":\"" + String(kFirmwareVersion) + "\",";
  json += "\"uptime_ms\":" + String(millis()) + ",";
  json += "\"uptime\":\"" + formatUptime(millis()) + "\",";
  json += "\"heartbeat_ms\":" + String(kHeartbeatIntervalMs) + ",";
  json += "\"led_state\":\"" + String(ledState ? "on" : "off") + "\",";
  json += "\"stepper_state\":\"" + String(stepperStateLabel(stepperState)) + "\",";
  json += "\"stepper_driver_presence\":\"" + String(kStepperPresenceStatus) + "\",";
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
  appendItem(F("Stepper State"), String(stepperStateLabel(stepperState)));
  appendItem(F("Stepper Driver Presence"), String(kStepperPresenceStatus));
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
  Serial.print("Stepper sweep steps (270 deg): ");
  Serial.println(kSweepSteps);
  Serial.print("Stepper driver presence detection: ");
  Serial.println(kStepperPresenceStatus);
}

void startStepperTest() {
  gaugeStepper.setMaxSpeed(kReverseSpeedStepsPerSec + 200.0f);
  gaugeStepper.setSpeed(kForwardSpeedStepsPerSec);
  gaugeStepper.setCurrentPosition(0);
  stepperState = StepperTestState::ForwardSweep;
  stepperStateStartMs = millis();
  Serial.println("Stepper test started: 1.0s forward, 1.0s pause, 0.5s return, 1.0s pause.");
}

void updateStepperTest() {
  const unsigned long now = millis();
  const unsigned long elapsedMs = now - stepperStateStartMs;

  switch (stepperState) {
    case StepperTestState::ForwardSweep:
      gaugeStepper.setSpeed(kForwardSpeedStepsPerSec);
      gaugeStepper.runSpeed();
      if (elapsedMs >= kForwardSweepMs) {
        gaugeStepper.setCurrentPosition(kSweepSteps);
        stepperState = StepperTestState::PauseAtMax;
        stepperStateStartMs = now;
      }
      break;

    case StepperTestState::PauseAtMax:
      if (elapsedMs >= kPauseAtLimitMs) {
        stepperState = StepperTestState::ReverseSweep;
        stepperStateStartMs = now;
      }
      break;

    case StepperTestState::ReverseSweep:
      gaugeStepper.setSpeed(-kReverseSpeedStepsPerSec);
      gaugeStepper.runSpeed();
      if (elapsedMs >= kReverseSweepMs) {
        gaugeStepper.setCurrentPosition(0);
        stepperState = StepperTestState::PauseAtMin;
        stepperStateStartMs = now;
      }
      break;

    case StepperTestState::PauseAtMin:
      if (elapsedMs >= kPauseAtLimitMs) {
        stepperState = StepperTestState::ForwardSweep;
        stepperStateStartMs = now;
      }
      break;
  }
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
  setStatusLed(false);
  logStartupBanner();
  startAccessPoint();
  startStepperTest();
}

void loop() {
  server.handleClient();
  updateStepperTest();

  const unsigned long now = millis();

  if (now - lastHeartbeatMs >= kHeartbeatIntervalMs) {
    lastHeartbeatMs = now;
    setStatusLed(!ledState);

    Serial.print("Heartbeat ms=");
    Serial.print(now);
    Serial.print(", led=");
    Serial.println(ledState ? "ON" : "OFF");
  }
}
