#pragma once

#include <Arduino.h>

constexpr char kFirmwareVersion[] = "0.1.0-stage1";
constexpr uint32_t kSerialBaudRate = 115200;
constexpr char kApSsid[] = "StirlingControlPanel";
constexpr char kApPassword[] = "Stirling123";
constexpr uint8_t kApChannel = 1;
constexpr bool kApHidden = false;
constexpr uint8_t kApMaxConnections = 2;

// Adjust this pin if the chosen ESP32-S3 board routes the built-in LED elsewhere.
constexpr uint8_t kStatusLedPin = LED_BUILTIN;
constexpr unsigned long kHeartbeatIntervalMs = 2000;

// Stepper bench-test settings (4-wire stepper via IN1..IN4 driver module).
constexpr uint8_t kStepperIn1Pin = 4;
constexpr uint8_t kStepperIn2Pin = 5;
constexpr uint8_t kStepperIn3Pin = 6;
constexpr uint8_t kStepperIn4Pin = 7;
constexpr long kStepperStepsPerRevolution = 2048;
constexpr long kGaugeSweepDegrees = 270;
constexpr unsigned long kForwardSweepMs = 1000;
constexpr unsigned long kPauseAtLimitMs = 1000;
constexpr unsigned long kReverseSweepMs = 500;
