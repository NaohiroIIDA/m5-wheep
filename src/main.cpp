// m5-wheep — procedural "language-like" beep speech for M5StickS3
// ------------------------------------------------------------------
// Generates non-verbal, speech-like vocalizations (R2-D2 style) by
// building a PCM waveform and playing it through the ES8311 codec via
// M5Unified's M5.Speaker abstraction.
//
// Four layers from the design:
//   1. Pitch contour (intonation)  -> per-phrase falling/rising melody
//   2. Syllable envelopes          -> on/off amplitude humps, varied length
//   3. Phrase structure            -> grouping + inter-phrase pauses
//   4. Emotion -> parameters        -> STATEMENT / QUESTION / HAPPY / CONFUSED
//
// Determinism: same (intent, seed) always produces the same utterance,
// so a given "intent" sounds like it has consistent meaning.
//
// Controls:
//   BtnA (front) : cycle through the 4 intents and speak
//   BtnB (side)  : speak the current intent again with a new seed
// ------------------------------------------------------------------

#include <M5Unified.h>
#include <vector>
#include <cmath>

static const uint32_t SR = 16000;  // sample rate (Hz)

// ---- deterministic PRNG (xorshift32) ----
struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed ? seed : 0x12345678u) {}
  uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  float    f()    { return (next() >> 8) * (1.0f / 16777216.0f); }   // 0..1
  float    range(float a, float b) { return a + (b - a) * f(); }
  int      irange(int a, int b)    { return a + (int)(f() * (b - a + 1)); }
};

enum Intent { STATEMENT, QUESTION, HAPPY, CONFUSED, INTENT_COUNT };

struct Params {
  float f0;          // base pitch (Hz)
  float prange;      // pitch span across the contour (Hz)
  float endMove;     // final-syllable glide (>1 rises = question, <1 falls)
  float rate;        // syllable rate scaler (1 = normal, >1 = faster)
  float harmonic;    // 0 = pure-ish sine, 1 = buzzy square (robotic/bright)
  float wobble;      // vibrato / pitch jitter amount
  int   phrasesMin, phrasesMax;
};

Params paramsFor(Intent in) {
  switch (in) {
    case STATEMENT: return {300, 130, 0.82f, 1.00f, 0.60f, 0.04f, 1, 2};
    case QUESTION:  return {330, 150, 1.50f, 1.00f, 0.60f, 0.05f, 1, 1};
    case HAPPY:     return {430, 200, 1.10f, 1.25f, 0.80f, 0.06f, 1, 2};
    case CONFUSED:  return {250,  90, 0.95f, 0.75f, 0.50f, 0.12f, 1, 2};
    default:        return {300, 130, 0.82f, 1.00f, 0.60f, 0.04f, 1, 2};
  }
}

// one oscillator sample: blend of sine + square for a buzzy voice
static inline float osc(float phase, float harmonic) {
  float s  = sinf(phase);
  float sq = (s >= 0.f) ? 1.f : -1.f;
  return (1.f - harmonic) * s + harmonic * sq * 0.7f;
}

void addSilence(std::vector<int16_t>& buf, int ms) {
  int n = SR * ms / 1000;
  for (int i = 0; i < n; ++i) buf.push_back(0);
}

// append one syllable (with pitch glide, vibrato, and a click-free envelope)
void addSyllable(std::vector<int16_t>& buf, float startHz, float endHz,
                 int durMs, float harmonic, float wobble, Rng& rng) {
  int   n     = SR * durMs / 1000;
  float phase = 0.f;
  float vib   = rng.f() * 6.28318f;
  const float att = 0.10f, rel = 0.25f;   // attack / release fraction
  for (int i = 0; i < n; ++i) {
    float t  = (float)i / n;
    float hz = startHz + (endHz - startHz) * t;
    hz *= 1.f + wobble * sinf(vib + t * 40.f);
    phase += 6.28318530718f * hz / SR;
    if (phase > 6.2831853f) phase -= 6.2831853f;

    float env;
    if      (t < att)        env = t / att;
    else if (t > 1.f - rel)  env = (1.f - t) / rel;
    else                     env = 1.f;
    env = env * env * (3.f - 2.f * env);   // smoothstep

    float v = osc(phase, harmonic) * env * 0.7f;
    int   s = (int)(v * 22000.f);
    if (s >  32767) s =  32767;
    if (s < -32767) s = -32767;
    buf.push_back((int16_t)s);
  }
}

// build a full utterance into buf
void buildUtterance(std::vector<int16_t>& buf, Intent in, uint32_t seed) {
  Params p = paramsFor(in);
  Rng    rng(seed * 2654435761u + (uint32_t)in);

  int   phrases = rng.irange(p.phrasesMin, p.phrasesMax);
  float decl    = 1.0f;  // declination: pitch drifts down over the utterance

  for (int ph = 0; ph < phrases; ++ph) {
    int syl = rng.irange(2, 5);
    for (int k = 0; k < syl; ++k) {
      float pos     = (syl > 1) ? (float)k / (syl - 1) : 0.f;   // 0..1 in phrase
      float contour = 1.f - 0.5f * pos;                         // falls toward end
      float hz      = p.f0 * decl * (0.85f + (p.prange / p.f0) * contour);

      bool  finalSyl = (ph == phrases - 1 && k == syl - 1);
      float startHz  = hz * rng.range(0.97f, 1.04f);
      float endHz    = hz * (finalSyl ? p.endMove : rng.range(0.95f, 1.05f));
      int   dur      = (int)(rng.irange(85, 170) / p.rate);

      addSyllable(buf, startHz, endHz, dur, p.harmonic, p.wobble, rng);

      if (rng.f() < 0.7f) addSilence(buf, rng.irange(8, 35));   // syllable gap
      decl *= 0.985f;
    }
    if (ph < phrases - 1) addSilence(buf, rng.irange(150, 320)); // phrase pause
  }
}

void speak(Intent in, uint32_t seed) {
  std::vector<int16_t> buf;
  buf.reserve(SR * 2);
  buildUtterance(buf, in, seed);
  M5.Speaker.playRaw(buf.data(), buf.size(), SR, false);
  while (M5.Speaker.isPlaying()) delay(5);
}

const char* kNames[INTENT_COUNT] = {"statement", "question", "happy", "confused"};
int      gIntent = 0;
uint32_t gSeed   = 1;

void showStatus(const char* line) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.setTextSize(2);
  M5.Display.println("m5-wheep");
  M5.Display.setTextSize(2);
  M5.Display.printf("> %s\n", line);
  M5.Display.setTextSize(1);
  M5.Display.println("\nA: next intent");
  M5.Display.println("B: repeat");
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Speaker.begin();
  M5.Speaker.setVolume(150);   // ~59%. Keep <=75% on battery (reboot risk).
  showStatus("ready");
}

void loop() {
  M5.update();
  if (M5.BtnA.wasPressed()) {
    Intent in = (Intent)gIntent;
    showStatus(kNames[gIntent]);
    speak(in, gSeed++);
    gIntent = (gIntent + 1) % INTENT_COUNT;
  }
  if (M5.BtnB.wasPressed()) {
    int prev = (gIntent + INTENT_COUNT - 1) % INTENT_COUNT;
    showStatus(kNames[prev]);
    speak((Intent)prev, gSeed++);
  }
  delay(5);
}