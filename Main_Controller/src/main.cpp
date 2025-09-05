#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static uint8_t ESPNOW_PRIMARY_CH = 1;

// ======= WIFI + CLOUD CONFIG =======
static const char* WIFI_SSID = "Velocity Wi-Fi";
static const char* WIFI_PASS = "stolypxc";

// Your deployed HTTPS Cloud Function URL (region us-central1 for free tier)
static const char* INGEST_URL = "https://us-west2-room-state-cloud.cloudfunctions.net/ingestEvent";

// Same value you set with: firebase functions:secrets:set INGEST_API_KEY
static const char* API_KEY = "my-esp32-key";

// (Optional) Give this device a short name for 'source'
static const char* SOURCE_ID = "main-ttgo";

// Wi-Fi connect helper (call once; ESP-NOW still works in STA)
void connectWiFiOnce() {
  static bool done = false;
  if (done) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
  done = (WiFi.status() == WL_CONNECTED);
}

String makeTxId(const char* prefix) { return String(prefix) + "-" + String((uint32_t)millis()); }

bool postEvent(const char* device, const char* action,
               const char* requested_state, const char* confirmed_state,
               const char* tx_id, const char* source, uint64_t client_ts_ms) {
  // CONNECT (temporary association for the POST)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[postEvent] WiFi connect failed");
    // Restore ESP-NOW channel and bail
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    esp_wifi_set_channel(ESPNOW_PRIMARY_CH, WIFI_SECOND_CHAN_NONE);
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();  // MVP: skip cert validation (works with Google HTTPS)

  HTTPClient http;
  if (!http.begin(client, INGEST_URL)) {
    Serial.println("[postEvent] http.begin failed");
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    esp_wifi_set_channel(ESPNOW_PRIMARY_CH, WIFI_SECOND_CHAN_NONE);
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-api-key", API_KEY);
  http.setTimeout(4000);

  StaticJsonDocument<320> doc;
  doc["device"] = device;
  doc["action"] = action;
  if (requested_state) doc["requested_state"] = requested_state;
  if (confirmed_state) doc["confirmed_state"] = confirmed_state;
  doc["tx_id"] = tx_id;
  doc["source"] = source;
  doc["client_ts"] = (double)client_ts_ms;

  String body;  serializeJson(doc, body);

  int code = http.POST(body);
  String resp = http.getString();   // read body (even on errors) for debug
  http.end();

  // TEARDOWN: back to ESP-NOW
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(ESPNOW_PRIMARY_CH, WIFI_SECOND_CHAN_NONE);

  Serial.printf("[postEvent] HTTP %d  body=%s\n", code, resp.c_str());
  return (code >= 200 && code < 300);
}


// ===== Pins =====
#define BLINDS_BTN_PIN    21     // to GND, uses internal pull-up
#define DESK_LED_BTN_PIN  22     // to GND, uses internal pull-up
#define LDR_PIN 32
#define NIGHT_BTN_PIN 13

static const uint32_t DEBOUNCE_MS = 30;

// ===== Peers (MACs) =====
uint8_t motorPeerMac[6] = { 0x14, 0x33, 0x5C, 0x02, 0xAD, 0x70 }; // blinds motor node
uint8_t lightPeerMac[6] = { 0x6C, 0xC8, 0x40, 0x89, 0x73, 0xE8 }; // LED strip node

// Globals
static bool ledAssumedOn = true;
static bool blindsAssumedOpen = true;

// Photoresistor stuff
bool nightmode = false;
static uint16_t LDR_ON_THR  = 1000;  // turn LEDs ON when ADC reading goes below this (darker)
static uint16_t LDR_OFF_THR = 1500;  // turn LEDs OFF when above this (brighter)
int lightVal;

// ===== Motor movement params =====
static const int32_t  STEPS_PER_TAP = 1200;
static const uint16_t US_PER_STEP   = 800;   // lower = faster

// ===== Payloads =====
struct __attribute__((packed)) Command {
  int32_t  steps;        // +/- step count; sign used if dir==0
  uint16_t us_per_step;  // microseconds per step period
  int8_t   dir;          // 1=CW, -1=CCW, 0=use sign of steps
  uint8_t  enable;       // 1=enable driver, 0=disable
};
struct __attribute__((packed)) LightCmd {
  uint8_t action;        // 1=ON, 2=OFF, 3=TOGGLE, 4=SET_BRIGHTNESS
  uint8_t value;         // used only for action 4 (0..255)
};

// ===== Utils =====
static String macToStr(const uint8_t mac[6]) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(b);
}
void onSend(const uint8_t* mac, esp_now_send_status_t status) {
  Serial.printf("Send -> %s : %s\n",
                macToStr(mac).c_str(),
                status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}
void sendMove(int32_t steps, uint16_t us_per_step, int8_t dir = 0, bool enable = true) {
  Command cmd{steps, us_per_step, dir, (uint8_t)(enable ? 1 : 0)};
  esp_err_t err = esp_now_send(motorPeerMac, (uint8_t*)&cmd, sizeof(cmd));
  Serial.printf("Motor: steps=%ld us=%u dir=%d en=%u -> %s\n",
                (long)cmd.steps, cmd.us_per_step, cmd.dir, cmd.enable,
                (err == ESP_OK ? "queued" : "error"));
  bool next = !blindsAssumedOpen;
  const char* nextStr = next ? "open" : "closed";

  String tx2 = makeTxId("blinds");
  postEvent("blinds", "TOGGLE", nextStr, nullptr, tx2.c_str(), "main-ttgo", millis());

  blindsAssumedOpen = next;
}
void sendLightToggle() {
  LightCmd c{3, 0};  // TOGGLE
  esp_err_t err = esp_now_send(lightPeerMac, (uint8_t*)&c, sizeof(c));

  // what we're asking it to become:
  bool next = !ledAssumedOn;
  const char* nextStr = next ? "on" : "off";

  Serial.printf("Light: TOGGLE -> %s (requested %s)\n",
                (err == ESP_OK ? "queued" : "error"), nextStr);

  String tx1 = makeTxId("led");
  postEvent("desk_led", "TOGGLE",
            nextStr,              // <-- send the *new* state
            nullptr,
            tx1.c_str(), "main-ttgo", millis());

  if (err == ESP_OK) ledAssumedOn = next;  // optimistic update
}

// ===== Debouncer (with ctor) =====
struct DebouncedButton {
  uint8_t  pin;
  bool     stable;
  bool     last_raw;
  uint32_t last_change;
  explicit DebouncedButton(uint8_t p)
    : pin(p), stable(HIGH), last_raw(HIGH), last_change(0) {}
  DebouncedButton() : DebouncedButton(0) {}
};
static DebouncedButton blindsBtn(BLINDS_BTN_PIN);
static DebouncedButton lightBtn(DESK_LED_BTN_PIN);
static DebouncedButton nightBtn(NIGHT_BTN_PIN);

static bool fallingEdge(DebouncedButton &b) {
  bool raw = digitalRead(b.pin);
  uint32_t now = millis();

  if (raw != b.last_raw) {
    b.last_raw = raw;
    b.last_change = now;
  }

  if ((now - b.last_change) >= DEBOUNCE_MS && raw != b.stable) {
    b.stable = raw;
    if (b.stable == LOW) {       // pressed (pull-up)
      while (digitalRead(b.pin) == LOW) delay(1); // wait release
      delay(10);
      return true;
    }
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nTTGO main controller booting...");

  wifi_second_chan_t sc;
  esp_wifi_get_channel(&ESPNOW_PRIMARY_CH, &sc);

  pinMode(BLINDS_BTN_PIN, INPUT_PULLUP);
  pinMode(DESK_LED_BTN_PIN, INPUT_PULLUP);
  pinMode(NIGHT_BTN_PIN, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (true) delay(1000);
  }
  esp_now_register_send_cb(onSend);

  // Add motor peer
  { esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, motorPeerMac, 6);
    p.channel = 0; p.encrypt = false;
    if (esp_now_add_peer(&p) != ESP_OK) Serial.println("add_peer (motor) failed");
    else Serial.printf("Motor peer: %s\n", macToStr(motorPeerMac).c_str());
  }
  // Add light peer
  { esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, lightPeerMac, 6);
    p.channel = 0; p.encrypt = false;
    if (esp_now_add_peer(&p) != ESP_OK) Serial.println("add_peer (light) failed");
    else Serial.printf("Light peer: %s\n", macToStr(lightPeerMac).c_str());
  }
}

void loop() {
  static bool dir_toggle = false;
  

  if (nightmode) {
    lightVal = analogRead(LDR_PIN);
    if (lightVal > LDR_OFF_THR && ledAssumedOn) {
      sendLightToggle();
    } else if (lightVal < LDR_ON_THR && !ledAssumedOn) {
      sendLightToggle();
    }
  }

  if (fallingEdge(blindsBtn)) {
    dir_toggle = !dir_toggle;
    int32_t steps = dir_toggle ? STEPS_PER_TAP : -STEPS_PER_TAP;
    sendMove(steps, US_PER_STEP, 0, true);
  }

  if (fallingEdge(lightBtn)) {
    sendLightToggle();
  }

  if (fallingEdge(nightBtn)) {
    nightmode = !nightmode;
    Serial.printf("Night mode -> %s\n", nightmode ? "ON" : "OFF");
  }

  delay(1);
}
