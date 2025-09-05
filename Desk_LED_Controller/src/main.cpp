#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

// ---------------- Pins ----------------
#define LIGHT_BUTTON     35   // needs external pull-up! (or change to 27)
#define LIGHT_UP_BUTTON  32
#define LIGHT_DOWN_BUTTON 33
#define GATE_PIN         22   // PWM to MOSFET gate (inverted output)

// -------------- PWM setup -------------
const int pwmChannel   = 0;
const int pwmFreq      = 5000;  // 5 kHz
const int pwmResolution= 8;     // 0..255

// -------------- State -----------------
bool toggleState = true;     // ON/OFF
int  brightness  = 255;      // 0..255
const int STEP   = 25;       // local +/- step
const uint32_t DEBOUNCE_MS = 30;

// -------------- Helpers ---------------
static inline void applyBrightness(int level) {
  level = constrain(level, 0, 255);
  brightness  = level;
  toggleState = (level > 0);
  // Low-side with inverted duty (0=off, 255=full)
  ledcWrite(pwmChannel, 255 - level);
}

static inline bool fallingEdge(uint8_t pin) {
  struct Btn { uint8_t pin; bool last; uint32_t t; };
  static Btn b35 = {LIGHT_BUTTON, HIGH, 0};
  static Btn b32 = {LIGHT_UP_BUTTON, HIGH, 0};
  static Btn b33 = {LIGHT_DOWN_BUTTON, HIGH, 0};

  Btn* b = (pin == LIGHT_BUTTON) ? &b35 : (pin == LIGHT_UP_BUTTON) ? &b32 : &b33;

  bool raw = digitalRead(pin);
  uint32_t now = millis();
  if (raw != b->last) { b->last = raw; b->t = now; }
  if ((now - b->t) >= DEBOUNCE_MS && raw == LOW) {
    // wait until released before reporting another edge
    while (digitalRead(pin) == LOW) delay(1);
    delay(10);
    return true;
  }
  return false;
}

// --------- ESP-NOW command payload ----
// Keep this tiny; matches what the TTGO will send.
struct __attribute__((packed)) LightCmd {
  uint8_t action;   // 1=ON, 2=OFF, 3=TOGGLE, 4=SET_BRIGHTNESS
  uint8_t value;    // used only for action 4 (0..255)
};

volatile bool have_cmd = false;
LightCmd latest;

void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len < (int)sizeof(LightCmd)) return;
  noInterrupts();
  memcpy((void*)&latest, data, sizeof(LightCmd));
  have_cmd = true;
  interrupts();
}

static inline void handleCmd(const LightCmd& c) {
  switch (c.action) {
    case 1: // ON
      applyBrightness(brightness == 0 ? 255 : brightness);
      break;
    case 2: // OFF
      applyBrightness(0);
      break;
    case 3: // TOGGLE
      applyBrightness(toggleState ? 0 : (brightness == 0 ? 255 : brightness));
      break;
    case 4: // SET_BRIGHTNESS
      applyBrightness(c.value);
      break;
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  // Buttons
  pinMode(LIGHT_UP_BUTTON,   INPUT_PULLUP);
  pinMode(LIGHT_DOWN_BUTTON, INPUT_PULLUP);
  // LIGHT_BUTTON (GPIO35) has no internal pull-ups:
  pinMode(LIGHT_BUTTON,      INPUT);  // expect external pull-up if using 35

  // PWM
  ledcSetup(pwmChannel, pwmFreq, pwmResolution);
  ledcAttachPin(GATE_PIN, pwmChannel);
  applyBrightness(brightness);

  // ESP-NOW RX
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed"); 
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onRecv);

  Serial.println("LED node ready. Local buttons + ESP-NOW commands active.");
}

void loop() {
  // Handle remote command
  if (have_cmd) {
    noInterrupts(); LightCmd c = latest; have_cmd = false; interrupts();
    handleCmd(c);
    Serial.printf("CMD: action=%u value=%u -> brightness=%d, state=%s\n",
                  c.action, c.value, brightness, toggleState ? "ON":"OFF");
  }

  // Local controls
  if (fallingEdge(LIGHT_BUTTON)) {
    // Toggle
    applyBrightness(toggleState ? 0 : (brightness == 0 ? 255 : brightness));
  }

  if (fallingEdge(LIGHT_UP_BUTTON) && toggleState) {
    applyBrightness(brightness + STEP);
  }

  if (fallingEdge(LIGHT_DOWN_BUTTON)) {
    if (toggleState) {
      applyBrightness(brightness - STEP);
      if (brightness == 0) toggleState = false;
    } else {
      // If currently OFF, a "down" press can leave it OFF; no change
    }
  }

  delay(1);
}
