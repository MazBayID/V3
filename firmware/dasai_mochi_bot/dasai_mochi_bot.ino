// ============================================================================
//  dasai_mochi_bot.ino  —  main firmware
//
//  Board:  Waveshare ESP32-C3-Zero  (Arduino core "ESP32C3 Dev Module")
//  Flow:
//     1. Boot. Load saved settings from NVS.
//     2. No WiFi saved?  -> start captive-portal AP "DasaiMochi-Setup".
//        User enters WiFi + provider + API key on the on-device page.
//     3. WiFi saved?     -> connect. Show idle silly faces.
//     4. Press TALK  -> record mic (I2S) -> STT -> LLM -> TTS -> speaker.
//        Faces animate through LISTEN / THINK / TALK the whole time.
//     5. MODE button toggles between "assistant" and pure "silly-faces" mode.
//
//  This single sketch + the .h/.cpp helpers compile into ONE firmware.bin
//  that the GitHub Pages web flasher installs in the browser.
//
//  REQUIRED LIBRARIES (Arduino Library Manager):
//     - Adafruit GFX Library
//     - Adafruit SSD1306
//     - ArduinoJson  (v7)
//     - Adafruit NeoPixel  (status LED)
//  ESP32 core provides: WiFi, WebServer, DNSServer, Preferences,
//     HTTPClient, WiFiClientSecure, driver/i2s.
// ============================================================================
#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>

#include "config.h"
#include "providers.h"
#include "faces.h"
#include "portal_page.h"
#include "llm_client.h"
#include "audio_io.h"

// --- globals ---------------------------------------------------------------
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
Adafruit_NeoPixel led(RGB_LED_COUNT, PIN_RGB_LED, NEO_GRB + NEO_KHZ800);
FaceEngine face(display);
WebServer  server(HTTP_PORT);
DNSServer  dns;
Preferences prefs;
LlmClient   llm;
AudioIO     audio;

// --- runtime state ---------------------------------------------------------
struct Settings {
  String ssid, pass, provider = "groq", apikey, model, botname = "Mochi";
} cfg;

bool   provisioned   = false;     // do we have WiFi + key?
bool   portalMode    = false;     // are we serving the setup AP?
bool   assistantMode = true;      // false = pure silly-faces toy
uint32_t lastInteract = 0;

// --- button debounce --------------------------------------------------------
struct Btn { 
  uint8_t pin; 
  bool last=false; 
  uint32_t t=0; 
  Btn(uint8_t p) : pin(p) {} 
};

Btn bMain{PIN_TOUCH_MAIN}, bNext{PIN_TOUCH_NEXT}, bMode{PIN_TOUCH_MODE};
bool pressed(Btn& b) {
  bool now = digitalRead(b.pin);
  bool fired = false;
  if (now && !b.last && millis() - b.t > 200) { fired = true; b.t = millis(); }
  b.last = now;
  return fired;
}

// --- LED helper -------------------------------------------------------------
void setLed(uint8_t r, uint8_t g, uint8_t b) {
  led.setPixelColor(0, led.Color(r, g, b)); led.show();
}

// Limit Wi-Fi RF transmit power to 10 dBm. This is applied after selecting
// the Wi-Fi mode and again before connecting/starting the AP.
void applyWifiTxPower() {
#ifdef WIFI_TX_POWER_10DBM
  // ESP-IDF expresses max TX power in 0.25 dBm units.
  // 10 dBm = 40 quarter-dBm units.
  esp_wifi_set_max_tx_power(40);
#endif
}

// ============================================================================
//  SETTINGS PERSISTENCE
// ============================================================================
void loadSettings() {
  prefs.begin(NVS_NAMESPACE, true);
  cfg.ssid     = prefs.getString(NVS_KEY_SSID, "");
  cfg.pass     = prefs.getString(NVS_KEY_PASS, "");
  cfg.provider = prefs.getString(NVS_KEY_PROVIDER, "groq");
  cfg.apikey   = prefs.getString(NVS_KEY_APIKEY, "");
  cfg.model    = prefs.getString(NVS_KEY_MODEL, "");
  cfg.botname  = prefs.getString(NVS_KEY_NAME, "Mochi");
  prefs.end();
  provisioned = cfg.ssid.length() && cfg.apikey.length();
}

void saveSettings() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString(NVS_KEY_SSID, cfg.ssid);
  prefs.putString(NVS_KEY_PASS, cfg.pass);
  prefs.putString(NVS_KEY_PROVIDER, cfg.provider);
  prefs.putString(NVS_KEY_APIKEY, cfg.apikey);
  prefs.putString(NVS_KEY_MODEL, cfg.model);
  prefs.putString(NVS_KEY_NAME, cfg.botname);
  prefs.end();
}

// ============================================================================
//  CAPTIVE PORTAL
// ============================================================================
void showBanner(const String& l1, const String& l2 = "") {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 20); display.println(l1);
  if (l2.length()) { display.setCursor(0, 36); display.println(l2); }
  display.display();
}

void startPortal() {
  portalMode = true;
  WiFi.mode(WIFI_AP);
  applyWifiTxPower();
  IPAddress ip(CONFIG_PORTAL_IP);
  WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  dns.start(DNS_PORT, "*", ip);

  server.onNotFound([]() {                  // captive-portal redirect
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
  server.on("/", []() { server.send_P(200, "text/html", PORTAL_HTML); });
  server.on("/save", HTTP_POST, []() {
    cfg.ssid     = server.arg("ssid");
    cfg.pass     = server.arg("pass");
    cfg.provider = server.arg("provider");
    cfg.model    = server.arg("model");
    cfg.apikey   = server.arg("apikey");
    cfg.botname  = server.arg("botname"); if (!cfg.botname.length()) cfg.botname = "Mochi";
    saveSettings();
    server.send_P(200, "text/html", SAVED_HTML);
    delay(1200);
    ESP.restart();
  });
  server.begin();

  setLed(40, 0, 40);  // purple = waiting for setup
  showBanner("Setup mode", "Join: " AP_SSID);
}

// ============================================================================
//  WIFI CONNECT
// ============================================================================
bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  applyWifiTxPower();
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  showBanner("Connecting WiFi", cfg.ssid);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    setLed(0, 0, (millis() / 250) % 2 ? 30 : 0);  // blink blue
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

// ============================================================================
//  ONE CONVERSATION TURN
// ============================================================================
void doConversation() {
  lastInteract = millis();

  // 1) record ------------------------------------------------------
  face.setState(FACE_LISTEN); face.update();
  setLed(0, 40, 0);                    // green = listening
  // Allocate the PCM buffer only for the recording/STT phase. Keeping this
  // out of global .bss is critical on ESP32-C3, where a static 8-second
  // 16 kHz PCM buffer consumes about 256 KB of internal RAM.
  const size_t maxSamples = (size_t)MIC_SAMPLE_RATE * RECORD_MAX_SECS;
  int16_t* pcm = static_cast<int16_t*>(malloc(maxSamples * sizeof(int16_t)));
  if (!pcm) {
    Serial.println("ERROR: PCM buffer allocation failed");
    face.setState(FACE_SAD); face.update();
    setLed(40, 0, 0);
    delay(1000);
    face.setState(FACE_IDLE); face.update();
    setLed(0, 0, 0);
    return;
  }

  size_t samples = audio.record(pcm, maxSamples,
                                /*untilButtonReleased*/ PIN_TOUCH_MAIN);

  // 2) speech-to-text ---------------------------------------------
  face.setState(FACE_THINK); face.update();
  setLed(40, 30, 0);                   // amber = thinking
  String userText = llm.speechToText(pcm, samples, cfg.provider, cfg.apikey);
  free(pcm);
  pcm = nullptr;
  if (!userText.length()) { face.setState(FACE_SAD); face.update(); delay(800); return; }

  // 3) LLM reply ---------------------------------------------------
  const ProviderInfo* p = providerById(cfg.provider);
  String model = cfg.model.length() ? cfg.model : String(p->defaultModel);
  String reply = llm.chat(userText, cfg.provider, cfg.apikey, model, cfg.botname);
  if (!reply.length()) { face.setState(FACE_SAD); face.update(); delay(800); return; }

  // 4) speak -------------------------------------------------------
  face.setState(FACE_TALK);
  setLed(0, 20, 40);                   // cyan = talking
  audio.speak(reply, cfg.provider, cfg.apikey,
              [](uint8_t amp){ face.setMouthOpen(amp); face.update(); });

  face.setState(FACE_HAPPY); face.update(); delay(600);
  face.setState(FACE_IDLE);
  setLed(0, 0, 0);
  lastInteract = millis();
}

// ============================================================================
//  SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_TOUCH_MAIN, INPUT);
  pinMode(PIN_TOUCH_NEXT, INPUT);
  pinMode(PIN_TOUCH_MODE, INPUT);

  led.begin(); led.setBrightness(60); setLed(20, 20, 20);

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    Serial.println("SSD1306 not found — check wiring/address");
  }
  display.clearDisplay(); display.display();
  face.begin();

  loadSettings();

  if (!provisioned) {
    startPortal();
    return;                            // stay in portal loop()
  }

  if (!connectWiFi()) {                // saved creds failed -> re-provision
    showBanner("WiFi failed", "Re-entering setup");
    delay(1200);
    startPortal();
    return;
  }

  audio.begin();                       // bring up I2S mic + amp
  llm.begin();
  setLed(0, 0, 0);
  face.setState(FACE_HAPPY); face.update(); delay(700);
  face.setState(FACE_IDLE);
  lastInteract = millis();
}

// ============================================================================
//  LOOP
// ============================================================================
void loop() {
  if (portalMode) {                    // setup mode: just serve the page
    dns.processNextRequest();
    server.handleClient();
    face.setState(FACE_SLEEP);         // snoozing until configured
    face.update();
    return;
  }

  // --- buttons ---
  if (pressed(bMode)) {
    assistantMode = !assistantMode;
    face.setState(assistantMode ? FACE_HAPPY : FACE_SURPRISE);
    face.update(); delay(400);
    face.setState(FACE_IDLE);
    lastInteract = millis();
  }
  if (pressed(bNext)) {                // cycle silly faces
    face.nextSillyFace();
    lastInteract = millis();
  }
  if (assistantMode && pressed(bMain)) {
    if (WiFi.status() == WL_CONNECTED) doConversation();
    else { showBanner("No WiFi", "reconnecting..."); connectWiFi(); }
  }

  // --- idle entertainment ---
  if (millis() - lastInteract > IDLE_TIMEOUT_MS && face.state() != FACE_IDLE
      && face.state() != FACE_SLEEP) {
    face.setState(FACE_IDLE);
  }
  face.update();
}
