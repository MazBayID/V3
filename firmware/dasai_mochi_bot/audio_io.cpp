// ============================================================================
// audio_io.cpp — FIXED V1
//
// ESP32-C3 Super Mini
// INMP441 microphone + MAX98357A speaker
//
// Current Neo wiring:
//
// INMP441
//   SCK -> GPIO 1
//   WS  -> GPIO 2
//   SD  -> GPIO 8
//   L/R -> GND
//
// MAX98357A
//   BCLK -> GPIO 1
//   LRC  -> GPIO 2
//   DIN  -> GPIO 5
//
// IMPORTANT:
// ESP32-C3 has one I2S peripheral used here.
// Therefore RX and TX are configured separately.
// This version performs a clean shutdown before switching
// between microphone and speaker.
// ============================================================================

#include "audio_io.h"
#include "config.h"
#include "providers.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <string.h>

#define I2S_PORT I2S_NUM_0

// -----------------------------------------------------------------------------
// I2S / AUDIO SETTINGS
// -----------------------------------------------------------------------------

#define MIC_DMA_BUF_COUNT       4
#define MIC_DMA_BUF_LEN         256

#define SPK_DMA_BUF_COUNT       8
#define SPK_DMA_BUF_LEN         256

#define AUDIO_CHUNK_SAMPLES     256

#define I2S_READ_TIMEOUT_MS     100
#define I2S_WRITE_TIMEOUT_MS    100

// INMP441 gives 24-bit audio inside a 32-bit I2S slot.
// We convert it to signed 16-bit PCM.
#define MIC_SHIFT               14

// -----------------------------------------------------------------------------
// INTERNAL HELPERS
// -----------------------------------------------------------------------------

namespace {

enum I2SState {
  I2S_STATE_NONE = 0,
  I2S_STATE_MIC,
  I2S_STATE_SPK
};

I2SState currentState = I2S_STATE_NONE;


// -----------------------------------------------------------------------------
// Stop / uninstall current I2S configuration
// -----------------------------------------------------------------------------

void shutdownI2S() {

  if (currentState == I2S_STATE_NONE) {
    return;
  }

  // Stop the peripheral first.
  i2s_stop(I2S_PORT);

  delay(5);

  // Destroy driver and DMA buffers.
  i2s_driver_uninstall(I2S_PORT);

  delay(5);

  currentState = I2S_STATE_NONE;
}


// -----------------------------------------------------------------------------
// Convert 32-bit INMP441 sample to 16-bit PCM
// -----------------------------------------------------------------------------

static inline int16_t convertMicSample(int32_t raw) {

  int32_t value = raw >> MIC_SHIFT;

  if (value > 32767) {
    value = 32767;
  }

  if (value < -32768) {
    value = -32768;
  }

  return (int16_t)value;
}


// -----------------------------------------------------------------------------
// Read exact number of bytes from WiFiClientSecure
// -----------------------------------------------------------------------------

bool readExact(
    WiFiClientSecure& client,
    uint8_t* buffer,
    size_t length,
    uint32_t timeoutMs = 8000
) {

  size_t received = 0;
  uint32_t lastData = millis();

  while (received < length) {

    while (client.available()) {

      int c = client.read();

      if (c < 0) {
        break;
      }

      buffer[received++] = (uint8_t)c;
      lastData = millis();

      if (received >= length) {
        return true;
      }
    }

    if (!client.connected() && !client.available()) {
      return false;
    }

    if (millis() - lastData > timeoutMs) {
      return false;
    }

    delay(1);
  }

  return true;
}


// -----------------------------------------------------------------------------
// Little-endian helpers
// -----------------------------------------------------------------------------

uint16_t readLE16(const uint8_t* p) {

  return
      (uint16_t)p[0] |
      ((uint16_t)p[1] << 8);
}


uint32_t readLE32(const uint8_t* p) {

  return
      (uint32_t)p[0] |
      ((uint32_t)p[1] << 8) |
      ((uint32_t)p[2] << 16) |
      ((uint32_t)p[3] << 24);
}


// -----------------------------------------------------------------------------
// WAV parser
//
// We DO NOT assume a fixed 44-byte header.
//
// We search for:
//   RIFF
//   WAVE
//   fmt
//   data
//
// This prevents WAV metadata chunks from being interpreted as PCM.
// -----------------------------------------------------------------------------

struct WavInfo {

  uint16_t audioFormat;
  uint16_t channels;
  uint32_t sampleRate;
  uint16_t bitsPerSample;
  uint32_t dataSize;

  bool valid;
};


bool parseWavHeader(
    WiFiClientSecure& client,
    WavInfo& wav
) {

  wav.audioFormat = 0;
  wav.channels = 0;
  wav.sampleRate = 0;
  wav.bitsPerSample = 0;
  wav.dataSize = 0;
  wav.valid = false;

  uint8_t header[12];

  if (!readExact(client, header, sizeof(header))) {
    Serial.println("[AUDIO] Failed to read WAV RIFF header.");
    return false;
  }

  // RIFF
  if (memcmp(header, "RIFF", 4) != 0) {
    Serial.println("[AUDIO] ERROR: Response is not RIFF/WAV.");
    return false;
  }

  // WAVE
  if (memcmp(header + 8, "WAVE", 4) != 0) {
    Serial.println("[AUDIO] ERROR: RIFF container is not WAVE.");
    return false;
  }

  bool foundFmt = false;
  bool foundData = false;

  // WAV can contain many chunks.
  // Search until fmt and data are found.
  for (int chunkIndex = 0; chunkIndex < 32; chunkIndex++) {

    uint8_t chunkHeader[8];

    if (!readExact(client, chunkHeader, sizeof(chunkHeader))) {
      Serial.println("[AUDIO] Failed reading WAV chunk header.");
      return false;
    }

    char chunkId[5];

    chunkId[0] = chunkHeader[0];
    chunkId[1] = chunkHeader[1];
    chunkId[2] = chunkHeader[2];
    chunkId[3] = chunkHeader[3];
    chunkId[4] = '\0';

    uint32_t chunkSize = readLE32(chunkHeader + 4);

    // ---------------------------------------------------------------
    // fmt chunk
    // ---------------------------------------------------------------

    if (memcmp(chunkId, "fmt ", 4) == 0) {

      if (chunkSize < 16 || chunkSize > 4096) {
        Serial.println("[AUDIO] ERROR: Invalid WAV fmt chunk.");
        return false;
      }

      uint8_t fmt[16];

      if (!readExact(client, fmt, sizeof(fmt))) {
        Serial.println("[AUDIO] Failed reading WAV fmt chunk.");
        return false;
      }

      wav.audioFormat = readLE16(fmt + 0);
      wav.channels = readLE16(fmt + 2);
      wav.sampleRate = readLE32(fmt + 4);
      wav.bitsPerSample = readLE16(fmt + 14);

      foundFmt = true;

      // Skip any additional fmt bytes.
      uint32_t remaining = chunkSize - 16;

      while (remaining > 0) {

        uint8_t temp[64];

        size_t n =
            remaining > sizeof(temp)
                ? sizeof(temp)
                : remaining;

        if (!readExact(client, temp, n)) {
          Serial.println("[AUDIO] Failed skipping extended fmt data.");
          return false;
        }

        remaining -= n;
      }
    }

    // ---------------------------------------------------------------
    // data chunk
    // ---------------------------------------------------------------

    else if (memcmp(chunkId, "data", 4) == 0) {

      wav.dataSize = chunkSize;
      foundData = true;

      break;
    }

    // ---------------------------------------------------------------
    // Unknown chunk
    // ---------------------------------------------------------------

    else {

      // WAV chunks are padded to an even byte boundary.
      uint32_t skipSize = chunkSize;

      if (skipSize & 1) {
        skipSize++;
      }

      while (skipSize > 0) {

        uint8_t temp[64];

        size_t n =
            skipSize > sizeof(temp)
                ? sizeof(temp)
                : skipSize;

        if (!readExact(client, temp, n)) {
          Serial.println("[AUDIO] Failed skipping WAV chunk.");
          return false;
        }

        skipSize -= n;
      }
    }

    if (foundFmt && foundData) {
      break;
    }
  }

  if (!foundFmt) {
    Serial.println("[AUDIO] ERROR: WAV fmt chunk not found.");
    return false;
  }

  if (!foundData) {
    Serial.println("[AUDIO] ERROR: WAV data chunk not found.");
    return false;
  }

  // We only support normal PCM.
  if (wav.audioFormat != 1) {

    Serial.print("[AUDIO] ERROR: Unsupported WAV format: ");
    Serial.println(wav.audioFormat);

    return false;
  }

  // We expect 16-bit PCM from TTS.
  if (wav.bitsPerSample != 16) {

    Serial.print("[AUDIO] ERROR: Unsupported WAV bit depth: ");
    Serial.println(wav.bitsPerSample);

    return false;
  }

  if (wav.channels < 1 || wav.channels > 2) {

    Serial.print("[AUDIO] ERROR: Unsupported WAV channels: ");
    Serial.println(wav.channels);

    return false;
  }

  wav.valid = true;

  Serial.println("[AUDIO] WAV detected:");
  Serial.print("         channels     = ");
  Serial.println(wav.channels);

  Serial.print("         sample rate  = ");
  Serial.println(wav.sampleRate);

  Serial.print("         bits/sample  = ");
  Serial.println(wav.bitsPerSample);

  Serial.print("         data bytes   = ");
  Serial.println(wav.dataSize);

  return true;
}


// -----------------------------------------------------------------------------
// JSON escape
//
// Prevents quotation marks/newlines in assistant text from corrupting
// the TTS JSON request.
// -----------------------------------------------------------------------------

String jsonEscape(const String& input) {

  String output;
  output.reserve(input.length() + 16);

  for (size_t i = 0; i < input.length(); i++) {

    char c = input[i];

    switch (c) {

      case '\"':
        output += "\\\"";
        break;

      case '\\':
        output += "\\\\";
        break;

      case '\n':
        output += "\\n";
        break;

      case '\r':
        output += "\\r";
        break;

      case '\t':
        output += "\\t";
        break;

      default:
        output += c;
        break;
    }
  }

  return output;
}

} // namespace


// ============================================================================
// AudioIO
// ============================================================================

void AudioIO::begin() {

  // Lazy initialization.
  //
  // We intentionally DO NOT install I2S here.
  //
  // record() installs RX.
  // speak() installs TX.
  //
  // This keeps startup simple and avoids leaving the C3 I2S peripheral
  // in an unwanted state.
  micReady = false;
  spkReady = false;

  currentState = I2S_STATE_NONE;

  Serial.println("[AUDIO] AudioIO initialized.");
}


// ============================================================================
// START MICROPHONE
// ============================================================================

void AudioIO::startMic() {

  shutdownI2S();

  i2s_config_t cfg = {};

  cfg.mode =
      (i2s_mode_t)(
          I2S_MODE_MASTER |
          I2S_MODE_RX
      );

  cfg.sample_rate = MIC_SAMPLE_RATE;

  // INMP441 sends 24-bit data in a 32-bit I2S slot.
  cfg.bits_per_sample =
      I2S_BITS_PER_SAMPLE_32BIT;

  // L/R pin of INMP441 is tied to GND.
  // Therefore use LEFT channel.
  cfg.channel_format =
      I2S_CHANNEL_FMT_ONLY_LEFT;

  cfg.communication_format =
      I2S_COMM_FORMAT_STAND_I2S;

  cfg.intr_alloc_flags =
      ESP_INTR_FLAG_LEVEL1;

  cfg.dma_buf_count =
      MIC_DMA_BUF_COUNT;

  cfg.dma_buf_len =
      MIC_DMA_BUF_LEN;

  cfg.use_apll = false;

  cfg.tx_desc_auto_clear = true;

  cfg.fixed_mclk = 0;


  esp_err_t result =
      i2s_driver_install(
          I2S_PORT,
          &cfg,
          0,
          NULL
      );

  if (result != ESP_OK) {

    Serial.print("[AUDIO] ERROR installing MIC I2S: ");
    Serial.println(result);

    micReady = false;
    currentState = I2S_STATE_NONE;

    return;
  }


  i2s_pin_config_t pins = {};

  pins.bck_io_num =
      PIN_I2S_MIC_SCK;

  pins.ws_io_num =
      PIN_I2S_MIC_WS;

  pins.data_in_num =
      PIN_I2S_MIC_SD;

  pins.data_out_num =
      I2S_PIN_NO_CHANGE;


  result =
      i2s_set_pin(
          I2S_PORT,
          &pins
      );

  if (result != ESP_OK) {

    Serial.print("[AUDIO] ERROR setting MIC pins: ");
    Serial.println(result);

    i2s_driver_uninstall(I2S_PORT);

    micReady = false;
    currentState = I2S_STATE_NONE;

    return;
  }


  // Clear stale DMA data.
  i2s_zero_dma_buffer(I2S_PORT);

  delay(10);

  micReady = true;
  spkReady = false;

  currentState = I2S_STATE_MIC;

  Serial.println("[AUDIO] INMP441 RX ready.");
}


// ============================================================================
// START SPEAKER
// ============================================================================

void AudioIO::startSpk() {

  shutdownI2S();

  i2s_config_t cfg = {};

  cfg.mode =
      (i2s_mode_t)(
          I2S_MODE_MASTER |
          I2S_MODE_TX
      );

  cfg.sample_rate =
      SPK_SAMPLE_RATE;

  cfg.bits_per_sample =
      I2S_BITS_PER_SAMPLE_16BIT;

  // MAX98357A receives mono left-channel PCM.
  cfg.channel_format =
      I2S_CHANNEL_FMT_ONLY_LEFT;

  cfg.communication_format =
      I2S_COMM_FORMAT_STAND_I2S;

  cfg.intr_alloc_flags =
      ESP_INTR_FLAG_LEVEL1;

  cfg.dma_buf_count =
      SPK_DMA_BUF_COUNT;

  cfg.dma_buf_len =
      SPK_DMA_BUF_LEN;

  cfg.use_apll = false;

  cfg.tx_desc_auto_clear = true;

  cfg.fixed_mclk = 0;


  esp_err_t result =
      i2s_driver_install(
          I2S_PORT,
          &cfg,
          0,
          NULL
      );

  if (result != ESP_OK) {

    Serial.print("[AUDIO] ERROR installing SPK I2S: ");
    Serial.println(result);

    spkReady = false;
    currentState = I2S_STATE_NONE;

    return;
  }


  i2s_pin_config_t pins = {};

  pins.bck_io_num =
      PIN_I2S_SPK_BCLK;

  pins.ws_io_num =
      PIN_I2S_SPK_LRC;

  pins.data_out_num =
      PIN_I2S_SPK_DIN;

  pins.data_in_num =
      I2S_PIN_NO_CHANGE;


  result =
      i2s_set_pin(
          I2S_PORT,
          &pins
      );

  if (result != ESP_OK) {

    Serial.print("[AUDIO] ERROR setting SPK pins: ");
    Serial.println(result);

    i2s_driver_uninstall(I2S_PORT);

    spkReady = false;
    currentState = I2S_STATE_NONE;

    return;
  }


  // Start with silence.
  i2s_zero_dma_buffer(I2S_PORT);

  delay(10);

  micReady = false;
  spkReady = true;

  currentState = I2S_STATE_SPK;

  Serial.println("[AUDIO] MAX98357A TX ready.");
}


// ============================================================================
// RECORD MICROPHONE
// ============================================================================

size_t AudioIO::record(
    int16_t* buf,
    size_t maxSamples,
    uint8_t holdPin
) {

  if (buf == nullptr || maxSamples == 0) {
    return 0;
  }


  // Make sure we're in MIC mode.
  if (currentState != I2S_STATE_MIC || !micReady) {

    if (spkReady || currentState == I2S_STATE_SPK) {
      shutdownI2S();
    }

    startMic();

    if (!micReady) {
      Serial.println("[AUDIO] ERROR: microphone failed to start.");
      return 0;
    }
  }


  // Clear old DMA data before recording.
  i2s_zero_dma_buffer(I2S_PORT);

  delay(10);


  size_t got = 0;

  int32_t raw[256];

  uint32_t start = millis();

  Serial.println("[AUDIO] Recording...");


  // Record while TALK is held.
  //
  // A 400 ms grace period is preserved from the original firmware,
  // so a quick tap still records something.
  while (
      (digitalRead(holdPin) ||
       millis() - start < 400) &&
      got < maxSamples
  ) {

    size_t bytesRead = 0;

    esp_err_t result =
        i2s_read(
            I2S_PORT,
            raw,
            sizeof(raw),
            &bytesRead,
            pdMS_TO_TICKS(I2S_READ_TIMEOUT_MS)
        );


    if (result != ESP_OK) {

      Serial.print("[AUDIO] I2S read error: ");
      Serial.println(result);

      break;
    }


    if (bytesRead == 0) {
      continue;
    }


    size_t sampleCount =
        bytesRead / sizeof(int32_t);


    for (
        size_t i = 0;
        i < sampleCount && got < maxSamples;
        i++
    ) {

      buf[got++] =
          convertMicSample(raw[i]);
    }


    if (
        millis() - start >=
        RECORD_MAX_SECS * 1000UL
    ) {
      break;
    }
  }


  // --------------------------------------------------------------------------
  // Remove DC offset.
  //
  // The diagnostic test showed a noticeable average offset.
  // We calculate the mean of the captured block and subtract it.
  // This leaves the AC/audio component for STT.
  // --------------------------------------------------------------------------

  if (got > 0) {

    int64_t sum = 0;

    for (size_t i = 0; i < got; i++) {
      sum += buf[i];
    }

    int32_t dcOffset =
        (int32_t)(sum / (int64_t)got);


    for (size_t i = 0; i < got; i++) {

      int32_t sample =
          (int32_t)buf[i] -
          dcOffset;


      if (sample > 32767) {
        sample = 32767;
      }

      if (sample < -32768) {
        sample = -32768;
      }

      buf[i] =
          (int16_t)sample;
    }


    Serial.print("[AUDIO] Recorded samples: ");
    Serial.println(got);

    Serial.print("[AUDIO] DC offset removed: ");
    Serial.println(dcOffset);
  }


  Serial.println("[AUDIO] Recording finished.");

  return got;
}


// ============================================================================
// SPEAK / TTS
// ============================================================================

void AudioIO::speak(
    const String& text,
    const String& provider,
    const String& apiKey,
    std::function<void(uint8_t)> ampCb
) {

  if (text.length() == 0) {
    return;
  }


  // --------------------------------------------------------------------------
  // Switch from MIC to SPEAKER.
  // --------------------------------------------------------------------------

  if (
      currentState != I2S_STATE_SPK ||
      !spkReady
  ) {

    shutdownI2S();

    startSpk();

    if (!spkReady) {
      Serial.println("[AUDIO] ERROR: speaker failed to start.");
      return;
    }
  }


  // --------------------------------------------------------------------------
  // Select TTS endpoint.
  // --------------------------------------------------------------------------

  const char* host;
  const char* path;
  const char* model;
  const char* voice;


  if (provider == "groq") {

    host =
        "api.groq.com";

    path =
        "/openai/v1/audio/speech";

    model =
        "playai-tts";

    voice =
        "Fritz-PlayAI";

  } else {

    host =
        "api.openai.com";

    path =
        "/v1/audio/speech";

    model =
        "gpt-4o-mini-tts";

    voice =
        "alloy";
  }


  // --------------------------------------------------------------------------
  // JSON body
  // --------------------------------------------------------------------------

  String safeText =
      jsonEscape(text);


  String body =
      String("{\"model\":\"") +
      model +
      "\",\"voice\":\"" +
      voice +
      "\",\"input\":\"" +
      safeText +
      "\",\"response_format\":\"wav\"," +
      "\"sample_rate\":" +
      String(SPK_SAMPLE_RATE) +
      "}";


  Serial.println("[AUDIO] Connecting TTS...");
  Serial.print("[AUDIO] Provider: ");
  Serial.println(provider);


  WiFiClientSecure client;

  client.setInsecure();


  if (!client.connect(host, 443)) {

    Serial.println("[AUDIO] ERROR: TTS connection failed.");

    if (ampCb) {
      ampCb(0);
    }

    return;
  }


  // --------------------------------------------------------------------------
  // HTTP request
  // --------------------------------------------------------------------------

  client.printf(
      "POST %s HTTP/1.1\r\n",
      path
  );

  client.printf(
      "Host: %s\r\n",
      host
  );

  client.printf(
      "Authorization: Bearer %s\r\n",
      apiKey.c_str()
  );

  client.print(
      "Content-Type: application/json\r\n"
  );

  client.printf(
      "Content-Length: %u\r\n",
      (unsigned int)body.length()
  );

  client.print(
      "Connection: close\r\n"
  );

  client.print(
      "\r\n"
  );

  client.print(body);


  // --------------------------------------------------------------------------
  // Read HTTP status line.
  // --------------------------------------------------------------------------

  uint32_t httpStart =
      millis();


  while (
      !client.available() &&
      client.connected() &&
      millis() - httpStart < 8000
  ) {
    delay(1);
  }


  if (!client.available()) {

    Serial.println(
        "[AUDIO] ERROR: No HTTP response."
    );

    client.stop();

    if (ampCb) {
      ampCb(0);
    }

    return;
  }


  String statusLine =
      client.readStringUntil('\n');

  statusLine.trim();


  Serial.print("[AUDIO] HTTP: ");
  Serial.println(statusLine);


  // HTTP status must be 2xx.
  if (
      statusLine.indexOf(" 200 ") < 0 &&
      statusLine.indexOf(" 201 ") < 0
  ) {

    Serial.println(
        "[AUDIO] ERROR: TTS server returned an error."
    );


    // Print a limited amount of response body.
    uint32_t errorStart =
        millis();

    while (
        client.connected() &&
        millis() - errorStart < 3000
    ) {

      while (client.available()) {

        char c =
            (char)client.read();

        Serial.print(c);

        if (millis() - errorStart > 3000) {
          break;
        }
      }

      delay(1);
    }

    Serial.println();

    client.stop();

    if (ampCb) {
      ampCb(0);
    }

    return;
  }


  // --------------------------------------------------------------------------
  // Skip HTTP headers.
  // --------------------------------------------------------------------------

  uint32_t headerStart =
      millis();


  while (
      client.connected() &&
      millis() - headerStart < 8000
  ) {

    String line =
        client.readStringUntil('\n');

    if (
        line == "\r" ||
        line.length() == 0
    ) {
      break;
    }
  }


  // --------------------------------------------------------------------------
  // Parse WAV properly.
  // --------------------------------------------------------------------------

  WavInfo wav;

  if (!parseWavHeader(client, wav)) {

    Serial.println(
        "[AUDIO] ERROR: Invalid WAV response."
    );

    client.stop();

    if (ampCb) {
      ampCb(0);
    }

    return;
  }


  // --------------------------------------------------------------------------
  // For this firmware we expect mono or stereo 16-bit PCM.
  //
  // If stereo is returned, we mix L+R into mono.
  // --------------------------------------------------------------------------

  uint32_t remainingBytes =
      wav.dataSize;


  int16_t chunk[AUDIO_CHUNK_SAMPLES];

  size_t chunkCount =
      0;


  uint8_t sampleBytes[4];


  uint32_t bytesPerFrame =
      (uint32_t)wav.channels * 2;


  if (bytesPerFrame == 0) {

    client.stop();

    if (ampCb) {
      ampCb(0);
    }

    return;
  }


  // --------------------------------------------------------------------------
  // Stream PCM
  // --------------------------------------------------------------------------

  while (
      remainingBytes >= bytesPerFrame
  ) {

    if (
        !readExact(
            client,
            sampleBytes,
            bytesPerFrame,
            8000
        )
    ) {

      Serial.println(
          "[AUDIO] ERROR: TTS audio stream ended unexpectedly."
      );

      break;
    }


    int16_t sample = 0;


    if (wav.channels == 1) {

      sample =
          (int16_t)(
              (uint16_t)sampleBytes[0] |
              ((uint16_t)sampleBytes[1] << 8)
          );

    } else {

      // Stereo -> mono.
      int16_t left =
          (int16_t)(
              (uint16_t)sampleBytes[0] |
              ((uint16_t)sampleBytes[1] << 8)
          );

      int16_t right =
          (int16_t)(
              (uint16_t)sampleBytes[2] |
              ((uint16_t)sampleBytes[3] << 8)
          );

      int32_t mixed =
          ((int32_t)left +
           (int32_t)right) /
          2;


      if (mixed > 32767) {
        mixed = 32767;
      }

      if (mixed < -32768) {
        mixed = -32768;
      }

      sample =
          (int16_t)mixed;
    }


    chunk[chunkCount++] =
        sample;


    remainingBytes -=
        bytesPerFrame;


    // ------------------------------------------------------------------------
    // Send full PCM chunk to MAX98357A.
    // ------------------------------------------------------------------------

    if (
        chunkCount >=
        AUDIO_CHUNK_SAMPLES
    ) {

      size_t written =
          0;


      esp_err_t result =
          i2s_write(
              I2S_PORT,
              chunk,
              chunkCount * sizeof(int16_t),
              &written,
              pdMS_TO_TICKS(
                  I2S_WRITE_TIMEOUT_MS
              )
          );


      if (result != ESP_OK) {

        Serial.print(
            "[AUDIO] I2S write error: "
        );

        Serial.println(result);

        break;
      }


      // ----------------------------------------------------------------------
      // Lip-sync amplitude.
      // ----------------------------------------------------------------------

      int32_t sumAbs = 0;


      for (
          size_t i = 0;
          i < chunkCount;
          i++
      ) {

        int32_t value =
            chunk[i];

        if (value < 0) {
          value = -value;
        }

        sumAbs += value;
      }


      int32_t average =
          sumAbs /
          (int32_t)chunkCount;


      uint8_t amplitude =
          (uint8_t)min(
              255,
              (int)(average >> 7)
          );


      if (ampCb) {
        ampCb(amplitude);
      }


      chunkCount = 0;
    }
  }


  //--------------------------------------------------------------------------
  // Send remaining samples.
  // --------------------------------------------------------------------------

  if (chunkCount > 0) {

    size_t written =
        0;


    esp_err_t result =
        i2s_write(
            I2S_PORT,
            chunk,
            chunkCount * sizeof(int16_t),
            &written,
            pdMS_TO_TICKS(
                I2S_WRITE_TIMEOUT_MS
            )
        );


    if (result != ESP_OK) {

      Serial.print(
          "[AUDIO] Final I2S write error: "
      );

      Serial.println(result);
    }


    int32_t sumAbs = 0;


    for (
        size_t i = 0;
        i < chunkCount;
        i++
    ) {

      int32_t value =
          chunk[i];

      if (value < 0) {
        value = -value;
      }

      sumAbs += value;
    }


    int32_t average =
        sumAbs /
        (int32_t)chunkCount;


    uint8_t amplitude =
        (uint8_t)min(
            255,
            (int)(average >> 7)
        );


    if (ampCb) {
      ampCb(amplitude);
    }
  }


  // --------------------------------------------------------------------------
  // Give DMA a moment to finish the last samples.
  // --------------------------------------------------------------------------

  delay(30);


  // Stop output and clear DMA.
  i2s_zero_dma_buffer(I2S_PORT);

  delay(5);


  client.stop();


  if (ampCb) {
    ampCb(0);
  }


  Serial.println(
      "[AUDIO] TTS playback finished."
  );


  // Keep speaker mode active.
  //
  // The next record() call will cleanly uninstall it and switch to RX.
}
