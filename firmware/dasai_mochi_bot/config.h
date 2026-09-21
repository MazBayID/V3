// ============================================================================
//  Dasai Mochi Bot  —  config.h
//  Central place for pins, constants, and tunables.
//  Target: ESP32-C3 SuperMini (ESP32-C3, 4MB flash)
// ============================================================================
#pragma once

// ---------------------------------------------------------------------------
//  FIRMWARE INFO
// ---------------------------------------------------------------------------
#define FW_NAME     "Dasai Mochi Bot"
#define FW_VERSION  "2.0.1"

// ---------------------------------------------------------------------------
//  PIN MAP  (ESP32-C3 SuperMini port of the original Dasai Mochi firmware)
//
//  Port notes:
//    * Exposed GPIOs: 0,1,2,3,4,5,6,7,8,9,10,18,19,20,21
//    * GPIO12..GPIO17 are NOT broken out (used by the stacked 4MB flash).
//    * GPIO9  = BOOT button  (hold during USB connect to enter flash mode).
//    * GPIO10 = onboard WS2812 RGB LED.
//    * GPIO20 = UART0 RX, GPIO21 = UART0 TX (used for serial logging).
//
//  The ESP32-C3 has NO dedicated I2C/I2S pins — any GPIO can be mapped,
//  which is why the pins below are freely chosen and easy to rewire.
// ---------------------------------------------------------------------------

// --- OLED (SSD1306 0.96", I2C) ---
#define PIN_OLED_SDA      21
#define PIN_OLED_SCL      20
#define OLED_I2C_ADDR     0x3C       // most 0.96" SSD1306 modules; some are 0x3D
#define OLED_WIDTH        128
#define OLED_HEIGHT       64

// --- INMP441 microphone (I2S input) ---
#define PIN_I2S_MIC_SCK   1          // bit clock  (BCLK)
#define PIN_I2S_MIC_WS    2          // word select (LRCLK)
#define PIN_I2S_MIC_SD    8          // serial data out of mic
// INMP441 L/R pin -> tie to GND for LEFT channel (recommended)

// --- MAX98357A I2S amplifier -> 2W speaker (I2S output) ---
#define PIN_I2S_SPK_BCLK  1
#define PIN_I2S_SPK_LRC   2
#define PIN_I2S_SPK_DIN   5
// MAX98357A GAIN pin: leave floating = 9dB. SD pin: tie HIGH to enable.

// --- TTP touch buttons (active-HIGH digital) ---
#define PIN_TOUCH_MAIN    3          // TALK / wake (TTP223 active-HIGH)
#define PIN_TOUCH_NEXT    6          // NEXT face / menu (TTP223 active-HIGH)
#define PIN_TOUCH_MODE    7          // MODE assistant <-> idle-faces (TTP223 active-HIGH)

// --- Status LED (onboard WS2812) ---
#define PIN_RGB_LED       10
#define RGB_LED_COUNT     1

// ---------------------------------------------------------------------------
//  AUDIO
// ---------------------------------------------------------------------------
#define MIC_SAMPLE_RATE   16000      // 16 kHz mono is the STT sweet spot
#define SPK_SAMPLE_RATE   16000
#define RECORD_MAX_SECS   4          // hard cap on a single utterance; keeps runtime RAM practical

// ---------------------------------------------------------------------------
//  CAPTIVE PORTAL / PROVISIONING
// ---------------------------------------------------------------------------
#define AP_SSID           "DasaiMochi-Setup"
#define AP_PASSWORD       ""         // open AP for easy first-time setup
#define CONFIG_PORTAL_IP  192,168,4,1
#define DNS_PORT          53
#define HTTP_PORT         80

// How long to wait for a saved WiFi to connect before falling back to AP.
#define WIFI_CONNECT_TIMEOUT_MS  15000

// Keep Wi-Fi transmit power at 10 dBm to reduce RF output and power use.
#define WIFI_TX_POWER_10DBM

// ---------------------------------------------------------------------------
//  IDLE FACE ANIMATION
// ---------------------------------------------------------------------------
#define IDLE_FRAME_MS     120        // base tick for blink/idle animation
#define IDLE_TIMEOUT_MS   8000        // no interaction -> drift into idle faces

// ---------------------------------------------------------------------------
//  NVS (saved settings) KEYS
// ---------------------------------------------------------------------------
#define NVS_NAMESPACE     "mochi"
#define NVS_KEY_SSID      "wifi_ssid"
#define NVS_KEY_PASS      "wifi_pass"
#define NVS_KEY_PROVIDER  "llm_prov"
#define NVS_KEY_APIKEY    "llm_key"
#define NVS_KEY_MODEL     "llm_model"
#define NVS_KEY_NAME      "bot_name"
