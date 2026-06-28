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

// Velocity hall sensor input configuration.
constexpr uint8_t kHallSensorPin = 4;
constexpr bool kHallSensorUsePullup = true;
constexpr bool kHallSensorActiveLow = true;
constexpr uint8_t kHallPulsesPerRevolution = 4;
// Sensor on 15T sprocket driving 20T wheel sprocket: shaft/wheel rpm = 20/15.
constexpr float kHallShaftToWheelRatio = 20.0f / 15.0f;
constexpr float kDriveWheelDiameterInches = 8.0f;
constexpr float kDriveWheelDiameterMeters = kDriveWheelDiameterInches * 0.0254f;
constexpr unsigned long kHallSampleIntervalMs = 250;
constexpr unsigned long kHallMinPulseGapUs = 1000;

// Battery voltage monitor (pre-buck) via resistor divider to ESP32 ADC.
constexpr uint8_t kBatteryAdcPin = 1;
constexpr unsigned long kBatterySampleIntervalMs = 500;
constexpr uint8_t kBatteryAdcSamples = 16;
// Using available values: top = 10k + 4.7k, bottom = 1k.
constexpr float kBatteryDividerTopOhms = 14700.0f;
constexpr float kBatteryDividerBottomOhms = 1000.0f;
constexpr float kBatteryAdcReferenceVolts = 3.3f;
constexpr float kBatteryCalibrationFactor = 1.1284f;

// MicroSD adapter (SPI) for audio, logs, and cumulative distance storage.
constexpr uint8_t kSdCardCsPin = 10;
constexpr uint8_t kSdCardSckPin = 11;
constexpr uint8_t kSdCardMosiPin = 12;
constexpr uint8_t kSdCardMisoPin = 13;
constexpr uint32_t kSdCardSpiFrequencyHz = 1000000;
constexpr char kSdCardTestFile[] = "/sd_test.txt";
constexpr char kSdCardDistanceFile[] = "/distance_km.txt";
