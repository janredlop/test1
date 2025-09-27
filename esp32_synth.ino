#include <Arduino.h>
#include <esp_system.h>
#include <math.h>

/**
 * Simple polyphonic synthesizer for ESP32 using the Arduino framework.
 *
 * Features:
 *  - Eight note buttons laid out as a C major scale (C4 to C5).
 *  - Supports holding multiple keys for polyphonic playback.
 *  - Instrument toggle button that cycles through piano, guitar, violin and
 *    a fantasy synthesizer voice.
 *  - Rhythm toggle button that cycles through a collection of preset groove
 *    patterns which play alongside the notes.
 *  - Audio is generated on the ESP32 DAC (GPIO 25) using a timer interrupt.
 */

// ---------------------------------------------------------------------------
// Audio configuration
// ---------------------------------------------------------------------------

constexpr uint32_t SAMPLE_RATE = 22050;          // Audio sample rate in Hz
constexpr uint8_t DAC_PIN = 25;                  // Built-in DAC channel 1
constexpr uint8_t TIMER_INDEX = 0;               // Hardware timer to use
constexpr uint16_t TIMER_DIVIDER = 80;           // 1 µs per timer tick
constexpr uint32_t TIMER_INTERVAL_US = 1000000UL / SAMPLE_RATE;
constexpr float TWO_PI = 2.0f * static_cast<float>(M_PI);

// ---------------------------------------------------------------------------
// Musical configuration
// ---------------------------------------------------------------------------

constexpr size_t NUM_NOTES = 8; // C major scale

// GPIO pins connected to note buttons (active LOW, uses INPUT_PULLUP).
constexpr uint8_t NOTE_PINS[NUM_NOTES] = {32, 33, 27, 14, 12, 13, 15, 4};

// Frequencies (Hz) for C major scale starting at middle C (C4).
constexpr float NOTE_FREQUENCIES[NUM_NOTES] = {
    261.63f, // C4
    293.66f, // D4
    329.63f, // E4
    349.23f, // F4
    392.00f, // G4
    440.00f, // A4
    493.88f, // B4
    523.25f  // C5
};

// Instrument and rhythm control buttons (active LOW, uses INPUT_PULLUP).
constexpr uint8_t INSTRUMENT_BUTTON_PIN = 18;
constexpr uint8_t RHYTHM_BUTTON_PIN = 19;

// Debounce time for all buttons in milliseconds.
constexpr uint16_t DEBOUNCE_MS = 25;

// ---------------------------------------------------------------------------
// Instrument definitions
// ---------------------------------------------------------------------------

enum Instrument : uint8_t {
  INSTRUMENT_PIANO = 0,
  INSTRUMENT_GUITAR,
  INSTRUMENT_VIOLIN,
  INSTRUMENT_FANTASY,
  NUM_INSTRUMENTS
};

// ---------------------------------------------------------------------------
// Rhythm pattern definitions
// ---------------------------------------------------------------------------

struct RhythmPattern {
  const float *steps;    // Step intensities (0..1)
  uint8_t length;        // Number of steps in the pattern
  float bpm;             // Tempo for the pattern
};

constexpr uint8_t MAX_RHYTHM_STEPS = 16;

// Intensity arrays for rhythm patterns (stored in flash).
constexpr float RHYTHM_PATTERN_1[MAX_RHYTHM_STEPS] = {
    1.0f, 0.0f, 0.5f, 0.0f,
    0.8f, 0.0f, 0.4f, 0.0f,
    1.0f, 0.0f, 0.5f, 0.0f,
    0.6f, 0.0f, 0.3f, 0.0f};

constexpr float RHYTHM_PATTERN_2[MAX_RHYTHM_STEPS] = {
    1.0f, 0.0f, 0.7f, 0.0f,
    0.0f, 0.5f, 0.0f, 0.4f,
    0.9f, 0.0f, 0.6f, 0.0f,
    0.0f, 0.5f, 0.0f, 0.3f};

constexpr float RHYTHM_PATTERN_3[MAX_RHYTHM_STEPS] = {
    1.0f, 0.0f, 0.6f, 0.0f,
    0.7f, 0.0f, 0.5f, 0.0f,
    0.9f, 0.0f, 0.4f, 0.0f,
    0.7f, 0.0f, 0.5f, 0.0f};

constexpr RhythmPattern RHYTHMS[] = {
    {RHYTHM_PATTERN_1, 16, 92.0f},
    {RHYTHM_PATTERN_2, 16, 108.0f},
    {RHYTHM_PATTERN_3, 16, 124.0f},
};

constexpr int NUM_RHYTHMS = sizeof(RHYTHMS) / sizeof(RHYTHMS[0]);

// ---------------------------------------------------------------------------
// Button handling helper structure
// ---------------------------------------------------------------------------

struct ButtonState {
  uint8_t pin;
  bool lastReading;
  bool stableState;
  uint32_t lastDebounce;
};

ButtonState noteButtons[NUM_NOTES];
ButtonState instrumentButton{INSTRUMENT_BUTTON_PIN, false, false, 0};
ButtonState rhythmButton{RHYTHM_BUTTON_PIN, false, false, 0};

// ---------------------------------------------------------------------------
// Shared state between loop() and timer interrupt
// ---------------------------------------------------------------------------

portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

volatile bool noteActive[NUM_NOTES] = {false};
volatile float noteEnvelope[NUM_NOTES] = {0.0f};
float notePhase[NUM_NOTES] = {0.0f};

volatile Instrument currentInstrument = INSTRUMENT_PIANO;

volatile bool rhythmEnabled = false;
volatile int activeRhythm = -1; // -1 = off
volatile uint8_t rhythmStep = 0;
volatile float rhythmEnvelope = 0.0f;
volatile uint32_t nextRhythmStepSample = 0;
volatile uint32_t rhythmStepIntervalSamples = 1;
volatile uint32_t sampleCounter = 0;
volatile float rhythmPhase = 0.0f;

hw_timer_t *audioTimer = nullptr;

// ---------------------------------------------------------------------------
// Utility functions
// ---------------------------------------------------------------------------

bool updateButton(ButtonState &button, uint32_t nowMillis) {
  const bool reading = (digitalRead(button.pin) == LOW);

  if (reading != button.lastReading) {
    button.lastDebounce = nowMillis;
  }

  if ((nowMillis - button.lastDebounce) > DEBOUNCE_MS) {
    if (reading != button.stableState) {
      button.stableState = reading;
      button.lastReading = reading;
      return true; // state changed after debounce
    }
  }

  button.lastReading = reading;
  return false;
}

float applyInstrumentWave(Instrument instrument, float phase) {
  const float angle = TWO_PI * phase;

  switch (instrument) {
    case INSTRUMENT_PIANO:
      return sinf(angle);
    case INSTRUMENT_GUITAR:
      return 0.7f * sinf(angle) + 0.3f * sinf(2.0f * angle);
    case INSTRUMENT_VIOLIN:
      return 0.6f * sinf(angle) + 0.25f * sinf(3.0f * angle);
    case INSTRUMENT_FANTASY:
      return 0.4f * sinf(angle) + 0.4f * sinf(2.0f * angle + sinf(5.0f * angle)) +
             0.2f * sinf(0.5f * angle);
    default:
      return sinf(angle);
  }
}

void updateRhythmSelectionLocked(int rhythmIndex) {
  if (rhythmIndex < 0 || rhythmIndex >= NUM_RHYTHMS) {
    rhythmEnabled = false;
    activeRhythm = -1;
    rhythmStep = 0;
    rhythmEnvelope = 0.0f;
    rhythmPhase = 0.0f;
    nextRhythmStepSample = sampleCounter;
    rhythmStepIntervalSamples = 1;
    return;
  }

  const RhythmPattern &pattern = RHYTHMS[rhythmIndex];
  const float stepsPerBeat = 4.0f; // 16th notes
  const float secondsPerStep = 60.0f / (pattern.bpm * stepsPerBeat);

  rhythmEnabled = true;
  activeRhythm = rhythmIndex;
  rhythmStep = 0;
  rhythmEnvelope = 0.0f;
  rhythmPhase = 0.0f;
  sampleCounter = 0;
  rhythmStepIntervalSamples = static_cast<uint32_t>(secondsPerStep * SAMPLE_RATE);
  if (rhythmStepIntervalSamples == 0) {
    rhythmStepIntervalSamples = 1;
  }
  nextRhythmStepSample = 0;
}

// ---------------------------------------------------------------------------
// Audio generation ISR
// ---------------------------------------------------------------------------

void IRAM_ATTR onAudioTimer() {
  portENTER_CRITICAL_ISR(&timerMux);

  float mix = 0.0f;

  for (size_t i = 0; i < NUM_NOTES; ++i) {
    if (noteActive[i]) {
      // Attack towards full volume when pressed.
      noteEnvelope[i] += (1.0f - noteEnvelope[i]) * 0.01f;
    } else {
      // Release back to zero when not pressed.
      noteEnvelope[i] *= 0.995f;
    }

    if (noteEnvelope[i] > 0.0005f) {
      notePhase[i] += NOTE_FREQUENCIES[i] / static_cast<float>(SAMPLE_RATE);
      if (notePhase[i] >= 1.0f) {
        notePhase[i] -= 1.0f;
      }

      mix += noteEnvelope[i] * applyInstrumentWave(currentInstrument, notePhase[i]);
    }
  }

  if (rhythmEnabled && activeRhythm >= 0) {
    if (sampleCounter >= nextRhythmStepSample) {
      const RhythmPattern &pattern = RHYTHMS[activeRhythm];
      rhythmEnvelope = pattern.steps[rhythmStep % pattern.length];
      nextRhythmStepSample += rhythmStepIntervalSamples;
      rhythmStep = (rhythmStep + 1) % pattern.length;
    }

    // Simple percussive envelope decay and noise-based drum hit.
    rhythmEnvelope *= 0.997f;
    rhythmPhase += 120.0f / static_cast<float>(SAMPLE_RATE);
    if (rhythmPhase >= 1.0f) {
      rhythmPhase -= 1.0f;
    }

    const float drum = (sinf(TWO_PI * rhythmPhase) + ((float)esp_random() / UINT32_MAX) - 0.5f) * 0.5f;
    mix += rhythmEnvelope * drum;
  }

  // Simple master limiter to keep the mix within [-1, 1].
  mix = constrain(mix * 0.6f, -1.0f, 1.0f);

  const uint8_t outputValue = static_cast<uint8_t>((mix * 127.0f) + 128.0f);
  dacWrite(DAC_PIN, outputValue);

  ++sampleCounter;

  portEXIT_CRITICAL_ISR(&timerMux);
}

// ---------------------------------------------------------------------------
// Arduino setup and loop
// ---------------------------------------------------------------------------

void setupButtons() {
  for (size_t i = 0; i < NUM_NOTES; ++i) {
    pinMode(NOTE_PINS[i], INPUT_PULLUP);
    const bool pressed = (digitalRead(NOTE_PINS[i]) == LOW);
    noteButtons[i] = {NOTE_PINS[i], pressed, pressed, 0};
    noteActive[i] = pressed;
  }

  pinMode(INSTRUMENT_BUTTON_PIN, INPUT_PULLUP);
  const bool instrumentPressed = (digitalRead(INSTRUMENT_BUTTON_PIN) == LOW);
  instrumentButton = {INSTRUMENT_BUTTON_PIN, instrumentPressed, instrumentPressed, 0};

  pinMode(RHYTHM_BUTTON_PIN, INPUT_PULLUP);
  const bool rhythmPressed = (digitalRead(RHYTHM_BUTTON_PIN) == LOW);
  rhythmButton = {RHYTHM_BUTTON_PIN, rhythmPressed, rhythmPressed, 0};
}

void setupTimer() {
  audioTimer = timerBegin(TIMER_INDEX, TIMER_DIVIDER, true);
  timerAttachInterrupt(audioTimer, &onAudioTimer, true);
  timerAlarmWrite(audioTimer, TIMER_INTERVAL_US, true);
  timerAlarmEnable(audioTimer);
}

void setup() {
  setupButtons();
  setupTimer();
}

void loop() {
  const uint32_t now = millis();

  for (size_t i = 0; i < NUM_NOTES; ++i) {
    if (updateButton(noteButtons[i], now)) {
      portENTER_CRITICAL(&timerMux);
      noteActive[i] = noteButtons[i].stableState;
      portEXIT_CRITICAL(&timerMux);
    }
  }

  if (updateButton(instrumentButton, now) && instrumentButton.stableState) {
    portENTER_CRITICAL(&timerMux);
    currentInstrument = static_cast<Instrument>((currentInstrument + 1) % NUM_INSTRUMENTS);
    portEXIT_CRITICAL(&timerMux);
  }

  if (updateButton(rhythmButton, now) && rhythmButton.stableState) {
    static int rhythmSelection = -1;
    rhythmSelection = (rhythmSelection + 2) % (NUM_RHYTHMS + 1) - 1; // Cycle -1 -> 0 -> 1 -> ...

    portENTER_CRITICAL(&timerMux);
    updateRhythmSelectionLocked(rhythmSelection);
    portEXIT_CRITICAL(&timerMux);
  }
}

