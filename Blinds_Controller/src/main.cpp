#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

// ---- Pins ----
static const int PIN_EN   = 25;  // enable
static const int PIN_STEP = 22;  // step
static const int PIN_DIR  = 21;  // dir

// Most drivers use EN active-LOW. Set false if yours is active-HIGH.
static const bool EN_ACTIVE_LOW = true;

// ---- Command payload ----
struct __attribute__((packed)) Command {
  int32_t  steps;        // +/- step count; sign used if dir==0
  uint16_t us_per_step;  // microseconds per step period
  int8_t   dir;          // 1=CW, -1=CCW, 0=use sign of steps
  uint8_t  enable;       // 1=enable driver, 0=disable
};

volatile bool have_cmd = false;
Command latest; // not volatile; we copy it inside a critical section

inline void enableDriver(bool on) {
  digitalWrite(PIN_EN, EN_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
}

void moveStepsBlocking(int32_t steps, uint16_t us_per_step, int8_t dir_hint) {
  if (steps == 0) return;

  bool dir_cw = (dir_hint > 0) ? true : (dir_hint < 0) ? false : (steps >= 0);
  digitalWrite(PIN_DIR, dir_cw ? HIGH : LOW);

  uint32_t n = steps >= 0 ? steps : -steps;

  // conservative limits
  if (us_per_step < 200)   us_per_step = 200;    // ~2.5 kHz
  if (us_per_step > 50000) us_per_step = 50000;  // 20 Hz
  uint16_t half = us_per_step / 2;

  for (uint32_t i = 0; i < n; ++i) {
    digitalWrite(PIN_STEP, HIGH);
    delayMicroseconds(half);
    digitalWrite(PIN_STEP, LOW);
    delayMicroseconds(us_per_step - half);
  }
}

// ---- Legacy ESP-NOW callback (works on all cores) ----
void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len < (int)sizeof(Command)) return;
  noInterrupts();
  memcpy(&latest, data, sizeof(Command));
  have_cmd = true;
  interrupts();
}

void setup() {
  pinMode(PIN_EN, OUTPUT);
  pinMode(PIN_STEP, OUTPUT);
  pinMode(PIN_DIR, OUTPUT);
  digitalWrite(PIN_STEP, LOW);
  digitalWrite(PIN_DIR,  LOW);
  enableDriver(false);

  Serial.begin(115200);
  delay(300);
  Serial.println("\nMotor node booting...");

  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onRecv);
  Serial.println("ESP-NOW receiver ready.");
}

void loop() {
  if (have_cmd) {
    noInterrupts();
    Command c = latest;
    have_cmd = false;
    interrupts();

    Serial.printf("CMD: steps=%ld  us=%u  dir=%d  en=%u\n",
                  (long)c.steps, c.us_per_step, c.dir, c.enable);

    enableDriver(c.enable != 0);
    if (c.enable) {
      moveStepsBlocking(c.steps, c.us_per_step, c.dir);
      Serial.println("Move complete.");
      enableDriver(false); // optionally auto-disable after move
    }
  }
  delay(1);
}