#pragma once

#include <Arduino.h>

constexpr char kFirmwareVersion[] = "0.1.0-stage1";
constexpr uint32_t kSerialBaudRate = 115200;

// Adjust this pin if the chosen ESP32-S3 board routes the built-in LED elsewhere.
constexpr uint8_t kStatusLedPin = LED_BUILTIN;
constexpr unsigned long kHeartbeatIntervalMs = 2000;
