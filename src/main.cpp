#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"

namespace {

WebServer server(80);
unsigned long lastHeartbeatMs = 0;
bool ledState = false;

void setStatusLed(bool enabled) {
  digitalWrite(kStatusLedPin, enabled ? HIGH : LOW);
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
}

}  // namespace

void setup() {
  pinMode(kStatusLedPin, OUTPUT);
  setStatusLed(false);

  Serial.begin(kSerialBaudRate);
  delay(200);
  logStartupBanner();
  startAccessPoint();
}

void loop() {
  server.handleClient();

  const unsigned long now = millis();

  if (now - lastHeartbeatMs >= kHeartbeatIntervalMs) {
    lastHeartbeatMs = now;
    ledState = !ledState;
    setStatusLed(ledState);

    Serial.print("Heartbeat ms=");
    Serial.print(now);
    Serial.print(", led=");
    Serial.println(ledState ? "ON" : "OFF");
  }
}
