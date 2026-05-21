#include <Arduino.h>

#include "config.h"

namespace {

unsigned long lastHeartbeatMs = 0;
bool ledState = false;

void setStatusLed(bool enabled) {
  digitalWrite(kStatusLedPin, enabled ? HIGH : LOW);
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

}  // namespace

void setup() {
  pinMode(kStatusLedPin, OUTPUT);
  setStatusLed(false);

  Serial.begin(kSerialBaudRate);
  delay(200);
  logStartupBanner();
}

void loop() {
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
