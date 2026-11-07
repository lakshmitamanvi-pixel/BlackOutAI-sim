#include <OneWire.h>
#include <DallasTemperature.h>

// ================= CONFIG =================
const bool DEMO_MODE = true;              // true: 1 s = 1 sim min | false: real time (real prototype)

const int PIN_MAINS = 34, PIN_TEMP = 4, PIN_BTN = 12;
const int PIN_RED = 25, PIN_YEL = 26, PIN_GRN = 27;

const int      OUTAGE_ADC_BELOW   = 1000; // ADC < this = outage
const uint32_t OUTAGE_DEBOUNCE_MS = 2000;
const float    FRIDGE_LIMIT_C     = 5.0;
const int      GRID_FAULT_MIN_DARK = 5;
const int      SUPERCAP_SECONDS   = 30;

// illustrative, not validated
const unsigned long KEEP_UNDER_MIN      = 120;
const unsigned long EAT_TODAY_UNDER_MIN = 240;

// ================= LOGIC (pure: no pins, no Serial) =================
enum Crowd { CROWD_NONE, GRID_FAULT, HOUSE_FAULT };
enum Food  { FOOD_KEEP, FOOD_EAT_TODAY, FOOD_DISCARD };

struct MainsState {
  bool lost = false;
  bool pendingLow = false;
  uint32_t lowSince = 0;
};
struct FridgeTimer {
  uint32_t aboveMs = 0;
};

// returns true if state changed
bool mainsUpdate(MainsState &s, int adc, uint32_t now) {
  bool rawLow = adc < OUTAGE_ADC_BELOW;
  bool prev = s.lost;
  if (rawLow) {
    if (!s.pendingLow) {
      s.pendingLow = true;
      s.lowSince = now;
    }
    if (!s.lost && now - s.lowSince >= OUTAGE_DEBOUNCE_MS) s.lost = true;
  } else {
    s.pendingLow = false;
    s.lost = false; // restore instant; add debounce here too if mains is noisy
  }
  return s.lost != prev;
}

Crowd crowdVerdict(bool outage, int darkCount) {
  if (!outage) return CROWD_NONE;
  return darkCount >= GRID_FAULT_MIN_DARK ? GRID_FAULT : HOUSE_FAULT;
}

void fridgeUpdate(FridgeTimer &f, bool outage, float tempC, uint32_t dtMs) {
  if (outage && tempC > FRIDGE_LIMIT_C) f.aboveMs += dtMs;
  else if (!outage && tempC < FRIDGE_LIMIT_C) f.aboveMs = 0;
  // power back but temp >= 5C: timer holds (food still warm)
}

unsigned long aboveMinutes(const FridgeTimer &f) {
  return f.aboveMs / (DEMO_MODE ? 1000UL : 60000UL);
}

Food foodVerdict(unsigned long minutes) {
  if (minutes < KEEP_UNDER_MIN)      return FOOD_KEEP;
  if (minutes < EAT_TODAY_UNDER_MIN) return FOOD_EAT_TODAY;
  return FOOD_DISCARD;
}

// ================= SENSORS =================
OneWire oneWire(PIN_TEMP);
DallasTemperature ds(&oneWire);
const int DARK_STEPS[] = {0, 3, 8, 15};
int darkIdx = 0;

// REAL HW: replace with ADC from mains-sensing divider/optocoupler (isolated!), keep return = raw ADC
int readMains() {
  return analogRead(PIN_MAINS);
}

// REAL HW: same DS18B20 in sealed probe in fridge; add 4.7k pull-up DQ->3V3. Code unchanged.
float readFridgeC() {
  static uint32_t lastReq = 0;
  static float last = 25.0;
  uint32_t now = millis();
  if (now - lastReq >= 800) {            // non-blocking: conversion ~750 ms
    float t = ds.getTempCByIndex(0);
    if (t != DEVICE_DISCONNECTED_C) last = t;
    ds.requestTemperatures();
    lastReq = now;
  }
  return last;
}

// REAL HW: replace with count from crowd backend (WiFi/GSM API); button logic removed
int readNeighbourDark() {
  static bool prev = HIGH;
  static uint32_t lastChg = 0;
  bool b = digitalRead(PIN_BTN);
  if (b != prev && millis() - lastChg > 50) {
    lastChg = millis();
    prev = b;
    if (b == LOW) darkIdx = (darkIdx + 1) % 4;
  }
  return DARK_STEPS[darkIdx];
}

// ================= OUTPUT =================
void outLeds(bool outage, Crowd c) {
  digitalWrite(PIN_GRN, !outage);
  digitalWrite(PIN_RED, outage && c == GRID_FAULT);
  digitalWrite(PIN_YEL, outage && c == HOUSE_FAULT);
}

void outCrowdAction(Crowd c) {
  if (c == GRID_FAULT)  Serial.println("[MOCK] SMS sent to DISCOM");
  if (c == HOUSE_FAULT) Serial.println("Check MCB, safe steps...");
}

const char* crowdStr(Crowd c) {
  return c == GRID_FAULT ? "GRID FAULT" : c == HOUSE_FAULT ? "HOUSE FAULT" : "NONE";
}
const char* foodStr(Food f) {
  return f == FOOD_KEEP ? "KEEP" : f == FOOD_EAT_TODAY ? "EAT TODAY" : "DISCARD";
}

void outLine(uint32_t tSec, bool outage, int dark, Crowd c, float temp, unsigned long mins, Food f, int cap) {
  Serial.printf("[t=%us] mains=%s neighbours_dark=%d -> %s | fridge=%.1fC above5C=%lumin -> %s (advisory)",
                tSec, outage ? "LOST" : "OK", dark, crowdStr(c), temp, mins, foodStr(f));
  if (outage) Serial.printf(" | supercap=%ds", cap);
  Serial.println();
}

// ================= MAIN =================
MainsState mains;
FridgeTimer fridge;
Crowd lastCrowd = CROWD_NONE;
uint32_t lastLoop = 0, lastPrint = 0, outageStart = 0;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_RED, OUTPUT); pinMode(PIN_YEL, OUTPUT); pinMode(PIN_GRN, OUTPUT);
  ds.begin();
  ds.setWaitForConversion(false);
  ds.requestTemperatures();
  lastLoop = millis();
  Serial.printf("Power-Cut Companion | DEMO_MODE=%s\n", DEMO_MODE ? "true" : "false");
}

void loop() {
  uint32_t now = millis();
  uint32_t dt = now - lastLoop;
  lastLoop = now;

  int adc = readMains();
  float temp = readFridgeC();
  int dark = readNeighbourDark();

  bool changed = mainsUpdate(mains, adc, now);
  if (changed && mains.lost) {
    outageStart = now;
    Serial.println("supercap holding, node alive");
  }
  if (changed && !mains.lost) {
    Serial.println("Mains restored");
    lastCrowd = CROWD_NONE;
  }

  Crowd c = crowdVerdict(mains.lost, dark);
  if (c != lastCrowd) {           // fires on outage start AND if neighbour count flips verdict
    outCrowdAction(c);
    lastCrowd = c;
  }

  fridgeUpdate(fridge, mains.lost, temp, dt);
  outLeds(mains.lost, c);

  if (now - lastPrint >= 1000) {
    lastPrint += 1000;
    int cap = 0;
    if (mains.lost) {
      uint32_t el = (now - outageStart) / 1000;
      cap = el >= SUPERCAP_SECONDS ? 0 : SUPERCAP_SECONDS - el;
    }
    unsigned long m = aboveMinutes(fridge);
    outLine(now / 1000, mains.lost, dark, c, temp, m, foodVerdict(m), cap);
  }
}