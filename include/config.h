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
constexpr uint32_t kI2cClockHz = 100000;
constexpr uint16_t kI2cTimeoutMs = 50;
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
constexpr unsigned long kHallMinPulseGapUs = 25000;

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
constexpr unsigned long kSdCardPowerUpDelayMs = 500;
constexpr unsigned long kSdCardRetryIntervalMs = 5000;
constexpr uint8_t kSdCardMountAttemptsPerSpeed = 3;
constexpr char kSdCardSoundsDir[] = "/sounds";
constexpr char kSdCardLogsDir[] = "/logs";
constexpr char kSdCardConfigDir[] = "/config";
constexpr char kSdCardSoundsReadmeFile[] = "/sounds/README.txt";
constexpr char kSdCardTestFile[] = "/sd_test.txt";
constexpr char kSdCardDistanceFile[] = "/distance_km.txt";
constexpr char kSdCardBootLogFile[] = "/logs/boot.log";
constexpr char kGpsLocationLogFile[] = "/logs/location.csv";

// GPS receiver UART. GPS TX is wired to the ESP32 board RX pin.
constexpr uint32_t kGpsBaudRate = 115200;
constexpr int8_t kGpsRxPin = RX;
constexpr int8_t kGpsTxPin = -1;
constexpr unsigned long kGpsBaudProbeIntervalMs = 8000;
constexpr unsigned long kGpsLocationLogIntervalMs = 10000;
constexpr unsigned long kGpsDisplayRefreshMs = 1000;

// DS3231 RTC on the same I2C bus as the OLED.
constexpr unsigned long kRtcSampleIntervalMs = 1000;
constexpr unsigned long kRtcGpsSyncIntervalMs = 3600000;
constexpr uint16_t kRtcMaxGpsDriftSeconds = 2;
constexpr int32_t kAustralianEasternStandardOffsetSeconds = 10L * 60L * 60L;
constexpr int32_t kAustralianEasternDaylightOffsetSeconds = 11L * 60L * 60L;
constexpr bool kAustralianEasternUseDaylightSavings = true;

// I2S class-D amplifier wiring.
constexpr uint8_t kAudioI2sLrcPin = 18;
constexpr uint8_t kAudioI2sBclkPin = 17;
constexpr uint8_t kAudioI2sDinPin = 16;
constexpr uint8_t kAudioAmpSdPin = 7;
constexpr bool kAudioAmpSdActiveHigh = true;
constexpr uint32_t kAudioSampleRateHz = 44100;
constexpr float kAudioDefaultVolume = 1.00f;
constexpr bool kAudioHallUseGeneratedNoise = true;
constexpr unsigned long kAudioNoiseBurstDurationMs = 220;
constexpr float kAudioNoiseBurstVolume = 0.85f;
constexpr bool kAudioStartupSoundEnabled = true;
constexpr char kAudioStartupStreamFile[] = "/sounds/TonyAudio.wav";
constexpr char kAudioStartupTestFile[] = "/sounds/TonyAudio.wav";
constexpr size_t kAudioPlaybackBufferBytes = 1024;
constexpr uint8_t kAudioStartupPlayCount = 4;
constexpr unsigned long kAudioStartupPlayIntervalMs = 1000;
constexpr unsigned long kAudioStartupTestDurationMs = 5000;
constexpr uint8_t kAudioStartupTestRateHz = 3;
