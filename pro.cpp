#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// =============================================================================
// PIN MAPPING
// =============================================================================

// Traffic Light A
#define A_RED       PA0
#define A_YELLOW    PA1
#define A_GREEN     PA2

// Traffic Light B
#define B_RED       PA3
#define B_YELLOW    PA4
#define B_GREEN     PA5

// Audio / Noise Sensor
#define NOISE_PIN   PA6

// STM32 Blue Pill onboard LED (active LOW)
#define ONBOARD_LED PC13


// =============================================================================
// OLED DISPLAY CONFIGURATION
// =============================================================================

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  -1
);


// =============================================================================
// TRAFFIC LIGHT TIMING
// =============================================================================

// Default green duration
#define GREEN_DEFAULT_SEC 15

// Minimum and maximum allowed green duration
#define GREEN_MIN_SEC      5
#define GREEN_MAX_SEC      30

// Fixed yellow duration
#define YELLOW_SEC         3

// Seconds added when a noise event is detected
#define ADAPT_STEP_SEC     2

// Minimum time between two timer extensions
#define ADAPT_COOLDOWN_MS  2000


// =============================================================================
// NOISE / AUDIO SENSOR CONFIGURATION
// =============================================================================

// ADC threshold above which a sound is considered a noise event.
//
// STM32 ADC is configured for 12-bit resolution:
// 0 - 4095
//
// IMPORTANT:
// Calibrate this value on the actual hardware.
uint16_t NOISE_THRESHOLD = 1200;

// Number of ADC readings averaged together.
// Higher values reduce random spikes but make the reading slower.
#define ADC_SAMPLES 16


// =============================================================================
// TRAFFIC STATE MACHINE
// =============================================================================
//
// Traffic cycle:
//
// A GREEN
//     ↓
// A YELLOW
//     ↓
// B GREEN
//     ↓
// B YELLOW
//     ↓
// A GREEN
//     ↓
// ...

enum TrafficState {
  A_GREEN_ST,
  A_YELLOW_ST,
  B_GREEN_ST,
  B_YELLOW_ST
};

TrafficState state = A_GREEN_ST;


// =============================================================================
// RUNTIME VARIABLES
// =============================================================================

// Seconds remaining in the current traffic phase.
uint32_t phase_remaining = 0;

// Base green durations for each road.
//
// These are reset to GREEN_DEFAULT_SEC whenever a new green phase starts.
uint32_t road_a_green_time = GREEN_DEFAULT_SEC;
uint32_t road_b_green_time = GREEN_DEFAULT_SEC;

// Latest raw ADC reading from the noise sensor.
uint16_t noise_raw = 0;

// Smoothed noise value.
//
// Used only for displaying the value on the OLED.
uint16_t noise_filtered = 0;

// Time at which the last timer extension occurred.
uint32_t last_adapt_time = 0;

// Total seconds added to the current green phase by audio events.
// Resets to 0 at the start of every new green phase.
// Displayed on the OLED as "+Xs" so the extension amount is visible.
uint32_t green_time_added = 0;

// Indicates that a valid noise event was detected during the
// current green phase.
bool noise_triggered = false;

// Used for the one-second timer.
uint32_t last_second = 0;


// =============================================================================
// HELPER: TURN OFF ALL TRAFFIC LIGHTS
// =============================================================================

void allLightsOff() {

  digitalWrite(A_RED, LOW);
  digitalWrite(A_YELLOW, LOW);
  digitalWrite(A_GREEN, LOW);

  digitalWrite(B_RED, LOW);
  digitalWrite(B_YELLOW, LOW);
  digitalWrite(B_GREEN, LOW);
}


// =============================================================================
// HELPER: APPLY TRAFFIC LIGHT PATTERN
// =============================================================================

void applyLights(TrafficState s) {

  // Always turn everything off before applying the new state.
  allLightsOff();

  switch (s) {

    // -------------------------------------------------------------------------
    // Road A GREEN
    // Road B RED
    // -------------------------------------------------------------------------
    case A_GREEN_ST:
      digitalWrite(A_GREEN, HIGH);
      digitalWrite(B_RED, HIGH);
      break;

    // -------------------------------------------------------------------------
    // Road A YELLOW
    // Road B RED
    // -------------------------------------------------------------------------
    case A_YELLOW_ST:
      digitalWrite(A_YELLOW, HIGH);
      digitalWrite(B_RED, HIGH);
      break;

    // -------------------------------------------------------------------------
    // Road B GREEN
    // Road A RED
    // -------------------------------------------------------------------------
    case B_GREEN_ST:
      digitalWrite(A_RED, HIGH);
      digitalWrite(B_GREEN, HIGH);
      break;

    // -------------------------------------------------------------------------
    // Road B YELLOW
    // Road A RED
    // -------------------------------------------------------------------------
    case B_YELLOW_ST:
      digitalWrite(A_RED, HIGH);
      digitalWrite(B_YELLOW, HIGH);
      break;
  }
}


// =============================================================================
// HELPER: ROAD A STATUS STRING
// =============================================================================

const char* roadAStatus(TrafficState s) {

  switch (s) {

    case A_GREEN_ST:
      return "GREEN";

    case A_YELLOW_ST:
      return "YELLOW";

    default:
      return "RED";
  }
}


// =============================================================================
// HELPER: ROAD B STATUS STRING
// =============================================================================

const char* roadBStatus(TrafficState s) {

  switch (s) {

    case B_GREEN_ST:
      return "GREEN";

    case B_YELLOW_ST:
      return "YELLOW";

    default:
      return "RED";
  }
}


// =============================================================================
// HELPER: GET CURRENT GREEN TIME
// =============================================================================

uint32_t currentGreenTime() {

  if (state == A_GREEN_ST) {
    return road_a_green_time;
  }

  if (state == B_GREEN_ST) {
    return road_b_green_time;
  }

  return 0;
}


// =============================================================================
// HELPER: READ AND AVERAGE NOISE SENSOR
// =============================================================================
//
// Multiple ADC samples are taken and averaged to reduce electrical noise
// and random spikes.
//
// Returns:
//     Average ADC value from 0 to 4095.
// =============================================================================

uint16_t readNoiseAveraged() {

  uint32_t sum = 0;

  for (int i = 0; i < ADC_SAMPLES; i++) {

    sum += analogRead(NOISE_PIN);
  }

  return (uint16_t)(sum / ADC_SAMPLES);
}


// =============================================================================
// SMART AUDIO-BASED GREEN TIMER ADAPTATION
// =============================================================================
//
// Behavior:
//
// 1. Adaptation only happens during a GREEN phase.
// 2. A noise event must have been detected.
// 3. At least 2 seconds must have passed since the previous extension.
// 4. Green time can never exceed 30 seconds.
// 5. Each valid event adds 2 seconds.
//
// "Punishment" model:
//
// If the waiting road produces excessive noise/honking:
//
//       Current road = GREEN
//       Waiting road = RED
//
// The current green phase is extended.
//
// Therefore:
//
//       Waiting road stays RED for longer.
//
// This discourages unnecessary honking.
//
// =============================================================================

void tryAdaptGreenTime(uint32_t now) {

  // ---------------------------------------------------------------------------
  // Guard 1:
  // Only adapt during a GREEN phase.
  // ---------------------------------------------------------------------------

  if (state != A_GREEN_ST && state != B_GREEN_ST) {
    return;
  }


  // ---------------------------------------------------------------------------
  // Guard 2:
  // No noise event detected.
  // ---------------------------------------------------------------------------

  if (!noise_triggered) {
    return;
  }


  // ---------------------------------------------------------------------------
  // Guard 3:
  // Enforce the 2-second cooldown.
  //
  // This prevents multiple extensions from happening too quickly.
  // ---------------------------------------------------------------------------

  if (now - last_adapt_time < ADAPT_COOLDOWN_MS) {
    return;
  }


  // ---------------------------------------------------------------------------
  // Guard 4:
  // Do not extend if the current phase is already at maximum.
  // ---------------------------------------------------------------------------

  if (phase_remaining >= GREEN_MAX_SEC) {

    noise_triggered = false;

    return;
  }


  // ---------------------------------------------------------------------------
  // Extend the current green phase by 2 seconds.
  // ---------------------------------------------------------------------------

  phase_remaining  += ADAPT_STEP_SEC;
  green_time_added += ADAPT_STEP_SEC;  // track cumulative extension for OLED display


  // ---------------------------------------------------------------------------
  // Safety limit:
  // Never allow the timer to exceed GREEN_MAX_SEC.
  // ---------------------------------------------------------------------------

  if (phase_remaining > GREEN_MAX_SEC) {
    phase_remaining = GREEN_MAX_SEC;
  }


  // ---------------------------------------------------------------------------
  // Record the time of this adaptation.
  // ---------------------------------------------------------------------------

  last_adapt_time = now;


  // ---------------------------------------------------------------------------
  // Clear the event.
  //
  // Another extension requires another valid noise event.
  // ---------------------------------------------------------------------------

  noise_triggered = false;
}


// =============================================================================
// OLED STATUS DISPLAY
// =============================================================================
//
// Displays:
//
// SMART TRAFFIC
// A: GREEN/RED/YELLOW
// B: GREEN/RED/YELLOW
// Timer
// Noise level
// Current green durations
//
// =============================================================================

void showStatus() {

  display.clearDisplay();

  display.setTextColor(SSD1306_WHITE);


  // ---------------------------------------------------------------------------
  // Title
  // ---------------------------------------------------------------------------

  display.setTextSize(1);
  display.setCursor(0, 0);

  display.println("SMART TRAFFIC");


  // ---------------------------------------------------------------------------
  // Road A status
  // ---------------------------------------------------------------------------

  display.setCursor(0, 14);

  display.print("A: ");
  display.println(roadAStatus(state));


  // ---------------------------------------------------------------------------
  // Road B status
  // ---------------------------------------------------------------------------

  display.setCursor(0, 24);

  display.print("B: ");
  display.println(roadBStatus(state));


  // ---------------------------------------------------------------------------
  // Countdown timer
  // ---------------------------------------------------------------------------

  display.setTextSize(2);

  display.setCursor(0, 36);

  display.print(phase_remaining);
  display.println("s");


  // ---------------------------------------------------------------------------
  // Debug information
  // ---------------------------------------------------------------------------

  display.setTextSize(1);

  display.setCursor(0, 54);

  display.print("N:");
  display.print(noise_filtered);

  display.print(" A:");
  display.print(road_a_green_time);

  display.print(" B:");
  display.println(road_b_green_time);


  // Send everything to OLED.
  display.display();
}


// =============================================================================
// STATE TRANSITION
// =============================================================================
//
// Called whenever phase_remaining reaches zero.
//
// Traffic sequence:
//
// A GREEN
//     → A YELLOW
//     → B GREEN
//     → B YELLOW
//     → A GREEN
//
// Every new green phase starts at the default 15 seconds.
//
// =============================================================================

void enterNextState() {

  switch (state) {

    // -------------------------------------------------------------------------
    // A GREEN → A YELLOW
    // -------------------------------------------------------------------------

    case A_GREEN_ST:

      state = A_YELLOW_ST;

      phase_remaining = YELLOW_SEC;

      break;


    // -------------------------------------------------------------------------
    // A YELLOW → B GREEN
    // -------------------------------------------------------------------------

    case A_YELLOW_ST:

      state = B_GREEN_ST;

      // Start B's green phase from the default duration.
      road_b_green_time = GREEN_DEFAULT_SEC;

      phase_remaining = road_b_green_time;

      // Reset noise adaptation and extension counter for the new green phase.
      noise_triggered  = false;
      last_adapt_time  = 0;
      green_time_added = 0;

      break;


    // -------------------------------------------------------------------------
    // B GREEN → B YELLOW
    // -------------------------------------------------------------------------

    case B_GREEN_ST:

      state = B_YELLOW_ST;

      phase_remaining = YELLOW_SEC;

      break;


    // -------------------------------------------------------------------------
    // B YELLOW → A GREEN
    // -------------------------------------------------------------------------

    case B_YELLOW_ST:

      state = A_GREEN_ST;

      // Start A's green phase from the default duration.
      road_a_green_time = GREEN_DEFAULT_SEC;

      phase_remaining = road_a_green_time;

      // Reset noise adaptation and extension counter for the new green phase.
      noise_triggered  = false;
      last_adapt_time  = 0;
      green_time_added = 0;

      break;
  }


  // Apply the new traffic light state.
  applyLights(state);
}


// =============================================================================
// SETUP
// =============================================================================

void setup() {

  // ---------------------------------------------------------------------------
  // Configure traffic light pins as outputs
  // ---------------------------------------------------------------------------

  pinMode(A_RED, OUTPUT);
  pinMode(A_YELLOW, OUTPUT);
  pinMode(A_GREEN, OUTPUT);

  pinMode(B_RED, OUTPUT);
  pinMode(B_YELLOW, OUTPUT);
  pinMode(B_GREEN, OUTPUT);


  // ---------------------------------------------------------------------------
  // Configure noise sensor
  // ---------------------------------------------------------------------------

  pinMode(NOISE_PIN, INPUT_ANALOG);


  // ---------------------------------------------------------------------------
  // Configure onboard LED
  // ---------------------------------------------------------------------------

  pinMode(ONBOARD_LED, OUTPUT);


  // ---------------------------------------------------------------------------
  // Configure STM32 ADC resolution
  // 12-bit ADC = values from 0 to 4095
  // ---------------------------------------------------------------------------

  analogReadResolution(12);


  // ---------------------------------------------------------------------------
  // Turn all traffic lights off initially.
  // ---------------------------------------------------------------------------

  allLightsOff();


  // ---------------------------------------------------------------------------
  // Initialize I2C and OLED.
  // ---------------------------------------------------------------------------

  Wire.begin();

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {

    // If OLED initialization fails, blink the onboard LED forever.
    while (1) {

      digitalWrite(
        ONBOARD_LED,
        !digitalRead(ONBOARD_LED)
      );

      delay(100);
    }
  }


  // ---------------------------------------------------------------------------
  // Initialize traffic system.
  // ---------------------------------------------------------------------------

  state = A_GREEN_ST;

  road_a_green_time = GREEN_DEFAULT_SEC;
  road_b_green_time = GREEN_DEFAULT_SEC;

  // Start with Road A green.
  phase_remaining = road_a_green_time;

  noise_triggered  = false;
  last_adapt_time  = 0;
  green_time_added = 0;


  // ---------------------------------------------------------------------------
  // Apply initial traffic light state.
  // ---------------------------------------------------------------------------

  applyLights(state);


  // ---------------------------------------------------------------------------
  // Display initial status.
  // ---------------------------------------------------------------------------

  showStatus();


  // ---------------------------------------------------------------------------
  // Start one-second timer.
  // ---------------------------------------------------------------------------

  last_second = millis();
}


// =============================================================================
// MAIN LOOP
// =============================================================================
//
// The loop runs continuously, but the traffic logic executes once every
// approximately 1 second.
//
// =============================================================================

void loop() {

  uint32_t now = millis();


  // ===========================================================================
  // ONE-SECOND TICK
  // ===========================================================================

  if (now - last_second >= 1000) {

    // Advance by exactly one second to minimize timing drift.
    last_second += 1000;


    // =========================================================================
    // 1. READ NOISE SENSOR
    // =========================================================================

    noise_raw = readNoiseAveraged();


    // =========================================================================
    // 2. SMOOTH NOISE VALUE FOR OLED DISPLAY
    // =========================================================================
    //
    // Formula:
    //
    // filtered = (filtered * 3 + raw) / 4
    //
    // This gives approximately:
    //
    // 75% previous value
    // 25% new value
    //
    // NOTE:
    // The filtered value is NOT used for adaptation decisions.
    // The raw averaged ADC value is used instead.
    // =========================================================================

    noise_filtered =
      (noise_filtered * 3 + noise_raw) / 4;


    // =========================================================================
    // 3. DETECT NOISE EVENT
    // =========================================================================
    //
    // A noise event is only relevant during a GREEN phase.
    //
    // If the raw sensor value crosses the threshold, arm the adaptation flag.
    // =========================================================================

    if (
      (state == A_GREEN_ST || state == B_GREEN_ST) &&
      noise_raw >= NOISE_THRESHOLD
    ) {

      noise_triggered = true;
    }


    // =========================================================================
    // 4. ATTEMPT GREEN TIMER EXTENSION
    // =========================================================================

    tryAdaptGreenTime(now);


    // =========================================================================
    // 5. DECREASE PHASE TIMER
    // =========================================================================

    if (phase_remaining > 0) {

      phase_remaining--;
    }


    // =========================================================================
    // 6. CHECK FOR STATE TRANSITION
    // =========================================================================

    if (phase_remaining == 0) {

      enterNextState();
    }


    // =========================================================================
    // 7. HEARTBEAT LED
    // =========================================================================
    //
    // Toggles the onboard LED once per second.
    // This provides a simple indication that the controller is running.
    // =========================================================================

    digitalWrite(
      ONBOARD_LED,
      !digitalRead(ONBOARD_LED)
    );


    // =========================================================================
    // 8. REFRESH OLED
    // =========================================================================

    showStatus();
  }
}