#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <RTClib.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_SSD1306.h>
#include <SD.h>
#include <SPI.h>
#include <TinyGPSPlus.h>
#include <Wire.h>
#include <cstring>
#include <esp_heap_caps.h>
#include <driver/i2s.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"

namespace {

WebServer server(80);
SPIClass sdSpi(FSPI);
HardwareSerial gpsSerial(1);
RTC_DS3231 rtc;
TinyGPSPlus gps;
#if STIRLING_OLED_DRIVER == STIRLING_OLED_DRIVER_SH1106
Adafruit_SH1106G oled(kOledWidth, kOledHeight, &Wire, -1);
#else
Adafruit_SSD1306 oled(kOledWidth, kOledHeight, &Wire, -1);
#endif
const char kMotorControlMode[] = "io_expander_planned";
const uint32_t kGpsCandidateBaudRates[] = {115200, 9600, 38400, 57600, 4800};

volatile uint32_t hallPulseCount = 0;
volatile uint32_t hallRejectedPulseCount = 0;
volatile unsigned long hallLastPulseUs = 0;
uint32_t hallPulseCountSnapshot = 0;
uint32_t lastHeartbeatPulseCount = 0;
uint32_t lastAudioPulseCount = 0;
unsigned long lastHallSampleMs = 0;
float hallPulseHz = 0.0f;
float hallShaftRpm = 0.0f;
float hallSpeedKmh = 0.0f;
unsigned long lastBatterySampleMs = 0;
float batteryVoltage = 0.0f;
bool sdCardReady = false;
String sdCardStatus = "not_initialized";
uint32_t sdCardActiveSpiFrequencyHz = 0;
bool audioI2sReady = false;
String audioStatus = "not_initialized";
String startupStreamStatus = "not_run";
uint32_t audioPlayRequests = 0;
uint32_t audioPlayStarts = 0;
uint8_t startupClipPlayCount = 0;
unsigned long lastStartupClipPlayMs = 0;
unsigned long lastSdRetryMs = 0;
bool rtcReady = false;
bool rtcTimeValid = false;
bool rtcSetFromGps = false;
String rtcStatus = "not_initialized";
DateTime currentRtcTime(2000, 1, 1, 0, 0, 0);
unsigned long lastRtcSampleMs = 0;
unsigned long lastRtcGpsSyncMs = 0;
bool gpsReady = false;
bool gpsLocationValid = false;
String gpsStatus = "not_initialized";
double gpsLatitude = 0.0;
double gpsLongitude = 0.0;
double gpsAltitudeMeters = 0.0;
double gpsSpeedKmph = 0.0;
uint32_t gpsSatellites = 0;
double gpsHdop = 0.0;
unsigned long lastGpsLocationLogMs = 0;
unsigned long lastGpsDisplayRefreshMs = 0;
unsigned long lastGpsByteMs = 0;
uint32_t gpsBytesThisBoot = 0;
uint32_t gpsSentencesThisBoot = 0;
uint32_t gpsFailedChecksumsThisBoot = 0;
char gpsRawPreview[81] = "";
uint8_t gpsRawPreviewLength = 0;
uint8_t gpsBaudCandidateIndex = 0;
uint32_t gpsActiveBaudRate = kGpsBaudRate;
unsigned long lastGpsBaudProbeMs = 0;
uint32_t gpsSentencesAtLastBaudProbe = 0;
uint32_t gpsBytesAtLastBaudProbe = 0;

unsigned long lastHeartbeatMs = 0;
bool ledState = false;
bool oledReady = false;
String oledStatus = "not_initialized";

struct WavInfo {
  uint16_t audioFormat = 0;
  uint16_t channels = 0;
  uint32_t sampleRate = 0;
  uint16_t bitsPerSample = 0;
  uint32_t dataStart = 0;
  uint32_t dataSize = 0;
};

struct LoadedAudioClip {
  int16_t *samples = nullptr;
  size_t byteCount = 0;
  size_t frameCount = 0;
  uint32_t sampleRate = 0;
  unsigned long durationMs = 0;
  uint32_t sourceDataHash = 0;
  bool ready = false;
};

enum class AudioAssetLoadStage {
  LoadSteam,
  LoadStartup,
  PlayStartup,
  Done,
};

struct AudioLoadJob {
  File file;
  LoadedAudioClip *clip = nullptr;
  const char *path = nullptr;
  WavInfo wavInfo;
  int16_t *samples = nullptr;
  size_t sourceFrameBytes = 0;
  size_t outputSampleIndex = 0;
  uint32_t remainingBytes = 0;
  uint32_t sourceDataHash = 2166136261UL;
  bool active = false;
};

LoadedAudioClip startupClip;
LoadedAudioClip steamClip;
AudioLoadJob audioLoadJob;
AudioAssetLoadStage audioAssetLoadStage = AudioAssetLoadStage::LoadSteam;
bool audioPlaying = false;
bool startupClipPlayed = false;
const LoadedAudioClip *audioPlaybackClip = nullptr;
const char *audioPlaybackDoneStatus = "ram_ready";
bool audioPlaybackIsStartup = false;
bool audioPlaybackCountsAsHallStart = false;
bool audioPlaybackIsNoise = false;
size_t audioPlaybackOffsetBytes = 0;
uint32_t audioClockSampleRate = 0;
uint32_t audioNoiseFramesRemaining = 0;
uint32_t audioNoiseTotalFrames = 0;
uint32_t audioNoiseRandomState = 0xA53C9E27UL;

void renderOledStatus();

void setStatusLed(bool enabled);

void loadAudioAssetsAfterSdReady();

struct ScopedStatusLed {
  explicit ScopedStatusLed(bool enabled)
    : active(enabled) {
    if (active) {
      setStatusLed(true);
    }
  }

  ~ScopedStatusLed() {
    if (active) {
      setStatusLed(false);
    }
  }

  bool active = false;
};

void setAudioAmpEnabled(bool enabled) {
  const uint8_t level =
    (enabled == kAudioAmpSdActiveHigh) ? HIGH : LOW;
  digitalWrite(kAudioAmpSdPin, level);
}

bool readExact(File &file, uint8_t *buffer, size_t byteCount) {
  return file.read(buffer, byteCount) == byteCount;
}

uint16_t readLe16(const uint8_t *buffer) {
  return static_cast<uint16_t>(buffer[0]) |
    (static_cast<uint16_t>(buffer[1]) << 8);
}

uint32_t readLe32(const uint8_t *buffer) {
  return static_cast<uint32_t>(buffer[0]) |
    (static_cast<uint32_t>(buffer[1]) << 8) |
    (static_cast<uint32_t>(buffer[2]) << 16) |
    (static_cast<uint32_t>(buffer[3]) << 24);
}

uint32_t updateFnv1a32(uint32_t hash, const uint8_t *buffer, size_t byteCount) {
  for (size_t i = 0; i < byteCount; ++i) {
    hash ^= buffer[i];
    hash *= 16777619UL;
  }
  return hash;
}

bool skipWavChunk(File &file, uint32_t chunkSize) {
  const uint32_t paddedSize = chunkSize + (chunkSize & 1U);
  return file.seek(file.position() + paddedSize);
}

bool readWavInfo(File &file, WavInfo &info) {
  uint8_t header[12];
  if (!readExact(file, header, sizeof(header)) ||
      std::memcmp(header, "RIFF", 4) != 0 ||
      std::memcmp(header + 8, "WAVE", 4) != 0) {
    audioStatus = "wav_not_riff";
    return false;
  }

  bool foundFmt = false;
  bool foundData = false;

  while (file.available() >= 8) {
    uint8_t chunkHeader[8];
    if (!readExact(file, chunkHeader, sizeof(chunkHeader))) {
      break;
    }

    const uint32_t chunkSize = readLe32(chunkHeader + 4);
    if (std::memcmp(chunkHeader, "fmt ", 4) == 0) {
      if (chunkSize < 16) {
        audioStatus = "wav_bad_fmt";
        return false;
      }

      uint8_t fmt[16];
      if (!readExact(file, fmt, sizeof(fmt))) {
        audioStatus = "wav_fmt_read_failed";
        return false;
      }

      info.audioFormat = readLe16(fmt);
      info.channels = readLe16(fmt + 2);
      info.sampleRate = readLe32(fmt + 4);
      info.bitsPerSample = readLe16(fmt + 14);
      foundFmt = true;

      const uint32_t remainingFmtBytes = chunkSize - sizeof(fmt);
      if (remainingFmtBytes > 0 && !skipWavChunk(file, remainingFmtBytes)) {
        audioStatus = "wav_fmt_skip_failed";
        return false;
      }
    } else if (std::memcmp(chunkHeader, "data", 4) == 0) {
      info.dataStart = file.position();
      info.dataSize = chunkSize;
      foundData = true;
      if (!skipWavChunk(file, chunkSize)) {
        audioStatus = "wav_data_skip_failed";
        return false;
      }
    } else if (!skipWavChunk(file, chunkSize)) {
      audioStatus = "wav_chunk_skip_failed";
      return false;
    }

    if (foundFmt && foundData) {
      return true;
    }
  }

  audioStatus = foundFmt ? "wav_no_data" : "wav_no_fmt";
  return false;
}

// Scales signed 16-bit PCM samples in-place and clips to int16 range.
[[maybe_unused]] void scalePcm16BufferInPlace(int16_t *samples, size_t sampleCount, float gain) {
  if (samples == nullptr) {
    return;
  }

  for (size_t i = 0; i < sampleCount; ++i) {
    int32_t scaled = static_cast<int32_t>(samples[i] * gain);
    if (scaled > 32767) {
      scaled = 32767;
    } else if (scaled < -32768) {
      scaled = -32768;
    }
    samples[i] = static_cast<int16_t>(scaled);
  }
}

void initAudioI2s() {
  pinMode(kAudioAmpSdPin, OUTPUT);

  // Keep amplifier muted while I2S clocks and DMA are initialized.
  setAudioAmpEnabled(false);

  const i2s_config_t i2sConfig = {
    .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = static_cast<int>(kAudioSampleRateHz),
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = 0,
    .dma_buf_count = 12,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0,
  };

  const i2s_pin_config_t pinConfig = {
    .bck_io_num = static_cast<int>(kAudioI2sBclkPin),
    .ws_io_num = static_cast<int>(kAudioI2sLrcPin),
    .data_out_num = static_cast<int>(kAudioI2sDinPin),
    .data_in_num = I2S_PIN_NO_CHANGE,
  };

  const esp_err_t installResult = i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, nullptr);
  if (installResult != ESP_OK) {
    Serial.print("I2S driver install failed: ");
    Serial.println(static_cast<int>(installResult));
    return;
  }

  const esp_err_t pinResult = i2s_set_pin(I2S_NUM_0, &pinConfig);
  if (pinResult != ESP_OK) {
    Serial.print("I2S pin setup failed: ");
    Serial.println(static_cast<int>(pinResult));
    i2s_driver_uninstall(I2S_NUM_0);
    return;
  }

  i2s_zero_dma_buffer(I2S_NUM_0);
  audioI2sReady = true;
  audioStatus = "i2s_ready";
  setAudioAmpEnabled(true);
  Serial.print("Audio I2S pins LRC/BCLK/DIN/SD: ");
  Serial.print(kAudioI2sLrcPin);
  Serial.print('/');
  Serial.print(kAudioI2sBclkPin);
  Serial.print('/');
  Serial.print(kAudioI2sDinPin);
  Serial.print('/');
  Serial.println(kAudioAmpSdPin);
  Serial.println("Audio I2S initialized; amplifier enabled.");
}

void releaseLoadedClip(LoadedAudioClip &clip) {
  if (clip.samples != nullptr) {
    heap_caps_free(clip.samples);
  }
  clip = LoadedAudioClip{};
}

int16_t scalePcm16Sample(int16_t sample, float gain) {
  int32_t scaled = static_cast<int32_t>(sample * gain);
  if (scaled > 32767) {
    scaled = 32767;
  } else if (scaled < -32768) {
    scaled = -32768;
  }
  return static_cast<int16_t>(scaled);
}

bool loadWavFileToMemory(const char *path, LoadedAudioClip &clip, float volume) {
  if (!sdCardReady) {
    audioStatus = "sd_not_ready";
    return false;
  }

  ScopedStatusLed loadingLed(true);

  File wavFile = SD.open(path, FILE_READ);
  if (!wavFile) {
    audioStatus = "wav_open_failed";
    Serial.print("Audio WAV open failed: ");
    Serial.println(path);
    return false;
  }

  WavInfo wavInfo;
  if (!readWavInfo(wavFile, wavInfo)) {
    wavFile.close();
    Serial.print("Audio WAV parse failed: ");
    Serial.println(audioStatus);
    return false;
  }

  if (wavInfo.audioFormat != 1 || wavInfo.bitsPerSample != 16 ||
      (wavInfo.channels != 1 && wavInfo.channels != 2)) {
    wavFile.close();
    audioStatus = "wav_unsupported";
    Serial.print("Audio WAV unsupported format fmt/ch/bits: ");
    Serial.print(wavInfo.audioFormat);
    Serial.print('/');
    Serial.print(wavInfo.channels);
    Serial.print('/');
    Serial.println(wavInfo.bitsPerSample);
    return false;
  }

  if (!wavFile.seek(wavInfo.dataStart)) {
    wavFile.close();
    audioStatus = "wav_seek_failed";
    return false;
  }

  const size_t sourceFrameBytes = wavInfo.channels * sizeof(int16_t);
  const size_t frameCount = wavInfo.dataSize / sourceFrameBytes;
  const size_t outputByteCount = frameCount * 2 * sizeof(int16_t);
  int16_t *samples = static_cast<int16_t *>(heap_caps_malloc(
    outputByteCount,
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (samples == nullptr) {
    samples = static_cast<int16_t *>(heap_caps_malloc(outputByteCount, MALLOC_CAP_8BIT));
  }
  if (samples == nullptr) {
    wavFile.close();
    audioStatus = "ram_alloc_failed";
    Serial.print("Audio RAM allocation failed, bytes: ");
    Serial.println(outputByteCount);
    return false;
  }

  uint8_t readBuffer[kAudioPlaybackBufferBytes];
  size_t outputSampleIndex = 0;
  uint32_t remainingBytes = frameCount * sourceFrameBytes;
  uint32_t sourceDataHash = 2166136261UL;

  while (remainingBytes > 0) {
    size_t bytesToRead = min(static_cast<uint32_t>(sizeof(readBuffer)), remainingBytes);
    bytesToRead -= bytesToRead % sourceFrameBytes;
    if (bytesToRead == 0) {
      break;
    }

    const size_t bytesRead = wavFile.read(readBuffer, bytesToRead);
    if (bytesRead != bytesToRead) {
      heap_caps_free(samples);
      wavFile.close();
      audioStatus = "wav_read_failed";
      return false;
    }

    sourceDataHash = updateFnv1a32(sourceDataHash, readBuffer, bytesRead);

    const int16_t *sourceSamples = reinterpret_cast<const int16_t *>(readBuffer);
    const size_t framesRead = bytesRead / sourceFrameBytes;
    for (size_t frame = 0; frame < framesRead; ++frame) {
      if (wavInfo.channels == 1) {
        const int16_t sample = scalePcm16Sample(sourceSamples[frame], volume);
        samples[outputSampleIndex++] = sample;
        samples[outputSampleIndex++] = sample;
      } else {
        samples[outputSampleIndex++] = scalePcm16Sample(sourceSamples[frame * 2], volume);
        samples[outputSampleIndex++] = scalePcm16Sample(sourceSamples[frame * 2 + 1], volume);
      }
    }

    remainingBytes -= bytesRead;
  }

  wavFile.close();
  releaseLoadedClip(clip);
  clip.samples = samples;
  clip.byteCount = outputSampleIndex * sizeof(int16_t);
  clip.frameCount = outputSampleIndex / 2;
  clip.sampleRate = wavInfo.sampleRate;
  clip.durationMs = (clip.frameCount * 1000UL) / clip.sampleRate;
  clip.sourceDataHash = sourceDataHash;
  clip.ready = true;
  audioStatus = "ram_ready";

  Serial.print("Loaded WAV into RAM: ");
  Serial.print(path);
  Serial.print(" rate/ch/bits/duration_ms/bytes/hash: ");
  Serial.print(wavInfo.sampleRate);
  Serial.print('/');
  Serial.print(wavInfo.channels);
  Serial.print('/');
  Serial.print(wavInfo.bitsPerSample);
  Serial.print('/');
  Serial.print(clip.durationMs);
  Serial.print('/');
  Serial.print(clip.byteCount);
  Serial.print("/0x");
  Serial.println(clip.sourceDataHash, HEX);
  return true;
}

bool setAudioPlaybackClock(uint32_t sampleRate) {
  if (audioClockSampleRate == sampleRate) {
    return true;
  }

  const esp_err_t clockResult = i2s_set_clk(
    I2S_NUM_0,
    sampleRate,
    I2S_BITS_PER_SAMPLE_16BIT,
    I2S_CHANNEL_STEREO);
  if (clockResult != ESP_OK) {
    audioStatus = "i2s_clock_failed";
    Serial.print("Audio I2S clock setup failed: ");
    Serial.println(static_cast<int>(clockResult));
    return false;
  }

  audioClockSampleRate = sampleRate;
  return true;
}

bool startLoadedClipPlayback(
  const LoadedAudioClip &clip,
  const char *playStatus,
  const char *doneStatus,
  bool isStartup,
  bool countsAsHallStart) {
  if (!audioI2sReady) {
    audioStatus = "i2s_not_ready";
    return false;
  }
  if (!clip.ready) {
    audioStatus = "ram_clip_not_ready";
    return false;
  }
  if (!setAudioPlaybackClock(clip.sampleRate)) {
    return false;
  }

  setAudioAmpEnabled(true);
  audioPlaybackClip = &clip;
  audioPlaybackDoneStatus = doneStatus;
  audioPlaybackIsStartup = isStartup;
  audioPlaybackCountsAsHallStart = countsAsHallStart;
  audioPlaybackOffsetBytes = 0;
  audioPlaying = true;
  audioStatus = playStatus;
  renderOledStatus();
  return true;
}

uint32_t nextAudioNoiseRandom() {
  audioNoiseRandomState ^= audioNoiseRandomState << 13;
  audioNoiseRandomState ^= audioNoiseRandomState >> 17;
  audioNoiseRandomState ^= audioNoiseRandomState << 5;
  return audioNoiseRandomState;
}

bool startNoiseBurstPlayback() {
  if (!audioI2sReady) {
    audioStatus = "i2s_not_ready";
    return false;
  }
  if (!setAudioPlaybackClock(kAudioSampleRateHz)) {
    return false;
  }

  setAudioAmpEnabled(true);
  audioPlaybackClip = nullptr;
  audioPlaybackDoneStatus = "noise_ready";
  audioPlaybackIsStartup = false;
  audioPlaybackCountsAsHallStart = true;
  audioPlaybackIsNoise = true;
  audioPlaybackOffsetBytes = 0;
  audioNoiseTotalFrames = max(
    static_cast<uint32_t>(1),
    static_cast<uint32_t>((kAudioSampleRateHz * kAudioNoiseBurstDurationMs) / 1000UL));
  audioNoiseFramesRemaining = audioNoiseTotalFrames;
  audioPlaying = true;
  audioStatus = "playing_noise";
  renderOledStatus();
  return true;
}

bool requestAudioPlayback() {
  if (!audioI2sReady) {
    audioStatus = "i2s_not_ready";
    return false;
  }

  if (kAudioHallUseGeneratedNoise) {
    ++audioPlayRequests;
    if (audioPlaying) {
      if (audioPlaybackIsNoise) {
        return true;
      }

      audioPlaying = false;
      audioPlaybackOffsetBytes = 0;
      i2s_zero_dma_buffer(I2S_NUM_0);
      if (audioPlaybackIsStartup) {
        startupClipPlayed = true;
        startupStreamStatus = "startup_interrupted";
      }
    }

    return startNoiseBurstPlayback();
  }

  if (!steamClip.ready) {
    audioStatus = "ram_clip_not_ready";
    return false;
  }
  if (!setAudioPlaybackClock(steamClip.sampleRate)) {
    return false;
  }

  ++audioPlayRequests;
  if (audioPlaying) {
    if (audioPlaybackIsNoise || audioPlaybackClip == &steamClip) {
      return true;
    }

    audioPlaying = false;
    audioPlaybackOffsetBytes = 0;
    i2s_zero_dma_buffer(I2S_NUM_0);
    if (audioPlaybackIsStartup) {
      startupClipPlayed = true;
      startupStreamStatus = "startup_interrupted";
    }
  }

  return startLoadedClipPlayback(steamClip, "playing_ram", "ram_ready", false, true);
}

bool playLoadedClipBlocking(const LoadedAudioClip &clip, const char *doneStatus) {
    if (!audioI2sReady) {
      audioStatus = "i2s_not_ready";
      return false;
    }
    if (!clip.ready) {
      audioStatus = "ram_clip_not_ready";
      return false;
    }
    if (!setAudioPlaybackClock(clip.sampleRate)) {
      return false;
    }

    setAudioAmpEnabled(true);
    audioStatus = "playing_ram";
    renderOledStatus();

    size_t offsetBytes = 0;
    while (offsetBytes < clip.byteCount) {
      const size_t remainingBytes = clip.byteCount - offsetBytes;
      const size_t bytesToWrite = min(static_cast<size_t>(kAudioPlaybackBufferBytes), remainingBytes);
      size_t bytesWritten = 0;
      const esp_err_t writeResult = i2s_write(
        I2S_NUM_0,
        reinterpret_cast<const uint8_t *>(clip.samples) + offsetBytes,
        bytesToWrite,
        &bytesWritten,
        portMAX_DELAY);

      if (writeResult != ESP_OK || bytesWritten != bytesToWrite) {
        audioStatus = "i2s_write_failed";
        Serial.print("Audio blocking RAM playback stopped: ");
        Serial.println(audioStatus);
        renderOledStatus();
        return false;
      }

      offsetBytes += bytesWritten;
    }

    i2s_zero_dma_buffer(I2S_NUM_0);
    audioStatus = doneStatus;
    renderOledStatus();
    return true;
  }

bool streamWavFileFromSd(const char *path, float volume) {
  if (!sdCardReady) {
    audioStatus = "sd_not_ready";
    startupStreamStatus = audioStatus;
    return false;
  }
  if (!audioI2sReady) {
    audioStatus = "i2s_not_ready";
    startupStreamStatus = audioStatus;
    return false;
  }

  File wavFile = SD.open(path, FILE_READ);
  if (!wavFile) {
    audioStatus = "startup_stream_open_failed";
    startupStreamStatus = audioStatus;
    Serial.print("Startup stream WAV open failed: ");
    Serial.println(path);
    return false;
  }

  WavInfo wavInfo;
  if (!readWavInfo(wavFile, wavInfo)) {
    wavFile.close();
    startupStreamStatus = audioStatus;
    Serial.print("Startup stream WAV parse failed: ");
    Serial.println(audioStatus);
    return false;
  }

  if (wavInfo.audioFormat != 1 || wavInfo.bitsPerSample != 16 ||
      (wavInfo.channels != 1 && wavInfo.channels != 2)) {
    wavFile.close();
    audioStatus = "startup_stream_unsupported";
    startupStreamStatus = audioStatus;
    Serial.print("Startup stream unsupported format fmt/ch/bits: ");
    Serial.print(wavInfo.audioFormat);
    Serial.print('/');
    Serial.print(wavInfo.channels);
    Serial.print('/');
    Serial.println(wavInfo.bitsPerSample);
    return false;
  }

  if (!setAudioPlaybackClock(wavInfo.sampleRate)) {
    wavFile.close();
    startupStreamStatus = audioStatus;
    return false;
  }

  if (!wavFile.seek(wavInfo.dataStart)) {
    wavFile.close();
    audioStatus = "startup_stream_seek_failed";
    startupStreamStatus = audioStatus;
    return false;
  }

  Serial.print("Streaming startup WAV from SD: ");
  Serial.print(path);
  Serial.print(" rate/ch/bits/bytes: ");
  Serial.print(wavInfo.sampleRate);
  Serial.print('/');
  Serial.print(wavInfo.channels);
  Serial.print('/');
  Serial.print(wavInfo.bitsPerSample);
  Serial.print('/');
  Serial.println(wavInfo.dataSize);

  setAudioAmpEnabled(true);
  audioStatus = "streaming_startup_sd";
  startupStreamStatus = audioStatus;
  renderOledStatus();

  const size_t sourceFrameBytes = wavInfo.channels * sizeof(int16_t);
  uint32_t remainingBytes = wavInfo.dataSize - (wavInfo.dataSize % sourceFrameBytes);
  uint8_t readBuffer[kAudioPlaybackBufferBytes];
  int16_t stereoBuffer[kAudioPlaybackBufferBytes / sizeof(int16_t) * 2];

  while (remainingBytes > 0) {
    size_t bytesToRead = min(static_cast<uint32_t>(sizeof(readBuffer)), remainingBytes);
    bytesToRead -= bytesToRead % sourceFrameBytes;
    if (bytesToRead == 0) {
      break;
    }

    const size_t bytesRead = wavFile.read(readBuffer, bytesToRead);
    if (bytesRead != bytesToRead) {
      audioStatus = "startup_stream_read_failed";
      startupStreamStatus = audioStatus;
      break;
    }

    const void *writeBuffer = readBuffer;
    size_t bytesToWrite = bytesRead;

    if (wavInfo.channels == 1) {
      const int16_t *monoSamples = reinterpret_cast<const int16_t *>(readBuffer);
      const size_t monoSampleCount = bytesRead / sizeof(int16_t);
      for (size_t sampleIndex = 0; sampleIndex < monoSampleCount; ++sampleIndex) {
        const int16_t sample = scalePcm16Sample(monoSamples[sampleIndex], volume);
        stereoBuffer[sampleIndex * 2] = sample;
        stereoBuffer[sampleIndex * 2 + 1] = sample;
      }
      writeBuffer = stereoBuffer;
      bytesToWrite = monoSampleCount * 2 * sizeof(int16_t);
    } else {
      scalePcm16BufferInPlace(
        reinterpret_cast<int16_t *>(readBuffer),
        bytesRead / sizeof(int16_t),
        volume);
    }

    size_t bytesWritten = 0;
    const esp_err_t writeResult = i2s_write(
      I2S_NUM_0,
      writeBuffer,
      bytesToWrite,
      &bytesWritten,
      portMAX_DELAY);
    if (writeResult != ESP_OK || bytesWritten != bytesToWrite) {
      audioStatus = "startup_stream_write_failed";
      startupStreamStatus = audioStatus;
      break;
    }

    remainingBytes -= bytesRead;
  }

  wavFile.close();
  i2s_zero_dma_buffer(I2S_NUM_0);

  if (audioStatus == "streaming_startup_sd") {
    audioStatus = "startup_stream_done";
    startupStreamStatus = audioStatus;
    Serial.println("Startup stream WAV playback complete.");
  } else {
    startupStreamStatus = audioStatus;
    Serial.print("Startup stream WAV playback stopped: ");
    Serial.println(audioStatus);
  }

  renderOledStatus();
  return audioStatus == "startup_stream_done";
}

void serviceAudioPlayback() {
  if (audioPlaying && audioPlaybackIsNoise) {
    constexpr size_t kNoiseFramesPerBuffer = kAudioPlaybackBufferBytes / (2 * sizeof(int16_t));
    int16_t stereoBuffer[kNoiseFramesPerBuffer * 2];
    const size_t framesToWrite = min(
      static_cast<uint32_t>(kNoiseFramesPerBuffer),
      audioNoiseFramesRemaining);
    const int32_t maxAmplitude = static_cast<int32_t>(32767.0f * kAudioNoiseBurstVolume);

    for (size_t frame = 0; frame < framesToWrite; ++frame) {
      const uint32_t framesLeft = audioNoiseFramesRemaining - frame;
      const int32_t envelope = static_cast<int32_t>(
        (static_cast<uint64_t>(maxAmplitude) * framesLeft) / audioNoiseTotalFrames);
      const int32_t randomSample = static_cast<int32_t>(nextAudioNoiseRandom() & 0xFFFF) - 32768;
      const int16_t sample = static_cast<int16_t>((randomSample * envelope) / 32768);
      stereoBuffer[frame * 2] = sample;
      stereoBuffer[frame * 2 + 1] = sample;
    }

    size_t bytesWritten = 0;
    const size_t bytesToWrite = framesToWrite * 2 * sizeof(int16_t);
    const esp_err_t writeResult = i2s_write(
      I2S_NUM_0,
      stereoBuffer,
      bytesToWrite,
      &bytesWritten,
      portMAX_DELAY);

    if (writeResult != ESP_OK || bytesWritten != bytesToWrite) {
      audioPlaying = false;
      audioPlaybackIsNoise = false;
      audioStatus = "i2s_write_failed";
      Serial.print("Audio noise playback stopped: ");
      Serial.println(audioStatus);
      renderOledStatus();
      return;
    }

    if (audioNoiseFramesRemaining == audioNoiseTotalFrames && audioPlaybackCountsAsHallStart) {
      ++audioPlayStarts;
    }
    audioNoiseFramesRemaining -= framesToWrite;

    if (audioNoiseFramesRemaining == 0) {
      audioPlaying = false;
      audioPlaybackIsNoise = false;
      audioPlaybackCountsAsHallStart = false;
      audioStatus = "noise_ready";
      i2s_zero_dma_buffer(I2S_NUM_0);
      renderOledStatus();
    }
    return;
  }

  if (!audioPlaying || audioPlaybackClip == nullptr || !audioPlaybackClip->ready) {
    return;
  }

  const LoadedAudioClip &clip = *audioPlaybackClip;
  const size_t remainingBytes = clip.byteCount - audioPlaybackOffsetBytes;
  const size_t bytesToWrite = min(static_cast<size_t>(kAudioPlaybackBufferBytes), remainingBytes);
  size_t bytesWritten = 0;
  const esp_err_t writeResult = i2s_write(
    I2S_NUM_0,
    reinterpret_cast<const uint8_t *>(clip.samples) + audioPlaybackOffsetBytes,
    bytesToWrite,
    &bytesWritten,
    portMAX_DELAY);

  if (writeResult != ESP_OK || bytesWritten != bytesToWrite) {
    audioPlaying = false;
    audioPlaybackClip = nullptr;
    audioStatus = "i2s_write_failed";
    Serial.print("Audio RAM playback stopped: ");
    Serial.println(audioStatus);
    renderOledStatus();
    return;
  }

  if (audioPlaybackOffsetBytes == 0 && audioPlaybackCountsAsHallStart) {
    ++audioPlayStarts;
  }
  audioPlaybackOffsetBytes += bytesWritten;

  if (audioPlaybackOffsetBytes >= clip.byteCount) {
    const char *doneStatus = audioPlaybackDoneStatus;
    const bool wasStartup = audioPlaybackIsStartup;
    audioPlaying = false;
    audioPlaybackClip = nullptr;
    audioPlaybackDoneStatus = "ram_ready";
    audioPlaybackIsStartup = false;
    audioPlaybackCountsAsHallStart = false;
    audioPlaybackIsNoise = false;
    audioPlaybackOffsetBytes = 0;
    audioStatus = doneStatus;
    if (wasStartup) {
      startupStreamStatus = doneStatus;
    }
    i2s_zero_dma_buffer(I2S_NUM_0);
    renderOledStatus();
  }
}

void runAudioStartupTest() {
  if ((!kAudioHallUseGeneratedNoise && !steamClip.ready) || kAudioStartupTestRateHz == 0) {
    return;
  }

  const unsigned long intervalMs = 1000UL / kAudioStartupTestRateHz;
  const uint16_t playCount =
    (kAudioStartupTestDurationMs * kAudioStartupTestRateHz) / 1000UL;

  Serial.print("Audio startup test: ");
  Serial.print(playCount);
  Serial.print(" retriggers over ");
  Serial.print(kAudioStartupTestDurationMs);
  Serial.print(" ms at ");
  Serial.print(kAudioStartupTestRateHz);
  Serial.println(" Hz");

  for (uint16_t playIndex = 0; playIndex < playCount; ++playIndex) {
    const unsigned long windowStartMs = millis();
    requestAudioPlayback();

    while ((millis() - windowStartMs) < intervalMs) {
      serviceAudioPlayback();
      delay(1);
    }
  }

  audioStatus = audioPlaying ? "startup_test_done_playing" : "startup_test_done";
  renderOledStatus();
}

void handleHallAudioTriggers() {
  uint32_t pulseTotal = 0;
  noInterrupts();
  pulseTotal = hallPulseCount;
  interrupts();

  if (pulseTotal == lastAudioPulseCount) {
    return;
  }

  lastAudioPulseCount = pulseTotal;
  requestAudioPlayback();
}

void IRAM_ATTR onHallPulse() {
  const unsigned long nowUs = micros();
  if ((nowUs - hallLastPulseUs) < kHallMinPulseGapUs) {
    ++hallRejectedPulseCount;
    return;
  }

  hallLastPulseUs = nowUs;
  ++hallPulseCount;
}

bool probeI2cAddress(uint8_t address) {
  Wire.beginTransmission(address);
  const uint8_t error = Wire.endTransmission();

  Serial.print("I2C probe 0x");
  if (address < 16) {
    Serial.print('0');
  }
  Serial.print(address, HEX);
  Serial.print(": ");
  Serial.println(error == 0 ? "found" : "not found");
  return error == 0;
}

void probeExpectedI2cAddresses() {
  Serial.println("I2C targeted diagnostic probes");
  probeI2cAddress(kOledI2cAddress);
  probeI2cAddress(0x57);
  probeI2cAddress(0x68);
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

void recoverI2cBus() {
  pinMode(kOledSdaPin, INPUT_PULLUP);
  pinMode(kOledSclPin, INPUT_PULLUP);
  delay(10);

  if (digitalRead(kOledSdaPin) == HIGH && digitalRead(kOledSclPin) == HIGH) {
    return;
  }

  Serial.println("I2C bus appears busy; attempting bus recovery.");
  pinMode(kOledSclPin, OUTPUT_OPEN_DRAIN);
  digitalWrite(kOledSclPin, HIGH);

  for (uint8_t pulse = 0; pulse < 9 && digitalRead(kOledSdaPin) == LOW; ++pulse) {
    digitalWrite(kOledSclPin, LOW);
    delayMicroseconds(10);
    digitalWrite(kOledSclPin, HIGH);
    delayMicroseconds(10);
  }

  pinMode(kOledSdaPin, OUTPUT_OPEN_DRAIN);
  digitalWrite(kOledSdaPin, LOW);
  delayMicroseconds(10);
  digitalWrite(kOledSclPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(kOledSdaPin, HIGH);
  delayMicroseconds(10);

  pinMode(kOledSdaPin, INPUT_PULLUP);
  pinMode(kOledSclPin, INPUT_PULLUP);
  Serial.print("I2C recovery complete, SDA/SCL: ");
  Serial.print(digitalRead(kOledSdaPin));
  Serial.print('/');
  Serial.println(digitalRead(kOledSclPin));
}

String formatDateTimeUtc(const DateTime &dateTime) {
  char buffer[24];
  snprintf(
    buffer,
    sizeof(buffer),
    "%04u-%02u-%02uT%02u:%02u:%02uZ",
    dateTime.year(),
    dateTime.month(),
    dateTime.day(),
    dateTime.hour(),
    dateTime.minute(),
    dateTime.second());
  return String(buffer);
}

uint8_t firstSundayOfMonth(uint16_t year, uint8_t month) {
  const DateTime firstDay(year, month, 1, 0, 0, 0);
  return 1 + ((7 - firstDay.dayOfTheWeek()) % 7);
}

bool isAustralianEasternDaylightTimeUtc(const DateTime &utcTime) {
  if (!kAustralianEasternUseDaylightSavings) {
    return false;
  }

  const uint16_t year = utcTime.year();
  const uint32_t dstStartUtc = DateTime(
    year,
    10,
    firstSundayOfMonth(year, 10),
    2,
    0,
    0).unixtime() - kAustralianEasternStandardOffsetSeconds;
  const uint32_t dstEndUtc = DateTime(
    year,
    4,
    firstSundayOfMonth(year, 4),
    3,
    0,
    0).unixtime() - kAustralianEasternDaylightOffsetSeconds;
  const uint32_t unixTime = utcTime.unixtime();
  return unixTime >= dstStartUtc || unixTime < dstEndUtc;
}

const __FlashStringHelper *australianEasternTimeAbbrev(const DateTime &utcTime) {
  return isAustralianEasternDaylightTimeUtc(utcTime) ? F("AEDT") : F("AEST");
}

DateTime australianEasternTimeFromUtc(const DateTime &utcTime) {
  const uint32_t offsetSeconds = isAustralianEasternDaylightTimeUtc(utcTime)
    ? kAustralianEasternDaylightOffsetSeconds
    : kAustralianEasternStandardOffsetSeconds;
  return DateTime(utcTime.unixtime() + offsetSeconds);
}

String formatDateTimeAustralianEastern(const DateTime &utcTime) {
  const DateTime localTime = australianEasternTimeFromUtc(utcTime);
  char buffer[29];
  snprintf(
    buffer,
    sizeof(buffer),
    "%04u-%02u-%02uT%02u:%02u:%02u %s",
    localTime.year(),
    localTime.month(),
    localTime.day(),
    localTime.hour(),
    localTime.minute(),
    localTime.second(),
    isAustralianEasternDaylightTimeUtc(utcTime) ? "AEDT" : "AEST");
  return String(buffer);
}

String formatOledTime() {
  if (!rtcTimeValid) {
    return String(F("AET --:--:--"));
  }

  const DateTime localTime = australianEasternTimeFromUtc(currentRtcTime);
  char buffer[16];
  snprintf(
    buffer,
    sizeof(buffer),
    "%s %02u:%02u:%02u",
    isAustralianEasternDaylightTimeUtc(currentRtcTime) ? "AEDT" : "AEST",
    localTime.hour(),
    localTime.minute(),
    localTime.second());
  return String(buffer);
}

String formatGpsLocation() {
  if (!gpsLocationValid) {
    return String(F("GPS waiting"));
  }

  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%.5f,%.5f", gpsLatitude, gpsLongitude);
  return String(buffer);
}

void updateRtcTime() {
  const unsigned long nowMs = millis();
  if (!rtcReady || (nowMs - lastRtcSampleMs) < kRtcSampleIntervalMs) {
    return;
  }

  lastRtcSampleMs = nowMs;
  currentRtcTime = rtc.now();
  rtcTimeValid = !rtc.lostPower();
  if (!rtcTimeValid) {
    rtcStatus = "lost_power_waiting_gps";
  }
  renderOledStatus();
}

void initRtc() {
  if (!probeI2cAddress(0x68)) {
    rtcReady = false;
    rtcTimeValid = false;
    rtcStatus = "not_found";
    Serial.println("RTC not found at I2C address 0x68.");
    return;
  }

  if (!rtc.begin(&Wire)) {
    rtcReady = false;
    rtcTimeValid = false;
    rtcStatus = "not_found";
    Serial.println("RTC not found on I2C bus.");
    return;
  }

  rtcReady = true;
  const bool rtcLostPower = rtc.lostPower();
  currentRtcTime = rtc.now();
  rtcTimeValid = !rtcLostPower;
  rtcStatus = rtcLostPower ? "lost_power_waiting_gps" : "running";
  lastRtcSampleMs = millis();

  Serial.print("RTC initialized, status/time: ");
  Serial.print(rtcStatus);
  Serial.print(' ');
  Serial.println(rtcTimeValid ? formatDateTimeUtc(currentRtcTime) : String(F("unknown")));
}

void clearGpsRawPreview() {
  gpsRawPreview[0] = '\0';
  gpsRawPreviewLength = 0;
}

void startGpsSerial(uint32_t baudRate) {
  gpsSerial.end();
  gpsSerial.begin(baudRate, SERIAL_8N1, kGpsRxPin, kGpsTxPin);
  gpsActiveBaudRate = baudRate;
  lastGpsBaudProbeMs = millis();
  gpsSentencesAtLastBaudProbe = gpsSentencesThisBoot;
  gpsBytesAtLastBaudProbe = gpsBytesThisBoot;
  clearGpsRawPreview();

  Serial.print("GPS UART baud/RX/TX: ");
  Serial.print(gpsActiveBaudRate);
  Serial.print('/');
  Serial.print(kGpsRxPin);
  Serial.print('/');
  Serial.println(kGpsTxPin);
}

void initGps() {
  gpsReady = true;
  gpsStatus = "waiting_for_fix";
  startGpsSerial(kGpsCandidateBaudRates[gpsBaudCandidateIndex]);
}

void serviceGpsBaudProbe() {
  if (!gpsReady || gpsSentencesThisBoot > 0) {
    return;
  }

  const unsigned long nowMs = millis();
  if ((nowMs - lastGpsBaudProbeMs) < kGpsBaudProbeIntervalMs) {
    return;
  }

  gpsBaudCandidateIndex = (gpsBaudCandidateIndex + 1) %
    (sizeof(kGpsCandidateBaudRates) / sizeof(kGpsCandidateBaudRates[0]));
  gpsStatus = gpsBytesThisBoot > gpsBytesAtLastBaudProbe
    ? "baud_probe_garbled"
    : "baud_probe_no_data";
  gpsSentencesAtLastBaudProbe = gpsSentencesThisBoot;
  gpsBytesAtLastBaudProbe = gpsBytesThisBoot;
  startGpsSerial(kGpsCandidateBaudRates[gpsBaudCandidateIndex]);
}

bool gpsDateTimeIsUsable() {
  return gps.date.isValid() && gps.time.isValid() &&
    gps.date.age() < 2000 && gps.time.age() < 2000;
}

DateTime gpsDateTimeUtc() {
  return DateTime(
    gps.date.year(),
    gps.date.month(),
    gps.date.day(),
    gps.time.hour(),
    gps.time.minute(),
    gps.time.second());
}

void syncRtcFromGpsIfNeeded() {
  if (!rtcReady || !gpsDateTimeIsUsable()) {
    return;
  }

  const unsigned long nowMs = millis();
  if (rtcSetFromGps && (nowMs - lastRtcGpsSyncMs) < kRtcGpsSyncIntervalMs) {
    return;
  }

  const DateTime gpsTime = gpsDateTimeUtc();
  const DateTime rtcTime = rtc.now();
  const int64_t driftSeconds =
    static_cast<int64_t>(gpsTime.unixtime()) - static_cast<int64_t>(rtcTime.unixtime());
  const bool shouldSync = rtc.lostPower() || !rtcTimeValid || !rtcSetFromGps ||
    llabs(driftSeconds) > kRtcMaxGpsDriftSeconds;

  if (!shouldSync) {
    return;
  }

  rtc.adjust(gpsTime);
  currentRtcTime = gpsTime;
  rtcTimeValid = true;
  rtcSetFromGps = true;
  lastRtcGpsSyncMs = nowMs;
  rtcStatus = "synced_from_gps";

  Serial.print("RTC set from GPS UTC: ");
  Serial.println(formatDateTimeUtc(currentRtcTime));
  Serial.print("Local time: ");
  Serial.println(formatDateTimeAustralianEastern(currentRtcTime));
  renderOledStatus();
}

void ensureGpsLogHeader() {
  if (SD.exists(kGpsLocationLogFile)) {
    return;
  }

  File logFile = SD.open(kGpsLocationLogFile, FILE_WRITE);
  if (!logFile) {
    return;
  }

  logFile.println("millis,utc,latitude,longitude,altitude_m,speed_kmph,satellites,hdop");
  logFile.close();
}

void logGpsLocationIfNeeded() {
  if (!sdCardReady || !gpsLocationValid) {
    return;
  }

  const unsigned long nowMs = millis();
  if ((nowMs - lastGpsLocationLogMs) < kGpsLocationLogIntervalMs) {
    return;
  }

  ensureGpsLogHeader();
  File logFile = SD.open(kGpsLocationLogFile, FILE_APPEND);
  if (!logFile) {
    gpsStatus = "log_open_failed";
    return;
  }

  lastGpsLocationLogMs = nowMs;
  logFile.print(nowMs);
  logFile.print(',');
  logFile.print(rtcTimeValid ? formatDateTimeUtc(currentRtcTime) : String(F("unknown")));
  logFile.print(',');
  logFile.print(gpsLatitude, 6);
  logFile.print(',');
  logFile.print(gpsLongitude, 6);
  logFile.print(',');
  logFile.print(gpsAltitudeMeters, 1);
  logFile.print(',');
  logFile.print(gpsSpeedKmph, 2);
  logFile.print(',');
  logFile.print(gpsSatellites);
  logFile.print(',');
  logFile.println(gpsHdop, 2);
  logFile.close();
}

void serviceGps() {
  if (!gpsReady) {
    return;
  }

  bool decoded = false;
  while (gpsSerial.available() > 0) {
    const char gpsByte = static_cast<char>(gpsSerial.read());
    ++gpsBytesThisBoot;
    lastGpsByteMs = millis();
    if (gpsRawPreviewLength >= (sizeof(gpsRawPreview) - 1)) {
      memmove(gpsRawPreview, gpsRawPreview + 1, sizeof(gpsRawPreview) - 2);
      gpsRawPreviewLength = sizeof(gpsRawPreview) - 2;
    }
    if (gpsByte == '\r') {
      gpsRawPreview[gpsRawPreviewLength++] = '|';
    } else if (gpsByte == '\n') {
      gpsRawPreview[gpsRawPreviewLength++] = '/';
    } else if (gpsByte >= 32 && gpsByte <= 126) {
      gpsRawPreview[gpsRawPreviewLength++] = gpsByte;
    } else {
      gpsRawPreview[gpsRawPreviewLength++] = '.';
    }
    gpsRawPreview[gpsRawPreviewLength] = '\0';
    decoded = gps.encode(gpsByte) || decoded;
  }

  gpsSentencesThisBoot = gps.passedChecksum();
  gpsFailedChecksumsThisBoot = gps.failedChecksum();
  serviceGpsBaudProbe();

  if (!decoded) {
    if (gpsBytesThisBoot > 0 && gpsStatus == "waiting_for_fix") {
      gpsStatus = "receiving_no_fix";
    }
    return;
  }

  gpsSatellites = gps.satellites.isValid() ? gps.satellites.value() : 0;
  gpsHdop = gps.hdop.isValid() ? gps.hdop.hdop() : 0.0;

  if (gps.location.isValid()) {
    gpsLatitude = gps.location.lat();
    gpsLongitude = gps.location.lng();
    gpsAltitudeMeters = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
    gpsSpeedKmph = gps.speed.isValid() ? gps.speed.kmph() : 0.0;
    gpsLocationValid = true;
    gpsStatus = "fix";
    logGpsLocationIfNeeded();
  } else {
    gpsStatus = gpsSentencesThisBoot > 0 ? "nmea_no_fix" : "waiting_for_fix";
  }

  syncRtcFromGpsIfNeeded();

  const unsigned long nowMs = millis();
  if ((nowMs - lastGpsDisplayRefreshMs) >= kGpsDisplayRefreshMs) {
    lastGpsDisplayRefreshMs = nowMs;
    renderOledStatus();
  }
}

void renderOledStatus() {
  if (!oledReady) {
    return;
  }

  oled.clearDisplay();
  oled.setTextColor(SH110X_WHITE);

#if STIRLING_OLED_HEIGHT <= 32
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println(formatOledTime());
  oled.println(formatGpsLocation());
  oled.print(F("BAT "));
  oled.print(batteryVoltage, 1);
  oled.print(F("V SPD "));
  oled.print(hallSpeedKmh, 1);
#else
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println(F("Stirling Panel"));
  oled.println(formatOledTime());
  oled.println(formatGpsLocation());
  oled.print(F("BAT "));
  oled.print(batteryVoltage, 1);
  oled.print(F("V SD "));
  oled.println(sdCardReady ? F("OK") : F("ERR"));
  oled.print(F("SPD "));
  oled.print(hallSpeedKmh, 1);
  oled.println(F(" kmh"));
  oled.print(F("GPS "));
  oled.print(gpsLocationValid ? F("FIX") : F("WAIT"));
  oled.print(F(" SAT "));
  oled.println(gpsSatellites);
  oled.print(F("AUD "));
  if (audioStatus == "playing_ram" || audioStatus == "playing_noise") {
    oled.println(F("PLAY"));
  } else if (audioStatus == "ram_ready" || audioStatus == "noise_ready" || audioStatus == "startup_stream_done") {
    oled.println(F("OK"));
  } else {
    oled.println(F("ERR"));
  }
#endif

  oled.display();
}

void setStatusLed(bool enabled) {
  ledState = enabled;
  digitalWrite(kStatusLedPin, enabled ? HIGH : LOW);
  renderOledStatus();
}

void initOled() {
  Serial.println("Initializing I2C/OLED bus...");
  recoverI2cBus();
  Wire.begin(kOledSdaPin, kOledSclPin);
  Wire.setClock(kI2cClockHz);
  Wire.setTimeOut(kI2cTimeoutMs);
  const bool oledAddressFound = probeI2cAddress(kOledI2cAddress);
  oledStatus = oledAddressFound ? "address_found" : "address_not_found";
  probeExpectedI2cAddresses();

#if STIRLING_OLED_DRIVER == STIRLING_OLED_DRIVER_SH1106
  if (!oled.begin(kOledI2cAddress, true)) {
#else
  if (!oled.begin(SSD1306_SWITCHCAPVCC, kOledI2cAddress)) {
#endif
    Serial.println("OLED init failed");
    oledStatus = "begin_failed";
    return;
  }

  Serial.println("OLED initialized.");
  oledReady = true;
  oledStatus = "ready";
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
  uint32_t rejectedPulseTotal = 0;
  unsigned long lastPulseUs = 0;
  noInterrupts();
  pulseTotal = hallPulseCount;
  rejectedPulseTotal = hallRejectedPulseCount;
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
  json += "\"oled_ready\":" + String(oledReady ? "true" : "false") + ",";
  json += "\"oled_status\":\"" + oledStatus + "\",";
  json += "\"motor_control_mode\":\"" + String(kMotorControlMode) + "\",";
  json += "\"rtc_ready\":" + String(rtcReady ? "true" : "false") + ",";
  json += "\"rtc_status\":\"" + rtcStatus + "\",";
  json += "\"rtc_time_utc\":\"" + String(rtcTimeValid ? formatDateTimeUtc(currentRtcTime) : String(F("unknown"))) + "\",";
  json += "\"rtc_time_local\":\"" + String(rtcTimeValid ? formatDateTimeAustralianEastern(currentRtcTime) : String(F("unknown"))) + "\",";
  json += "\"rtc_timezone\":\"" + String(rtcTimeValid ? australianEasternTimeAbbrev(currentRtcTime) : F("AET")) + "\",";
  json += "\"rtc_set_from_gps\":" + String(rtcSetFromGps ? "true" : "false") + ",";
  json += "\"gps_ready\":" + String(gpsReady ? "true" : "false") + ",";
  json += "\"gps_status\":\"" + gpsStatus + "\",";
  json += "\"gps_baud\":" + String(gpsActiveBaudRate) + ",";
  json += "\"gps_bytes\":" + String(gpsBytesThisBoot) + ",";
  json += "\"gps_sentences\":" + String(gpsSentencesThisBoot) + ",";
  json += "\"gps_checksum_failures\":" + String(gpsFailedChecksumsThisBoot) + ",";
  json += "\"gps_last_byte_ms_ago\":" + String(lastGpsByteMs > 0 ? millis() - lastGpsByteMs : 0) + ",";
  json += "\"gps_raw_preview\":\"" + String(gpsRawPreview) + "\",";
  json += "\"gps_fix\":" + String(gpsLocationValid ? "true" : "false") + ",";
  json += "\"gps_latitude\":" + String(gpsLatitude, 6) + ",";
  json += "\"gps_longitude\":" + String(gpsLongitude, 6) + ",";
  json += "\"gps_altitude_m\":" + String(gpsAltitudeMeters, 1) + ",";
  json += "\"gps_speed_kmph\":" + String(gpsSpeedKmph, 2) + ",";
  json += "\"gps_satellites\":" + String(gpsSatellites) + ",";
  json += "\"gps_hdop\":" + String(gpsHdop, 2) + ",";
  json += "\"gps_log_file\":\"" + String(kGpsLocationLogFile) + "\",";
  json += "\"sd_ready\":" + String(sdCardReady ? "true" : "false") + ",";
  json += "\"sd_status\":\"" + sdCardStatus + "\",";
  json += "\"sd_spi_frequency_hz\":" + String(sdCardActiveSpiFrequencyHz) + ",";
  json += "\"audio_i2s_ready\":" + String(audioI2sReady ? "true" : "false") + ",";
  json += "\"audio_status\":\"" + audioStatus + "\",";
  json += "\"audio_hall_mode\":\"" + String(kAudioHallUseGeneratedNoise ? "generated_noise" : "wav_ram") + "\",";
  json += "\"audio_noise_burst_duration_ms\":" + String(kAudioNoiseBurstDurationMs) + ",";
  json += "\"audio_noise_burst_volume\":" + String(kAudioNoiseBurstVolume, 2) + ",";
  json += "\"startup_stream_status\":\"" + startupStreamStatus + "\",";
  json += "\"audio_clip_ready\":" + String(steamClip.ready ? "true" : "false") + ",";
  json += "\"audio_clip_duration_ms\":" + String(steamClip.durationMs) + ",";
  json += "\"audio_clip_bytes\":" + String(steamClip.byteCount) + ",";
  json += "\"audio_source_data_hash\":\"0x" + String(steamClip.sourceDataHash, HEX) + "\",";
  json += "\"audio_play_requests\":" + String(audioPlayRequests) + ",";
  json += "\"audio_play_starts\":" + String(audioPlayStarts) + ",";
  json += "\"audio_pins\":\"LRC=" + String(kAudioI2sLrcPin) + ",BCLK=" + String(kAudioI2sBclkPin) + ",DIN=" + String(kAudioI2sDinPin) + ",SD=" + String(kAudioAmpSdPin) + "\",";
  json += "\"audio_startup_stream_file\":\"" + String(kAudioStartupStreamFile) + "\",";
  json += "\"audio_hall_ram_file\":\"" + String(kAudioStartupTestFile) + "\",";
  json += "\"battery_voltage\":" + String(batteryVoltage, 2) + ",";
  json += "\"hall_total_pulses\":" + String(pulseTotal) + ",";
  json += "\"hall_rejected_pulses\":" + String(rejectedPulseTotal) + ",";
  json += "\"hall_min_pulse_gap_us\":" + String(kHallMinPulseGapUs) + ",";
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
  appendItem(F("OLED"), oledReady ? String(F("Ready")) : oledStatus);
  appendItem(F("Control Mode"), String(kMotorControlMode));
  appendItem(F("RTC"), rtcReady ? rtcStatus : String(F("Not found")));
  appendItem(F("Local Time"), rtcTimeValid ? formatDateTimeAustralianEastern(currentRtcTime) : String(F("Unknown")));
  appendItem(F("UTC Time"), rtcTimeValid ? formatDateTimeUtc(currentRtcTime) : String(F("Unknown")));
  appendItem(F("GPS"), gpsLocationValid ? String(F("Fix")) : gpsStatus);
  appendItem(F("GPS Baud"), String(gpsActiveBaudRate));
  appendItem(F("Location"), gpsLocationValid ? formatGpsLocation() : String(F("Waiting")));
  appendItem(F("GPS Detail"), String(gpsSatellites) + F(" sats / HDOP ") + String(gpsHdop, 2));
  appendItem(
    F("GPS Serial"),
    String(gpsBytesThisBoot) + F(" bytes / ") + String(gpsSentencesThisBoot) +
      F(" ok / ") + String(gpsFailedChecksumsThisBoot) + F(" bad"));
  appendItem(F("GPS Log"), String(kGpsLocationLogFile));
  appendItem(F("SD Card"), sdCardReady ? String(F("Ready")) : sdCardStatus);
  appendItem(F("SD SPI"), String(sdCardActiveSpiFrequencyHz) + F(" Hz"));
  appendItem(F("Audio"), audioStatus);
  appendItem(F("Hall Audio Mode"), kAudioHallUseGeneratedNoise ? String(F("Generated noise")) : String(F("WAV from RAM")));
  appendItem(F("Noise Burst"), String(kAudioNoiseBurstDurationMs) + F(" ms / ") + String(kAudioNoiseBurstVolume, 2));
  appendItem(F("Startup Stream Status"), startupStreamStatus);
  appendItem(
    F("Audio Clip"),
    steamClip.ready ? String(steamClip.durationMs) + F(" ms / ") + String(steamClip.byteCount) + F(" bytes") : String(F("Not loaded")));
  appendItem(F("Audio Hash"), String(F("0x")) + String(steamClip.sourceDataHash, HEX));
  appendItem(F("Audio Plays"), String(audioPlayRequests) + F(" req / ") + String(audioPlayStarts) + F(" starts"));
  appendItem(
    F("Audio Pins"),
    String(F("LRC ")) + kAudioI2sLrcPin + F(" / BCLK ") + kAudioI2sBclkPin +
      F(" / DIN ") + kAudioI2sDinPin + F(" / SD ") + kAudioAmpSdPin);
  appendItem(F("Startup Stream"), String(kAudioStartupStreamFile));
  appendItem(F("Hall RAM Sound"), String(kAudioStartupTestFile));
  appendItem(F("Battery"), String(batteryVoltage, 2) + F(" V"));
  appendItem(F("Pulse Rate"), String(hallPulseHz, 2) + F(" Hz"));
  appendItem(F("Pulse Debounce"), String(kHallMinPulseGapUs / 1000UL) + F(" ms"));
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
  Serial.print("Hall min pulse gap (us): ");
  Serial.println(kHallMinPulseGapUs);
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
  Serial.print("RTC status/time: ");
  Serial.print(rtcStatus);
  Serial.print('/');
  Serial.println(rtcTimeValid ? formatDateTimeUtc(currentRtcTime) : String(F("unknown")));
  Serial.print("GPS baud/RX/TX/status: ");
  Serial.print(gpsActiveBaudRate);
  Serial.print('/');
  Serial.print(kGpsRxPin);
  Serial.print('/');
  Serial.print(kGpsTxPin);
  Serial.print('/');
  Serial.println(gpsStatus);
  Serial.print("GPS log file: ");
  Serial.println(kGpsLocationLogFile);
  Serial.print("SD SPI pins (CS/SCK/MOSI/MISO): ");
  Serial.print(kSdCardCsPin);
  Serial.print('/');
  Serial.print(kSdCardSckPin);
  Serial.print('/');
  Serial.print(kSdCardMosiPin);
  Serial.print('/');
  Serial.println(kSdCardMisoPin);
  Serial.print("SD SPI frequency (Hz): ");
  Serial.println(sdCardActiveSpiFrequencyHz);
  Serial.print("Audio I2S pins (LRC/BCLK/DIN): ");
  Serial.print(kAudioI2sLrcPin);
  Serial.print('/');
  Serial.print(kAudioI2sBclkPin);
  Serial.print('/');
  Serial.println(kAudioI2sDinPin);
  Serial.print("Audio SD pin: ");
  Serial.println(kAudioAmpSdPin);
  Serial.print("Audio sample rate (Hz): ");
  Serial.println(kAudioSampleRateHz);
  Serial.print("Audio default volume: ");
  Serial.println(kAudioDefaultVolume, 2);
  Serial.print("Audio hall mode: ");
  Serial.println(kAudioHallUseGeneratedNoise ? "generated_noise" : "wav_ram");
  Serial.print("Audio noise burst duration/volume: ");
  Serial.print(kAudioNoiseBurstDurationMs);
  Serial.print('/');
  Serial.println(kAudioNoiseBurstVolume, 2);
  Serial.print("Audio startup sound enabled: ");
  Serial.println(kAudioStartupSoundEnabled ? "yes" : "no");
  Serial.print("Audio startup stream WAV: ");
  Serial.println(kAudioStartupStreamFile);
  Serial.print("Audio hall RAM WAV: ");
  Serial.println(kAudioStartupTestFile);
  Serial.print("Audio clip ready/duration_ms/bytes/hash: ");
  Serial.print(steamClip.ready ? "yes" : "no");
  Serial.print('/');
  Serial.print(steamClip.durationMs);
  Serial.print('/');
  Serial.print(steamClip.byteCount);
  Serial.print("/0x");
  Serial.println(steamClip.sourceDataHash, HEX);
  Serial.print("Audio status: ");
  Serial.println(audioStatus);
  Serial.print("Startup stream status: ");
  Serial.println(startupStreamStatus);
}

bool runSdCardSelfTest() {
  if (!SD.exists(kSdCardSoundsDir) && !SD.mkdir(kSdCardSoundsDir)) {
    sdCardStatus = "mkdir_sounds_failed";
    return false;
  }

  if (!SD.exists(kSdCardLogsDir) && !SD.mkdir(kSdCardLogsDir)) {
    sdCardStatus = "mkdir_logs_failed";
    return false;
  }

  if (!SD.exists(kSdCardConfigDir) && !SD.mkdir(kSdCardConfigDir)) {
    sdCardStatus = "mkdir_config_failed";
    return false;
  }

  if (!SD.exists(kSdCardSoundsReadmeFile)) {
    File soundsReadmeFile = SD.open(kSdCardSoundsReadmeFile, FILE_WRITE);
    if (!soundsReadmeFile) {
      sdCardStatus = "sounds_readme_failed";
      return false;
    }
    soundsReadmeFile.println("Place sound files for the Stirling Control Panel in this folder.");
    soundsReadmeFile.println("Example future uses: whistle, bell, chuff, and alerts.");
    soundsReadmeFile.close();
  }

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

  ensureGpsLogHeader();

  File bootLogFile = SD.open(kSdCardBootLogFile, FILE_APPEND);
  if (!bootLogFile) {
    sdCardStatus = "boot_log_failed";
    return false;
  }
  bootLogFile.print("boot_ms=");
  bootLogFile.print(millis());
  bootLogFile.print(", firmware=");
  bootLogFile.println(kFirmwareVersion);
  bootLogFile.close();

  sdCardStatus = "mounted_tested";
  return true;
}

void initSdCard() {
  sdCardReady = false;
  sdCardStatus = "mounting";
  sdCardActiveSpiFrequencyHz = 0;

  pinMode(kSdCardCsPin, OUTPUT);
  digitalWrite(kSdCardCsPin, HIGH);
  delay(kSdCardPowerUpDelayMs);

  sdSpi.begin(kSdCardSckPin, kSdCardMisoPin, kSdCardMosiPin, kSdCardCsPin);

  const uint32_t candidateFrequencies[] = {
    400000,
    kSdCardSpiFrequencyHz,
    4000000,
  };

  String lastFailureStatus = "mount_failed";

  for (const uint32_t frequencyHz : candidateFrequencies) {
    for (uint8_t attempt = 1; attempt <= kSdCardMountAttemptsPerSpeed; ++attempt) {
      Serial.print("Trying SD card SPI frequency/attempt: ");
      Serial.print(frequencyHz);
      Serial.print('/');
      Serial.println(attempt);

      if (!SD.begin(kSdCardCsPin, sdSpi, frequencyHz)) {
        SD.end();
        digitalWrite(kSdCardCsPin, HIGH);
        delay(100);
        continue;
      }

      sdCardActiveSpiFrequencyHz = frequencyHz;
      sdCardReady = runSdCardSelfTest();
      if (sdCardReady) {
        Serial.print("SD card mounted at SPI frequency (Hz): ");
        Serial.println(sdCardActiveSpiFrequencyHz);
        Serial.print("SD card status: ");
        Serial.println(sdCardStatus);
        return;
      }

      Serial.print("SD card self-test failed at SPI frequency (Hz): ");
      Serial.print(frequencyHz);
      Serial.print(", status: ");
      Serial.println(sdCardStatus);
      lastFailureStatus = sdCardStatus;
      SD.end();
      sdCardActiveSpiFrequencyHz = 0;
      digitalWrite(kSdCardCsPin, HIGH);
      delay(100);
    }
  }

  sdCardReady = false;
  sdCardStatus = lastFailureStatus;
  sdCardActiveSpiFrequencyHz = 0;
  Serial.println("SD card mount/self-test failed at all configured SPI frequencies.");
  Serial.print("SD card status: ");
  Serial.println(sdCardStatus);
}

void loadAudioAssetsAfterSdReady() {
  if (!sdCardReady) {
    return;
  }

  if (kAudioStartupSoundEnabled && !startupClip.ready) {
    if (loadWavFileToMemory(kAudioStartupStreamFile, startupClip, kAudioDefaultVolume)) {
      startupStreamStatus = "startup_ram_loaded";
    } else {
      startupStreamStatus = audioStatus;
    }
  }

  if (kAudioStartupSoundEnabled && startupClip.ready && !startupClipPlayed) {
    startupStreamStatus = playLoadedClipBlocking(startupClip, "startup_ram_done")
      ? "startup_ram_done"
      : audioStatus;
    startupClipPlayed = (startupStreamStatus == "startup_ram_done");
  }

  if (!kAudioHallUseGeneratedNoise && !steamClip.ready) {
    loadWavFileToMemory(kAudioStartupTestFile, steamClip, kAudioDefaultVolume);
  }
}

void resetAudioLoadJob() {
  if (audioLoadJob.file) {
    audioLoadJob.file.close();
  }
  if (audioLoadJob.samples != nullptr) {
    heap_caps_free(audioLoadJob.samples);
  }
  audioLoadJob = AudioLoadJob{};
  setStatusLed(false);
}

bool beginAudioLoadJob(const char *path, LoadedAudioClip &clip) {
  if (!sdCardReady) {
    audioStatus = "sd_not_ready";
    return false;
  }

  resetAudioLoadJob();
  setStatusLed(true);
  audioLoadJob.file = SD.open(path, FILE_READ);
  if (!audioLoadJob.file) {
    audioStatus = "wav_open_failed";
    Serial.print("Audio WAV open failed: ");
    Serial.println(path);
    return false;
  }

  if (!readWavInfo(audioLoadJob.file, audioLoadJob.wavInfo)) {
    resetAudioLoadJob();
    Serial.print("Audio WAV parse failed: ");
    Serial.println(audioStatus);
    return false;
  }

  if (audioLoadJob.wavInfo.audioFormat != 1 || audioLoadJob.wavInfo.bitsPerSample != 16 ||
      (audioLoadJob.wavInfo.channels != 1 && audioLoadJob.wavInfo.channels != 2)) {
    audioStatus = "wav_unsupported";
    Serial.print("Audio WAV unsupported format fmt/ch/bits: ");
    Serial.print(audioLoadJob.wavInfo.audioFormat);
    Serial.print('/');
    Serial.print(audioLoadJob.wavInfo.channels);
    Serial.print('/');
    Serial.println(audioLoadJob.wavInfo.bitsPerSample);
    resetAudioLoadJob();
    return false;
  }

  if (!audioLoadJob.file.seek(audioLoadJob.wavInfo.dataStart)) {
    audioStatus = "wav_seek_failed";
    resetAudioLoadJob();
    return false;
  }

  audioLoadJob.sourceFrameBytes = audioLoadJob.wavInfo.channels * sizeof(int16_t);
  const size_t frameCount = audioLoadJob.wavInfo.dataSize / audioLoadJob.sourceFrameBytes;
  const size_t outputByteCount = frameCount * 2 * sizeof(int16_t);
  audioLoadJob.samples = static_cast<int16_t *>(heap_caps_malloc(
    outputByteCount,
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (audioLoadJob.samples == nullptr) {
    audioLoadJob.samples = static_cast<int16_t *>(heap_caps_malloc(outputByteCount, MALLOC_CAP_8BIT));
  }
  if (audioLoadJob.samples == nullptr) {
    audioStatus = "ram_alloc_failed";
    Serial.print("Audio RAM allocation failed, bytes: ");
    Serial.println(outputByteCount);
    resetAudioLoadJob();
    return false;
  }

  audioLoadJob.clip = &clip;
  audioLoadJob.path = path;
  audioLoadJob.remainingBytes = frameCount * audioLoadJob.sourceFrameBytes;
  audioLoadJob.sourceDataHash = 2166136261UL;
  audioLoadJob.active = true;
  audioStatus = "loading_ram";
  Serial.print("Loading WAV into RAM in background: ");
  Serial.println(path);
  return true;
}

bool serviceAudioLoadJob() {
  if (!audioLoadJob.active) {
    return true;
  }

  uint8_t readBuffer[kAudioPlaybackBufferBytes];
  const size_t maxChunkBytes = min(static_cast<size_t>(kAudioPlaybackBufferBytes), static_cast<size_t>(512));
  size_t bytesToRead = min(static_cast<uint32_t>(maxChunkBytes), audioLoadJob.remainingBytes);
  bytesToRead -= bytesToRead % audioLoadJob.sourceFrameBytes;
  if (bytesToRead == 0) {
    bytesToRead = 0;
  } else {
    const size_t bytesRead = audioLoadJob.file.read(readBuffer, bytesToRead);
    if (bytesRead != bytesToRead) {
      audioStatus = "wav_read_failed";
      resetAudioLoadJob();
      return true;
    }

    audioLoadJob.sourceDataHash = updateFnv1a32(audioLoadJob.sourceDataHash, readBuffer, bytesRead);
    const int16_t *sourceSamples = reinterpret_cast<const int16_t *>(readBuffer);
    const size_t framesRead = bytesRead / audioLoadJob.sourceFrameBytes;
    for (size_t frame = 0; frame < framesRead; ++frame) {
      if (audioLoadJob.wavInfo.channels == 1) {
        const int16_t sample = scalePcm16Sample(sourceSamples[frame], kAudioDefaultVolume);
        audioLoadJob.samples[audioLoadJob.outputSampleIndex++] = sample;
        audioLoadJob.samples[audioLoadJob.outputSampleIndex++] = sample;
      } else {
        audioLoadJob.samples[audioLoadJob.outputSampleIndex++] =
          scalePcm16Sample(sourceSamples[frame * 2], kAudioDefaultVolume);
        audioLoadJob.samples[audioLoadJob.outputSampleIndex++] =
          scalePcm16Sample(sourceSamples[frame * 2 + 1], kAudioDefaultVolume);
      }
    }

    audioLoadJob.remainingBytes -= bytesRead;
  }

  if (audioLoadJob.remainingBytes > 0) {
    return false;
  }

  LoadedAudioClip *clip = audioLoadJob.clip;
  releaseLoadedClip(*clip);
  clip->samples = audioLoadJob.samples;
  clip->byteCount = audioLoadJob.outputSampleIndex * sizeof(int16_t);
  clip->frameCount = audioLoadJob.outputSampleIndex / 2;
  clip->sampleRate = audioLoadJob.wavInfo.sampleRate;
  clip->durationMs = (clip->frameCount * 1000UL) / clip->sampleRate;
  clip->sourceDataHash = audioLoadJob.sourceDataHash;
  clip->ready = true;
  audioLoadJob.samples = nullptr;

  Serial.print("Loaded WAV into RAM: ");
  Serial.print(audioLoadJob.path);
  Serial.print(" duration_ms/bytes/hash: ");
  Serial.print(clip->durationMs);
  Serial.print('/');
  Serial.print(clip->byteCount);
  Serial.print("/0x");
  Serial.println(clip->sourceDataHash, HEX);

  audioLoadJob.file.close();
  audioLoadJob = AudioLoadJob{};
  audioStatus = "ram_ready";
  setStatusLed(false);
  return true;
}

void serviceAudioAssetLoading() {
  if (audioPlaying) {
    return;
  }

  if (!sdCardReady || audioLoadJob.active) {
    if (audioLoadJob.active) {
      serviceAudioLoadJob();
    }
    return;
  }

  if (audioAssetLoadStage == AudioAssetLoadStage::LoadSteam) {
    if (kAudioHallUseGeneratedNoise) {
      audioStatus = "noise_ready";
      startupStreamStatus = kAudioStartupSoundEnabled ? startupStreamStatus : "startup_disabled";
      audioAssetLoadStage = kAudioStartupSoundEnabled
        ? AudioAssetLoadStage::LoadStartup
        : AudioAssetLoadStage::Done;
      return;
    }

    if (!steamClip.ready) {
      beginAudioLoadJob(kAudioStartupTestFile, steamClip);
      return;
    }
    if (!kAudioStartupSoundEnabled) {
      startupStreamStatus = "startup_disabled";
      audioAssetLoadStage = AudioAssetLoadStage::Done;
      return;
    }
    audioAssetLoadStage = AudioAssetLoadStage::LoadStartup;
  }

  if (audioAssetLoadStage == AudioAssetLoadStage::LoadStartup) {
    if (!startupClip.ready) {
      startupStreamStatus = beginAudioLoadJob(kAudioStartupStreamFile, startupClip)
        ? "startup_ram_loading"
        : audioStatus;
      return;
    }
    audioAssetLoadStage = AudioAssetLoadStage::PlayStartup;
  }

  if (audioAssetLoadStage == AudioAssetLoadStage::PlayStartup) {
    if (startupClipPlayCount < kAudioStartupPlayCount) {
      const unsigned long nowMs = millis();
      if (startupClipPlayCount > 0 &&
          (nowMs - lastStartupClipPlayMs) < kAudioStartupPlayIntervalMs) {
        return;
      }

      if (!startLoadedClipPlayback(
          startupClip,
          "playing_startup_ram",
          "startup_ram_ready",
          true,
          false)) {
        startupStreamStatus = audioStatus;
        startupClipPlayed = true;
        audioAssetLoadStage = AudioAssetLoadStage::Done;
        return;
      }

      ++startupClipPlayCount;
      lastStartupClipPlayMs = nowMs;
      startupStreamStatus = "playing_startup_ram";
      return;
    }

    startupClipPlayed = true;
    startupStreamStatus = "startup_sequence_done";
    audioAssetLoadStage = AudioAssetLoadStage::Done;
  }
}

void retrySdCardIfNeeded() {
  if (sdCardReady) {
    return;
  }

  const unsigned long nowMs = millis();
  if ((nowMs - lastSdRetryMs) < kSdCardRetryIntervalMs) {
    return;
  }

  lastSdRetryMs = nowMs;
  Serial.println("Retrying SD card initialization...");
  initSdCard();
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
  Serial.println();
  Serial.println("Stirling Control Panel boot start");
  initOled();
  initRtc();
  initGps();
  initHallSensor();
  initBatterySense();
  initSdCard();
  initAudioI2s();
  logStartupBanner();
  setStatusLed(false);
  startAccessPoint();
}

void loop() {
  server.handleClient();
  serviceGps();
  handleHallAudioTriggers();
  serviceAudioPlayback();
  retrySdCardIfNeeded();
  serviceAudioAssetLoading();
  updateRtcTime();
  updateHallVelocity();
  updateBatteryVoltage();

  const unsigned long now = millis();

  if (now - lastHeartbeatMs >= kHeartbeatIntervalMs) {
    lastHeartbeatMs = now;

    noInterrupts();
    const uint32_t pulseTotal = hallPulseCount;
    const uint32_t rejectedPulseTotal = hallRejectedPulseCount;
    interrupts();
    const uint32_t pulseDelta = pulseTotal - lastHeartbeatPulseCount;
    lastHeartbeatPulseCount = pulseTotal;

    Serial.print("Heartbeat ms=");
    Serial.print(now);
    Serial.print(", led=");
    Serial.print(ledState ? "ON" : "OFF");
    Serial.print(", oled=");
    Serial.print(oledStatus);
    Serial.print(", speed_kmh=");
    Serial.print(hallSpeedKmh, 2);
    Serial.print(", battery_v=");
    Serial.print(batteryVoltage, 2);
    Serial.print(", rtc=");
    Serial.print(rtcStatus);
    Serial.print('/');
    Serial.print(rtcTimeValid ? formatDateTimeUtc(currentRtcTime) : String(F("unknown")));
    Serial.print('/');
    Serial.print(rtcTimeValid ? formatDateTimeAustralianEastern(currentRtcTime) : String(F("unknown")));
    Serial.print(", gps=");
    Serial.print(gpsStatus);
    Serial.print(", gps_baud=");
    Serial.print(gpsActiveBaudRate);
    Serial.print(", gps_fix=");
    Serial.print(gpsLocationValid ? "yes" : "no");
    Serial.print(", gps_lat_lon=");
    if (gpsLocationValid) {
      Serial.print(gpsLatitude, 6);
      Serial.print('/');
      Serial.print(gpsLongitude, 6);
    } else {
      Serial.print("unknown");
    }
    Serial.print(", gps_sat=");
    Serial.print(gpsSatellites);
    Serial.print(", gps_bytes=");
    Serial.print(gpsBytesThisBoot);
    Serial.print(", gps_sentences=");
    Serial.print(gpsSentencesThisBoot);
    Serial.print(", gps_bad=");
    Serial.print(gpsFailedChecksumsThisBoot);
    Serial.print(", gps_last_byte_ms_ago=");
    Serial.print(lastGpsByteMs > 0 ? now - lastGpsByteMs : 0);
    Serial.print(", gps_raw=");
    Serial.print(gpsRawPreview);
    Serial.print(", sd=");
    Serial.print(sdCardStatus);
    Serial.print(", sd_spi_hz=");
    Serial.print(sdCardActiveSpiFrequencyHz);
    Serial.print(", audio=");
    Serial.print(audioStatus);
    Serial.print(", startup_stream=");
    Serial.print(startupStreamStatus);
    Serial.print(", audio_req/start=");
    Serial.print(audioPlayRequests);
    Serial.print('/');
    Serial.print(audioPlayStarts);
    Serial.print(", audio_hash=0x");
    Serial.print(steamClip.sourceDataHash, HEX);
    Serial.print(", audio_pins=LRC/BCLK/DIN/SD:");
    Serial.print(kAudioI2sLrcPin);
    Serial.print('/');
    Serial.print(kAudioI2sBclkPin);
    Serial.print('/');
    Serial.print(kAudioI2sDinPin);
    Serial.print('/');
    Serial.print(kAudioAmpSdPin);
    Serial.print(", pulse_total=");
    Serial.print(pulseTotal);
    Serial.print(", pulse_rejected=");
    Serial.print(rejectedPulseTotal);
    Serial.print(", pulse_delta=");
    Serial.println(pulseDelta);
  }
}
