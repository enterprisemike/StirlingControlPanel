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

// OLED display (I2C). Many larger 1.3in modules use SH1106 rather than SSD1306.
#define STIRLING_OLED_DRIVER_SSD1306 1
#define STIRLING_OLED_DRIVER_SH1106 2

#ifndef STIRLING_OLED_DRIVER
#define STIRLING_OLED_DRIVER STIRLING_OLED_DRIVER_SSD1306
#endif

#ifndef STIRLING_OLED_WIDTH
#define STIRLING_OLED_WIDTH 128
#endif

#ifndef STIRLING_OLED_HEIGHT
#define STIRLING_OLED_HEIGHT 64
#endif

constexpr uint8_t kOledSdaPin = 8;
constexpr uint8_t kOledSclPin = 9;
constexpr uint8_t kOledI2cAddress = 0x3C;
constexpr uint8_t kOledWidth = STIRLING_OLED_WIDTH;
constexpr uint8_t kOledHeight = STIRLING_OLED_HEIGHT;
constexpr unsigned long kOledTestPatternHoldMs = 750;

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
