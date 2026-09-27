#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------- Pin mapping ----------
#define A_RED     PA0
#define A_YELLOW  PA1
#define A_GREEN   PA2
#define B_RED     PA3
#define B_YELLOW  PA4
#define B_GREEN   PA5
#define NOISE_PIN PA6
#define ONBOARD_LED PC13

// ---------- OLED ----------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------- Timing ----------
#define GREEN_DEFAULT_SEC 15
#define GREEN_MIN_SEC      5
#define GREEN_MAX_SEC     30
#define YELLOW_SEC          3
#define ADAPT_STEP_SEC      2

// Placeholder — MUST calibrate on real hardware
uint16_t NOISE_THRESHOLD = 1200;
#define ADC_SAMPLES 16

enum TrafficState { A_GREEN_ST, A_YELLOW_ST, B_GREEN_ST, B_YELLOW_ST };
TrafficState state = A_GREEN_ST;

uint32_t phase_remaining;
uint32_t road_a_green_time = GREEN_DEFAULT_SEC;
uint32_t road_b_green_time = GREEN_DEFAULT_SEC;
uint16_t noise_filtered = 0;

void allLightsOff() {
  digitalWrite(A_RED, LOW);   digitalWrite(A_YELLOW, LOW); digitalWrite(A_GREEN, LOW);
  digitalWrite(B_RED, LOW);   digitalWrite(B_YELLOW, LOW); digitalWrite(B_GREEN, LOW);
}

void applyLights(TrafficState s) {
  allLightsOff();
  switch (s) {
    case A_GREEN_ST:  digitalWrite(A_GREEN, HIGH);  digitalWrite(B_RED, HIGH);   break;
    case A_YELLOW_ST: digitalWrite(A_YELLOW, HIGH); digitalWrite(B_RED, HIGH);   break;
    case B_GREEN_ST:  digitalWrite(A_RED, HIGH);    digitalWrite(B_GREEN, HIGH); break;
    case B_YELLOW_ST: digitalWrite(A_RED, HIGH);    digitalWrite(B_YELLOW, HIGH);break;
  }
}

const char* roadAStatus(TrafficState s) {
  switch (s) {
    case A_GREEN_ST:  return "GREEN";
    case A_YELLOW_ST: return "YELLOW";
    default:          return "RED";
  }
}

const char* roadBStatus(TrafficState s) {
  switch (s) {
    case B_GREEN_ST:  return "GREEN";
    case B_YELLOW_ST: return "YELLOW";
    default:          return "RED";
  }
}

uint32_t currentGreenTime() {
  if (state == A_GREEN_ST) return road_a_green_time;
  if (state == B_GREEN_ST) return road_b_green_time;
  return 0;
}

uint16_t readNoiseAveraged() {
  uint32_t sum = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) sum += analogRead(NOISE_PIN);
  return sum / ADC_SAMPLES;
}

uint16_t noise_raw = 0;

// PUNISHMENT MODEL (one-way):
// Noise sensor picks up honking from whichever road is currently RED (waiting).
// High noise from the waiting/honking side -> EXTEND the active green road,
// which means the honking side's red gets LONGER (discourages honking).
// Once extended, it STAYS extended — no automatic recovery when quiet.
void adaptGreenTime() {
  if (noise_filtered >= NOISE_THRESHOLD) {
    if (state == A_GREEN_ST && road_a_green_time < GREEN_MAX_SEC) road_a_green_time += ADAPT_STEP_SEC;
    if (state == B_GREEN_ST && road_b_green_time < GREEN_MAX_SEC) road_b_green_time += ADAPT_STEP_SEC;
  }
  road_a_green_time = constrain(road_a_green_time, GREEN_MIN_SEC, GREEN_MAX_SEC);
  road_b_green_time = constrain(road_b_green_time, GREEN_MIN_SEC, GREEN_MAX_SEC);
}





void showStatus() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println("SMART TRAFFIC");

  display.setCursor(0, 14);
  display.print("A: "); display.println(roadAStatus(state));

  display.setCursor(0, 24);
  display.print("B: "); display.println(roadBStatus(state));

  display.setCursor(0, 36);
  display.setTextSize(2);
  display.print(phase_remaining); display.println("s");

  display.setTextSize(1);
  display.setCursor(0, 54);
  display.print("N:"); display.print(noise_filtered);
  display.print(" A:"); display.print(road_a_green_time);
  display.print(" B:"); display.println(road_b_green_time);

  display.display();
}

void enterNextState() {
  switch (state) {
    case A_GREEN_ST:
      state = A_YELLOW_ST;
      phase_remaining = YELLOW_SEC;
      break;

    case A_YELLOW_ST:
      state = B_GREEN_ST;
      road_b_green_time = GREEN_DEFAULT_SEC;   // always start fresh at 15
      phase_remaining = road_b_green_time;
      break;

    case B_GREEN_ST:
      state = B_YELLOW_ST;
      phase_remaining = YELLOW_SEC;
      break;

    case B_YELLOW_ST:
      state = A_GREEN_ST;
      road_a_green_time = GREEN_DEFAULT_SEC;   // always start fresh at 15
      phase_remaining = road_a_green_time;
      break;
  }
  applyLights(state);
}

uint32_t last_second = 0;

void setup() {
  pinMode(A_RED, OUTPUT);   pinMode(A_YELLOW, OUTPUT); pinMode(A_GREEN, OUTPUT);
  pinMode(B_RED, OUTPUT);   pinMode(B_YELLOW, OUTPUT); pinMode(B_GREEN, OUTPUT);
  pinMode(NOISE_PIN, INPUT_ANALOG);
  pinMode(ONBOARD_LED, OUTPUT);

  analogReadResolution(12); // STM32 ADC is 12-bit (0-4095)

  allLightsOff();

  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    while (1) {
      digitalWrite(ONBOARD_LED, !digitalRead(ONBOARD_LED));
      delay(100);
    }
  }

  state = A_GREEN_ST;
  road_a_green_time = GREEN_DEFAULT_SEC;
  road_b_green_time = GREEN_DEFAULT_SEC;
  phase_remaining = road_a_green_time;
  applyLights(state);
  showStatus();

  last_second = millis();
}

void loop() {
  uint32_t now = millis();

  if (now - last_second >= 1000) {
    last_second += 1000;

    noise_raw = readNoiseAveraged();          // fresh reading, used for the punishment decision
    noise_filtered = (noise_filtered * 3 + noise_raw) / 4;  // smoothed only for the OLED display

    if (state == A_GREEN_ST || state == B_GREEN_ST) {
      uint32_t oldGreen = currentGreenTime();
      adaptGreenTime();
      uint32_t newGreen = currentGreenTime();
      if (newGreen > oldGreen) {
        phase_remaining += (newGreen - oldGreen);
      }
    }

    if (phase_remaining > 0) phase_remaining--;
    if (phase_remaining == 0) enterNextState();

    digitalWrite(ONBOARD_LED, !digitalRead(ONBOARD_LED));
    showStatus();
  }
}