// IMU-driven motion detectors, built on the BMI270's accelerometer.
//
// Shake: count zero-crossings of the high-pass-filtered dynamic
//   acceleration within a sliding window. N crossings in T ms → shake
//   event, with a refractory period to prevent multiple fires on one
//   gesture.
//
// Face-down: az < threshold sustained for ≥ FACEDOWN_HOLD_MS. The Nesso
//   screen faces +z, so a flat-on-desk-screen-down posture pushes az toward
//   -1g.
//
// The EMA pipeline mirrors the one used in NessoContapassi/NessoHourglass:
//   gravityEMA tracks the slow baseline; dynEMA is a lightly low-passed
//   high-frequency residual.

#include "motion.h"
#include <Arduino_Nesso_N1.h>
#include <math.h>

#define GRAVITY_EMA_ALPHA       0.06f   // slow baseline so a sustained tilt doesn't eat real motion
#define DYN_EMA_BETA            0.40f   // kept for telemetry only (not used by the detector)

// Shake is detected from the raw `dyn = mag - gravityEMA` signal — using
// dynEMA attenuates the peaks too much for a "normal" wrist shake to
// cross. We want either a clear oscillation OR one big jolt to count.
#define SHAKE_THRESHOLD_G       0.45f   // |dyn| crossing magnitude
#define SHAKE_CROSSINGS_NEEDED  3       // count within the window
#define SHAKE_WINDOW_MS         1200UL
#define SHAKE_INSTANT_G         1.80f   // a single huge spike triggers immediately
#define SHAKE_REFRACTORY_MS     1500UL

// A nudge is a nudge, not a shake: high enough that typing on the same
// desk does not register, low enough that picking the device up does.
#define NUDGE_THRESHOLD_G       0.18f
#define NUDGE_REFRACTORY_MS     1200UL

#define FACEDOWN_AZ_THRESHOLD  -0.70f
#define FACEDOWN_HOLD_MS        1000UL

// Optional: define DEBUG_MOTION to print peak dyn values for tuning.
// #define DEBUG_MOTION

static float          s_gravityEMA   = 1.0f;
static float          s_dynEMA       = 0.0f;
static bool           s_nudgePending  = false;
static unsigned long  s_lastNudgeMs   = 0;
static int            s_prevSign     = 0;     // sign of last suprathreshold sample
static unsigned long  s_crossings[8];
static int            s_crossIdx     = 0;
static int            s_crossCount   = 0;
static unsigned long  s_lastShakeMs  = 0;
static bool           s_pendingShake = false;
#ifdef DEBUG_MOTION
static float          s_peakAbsDyn   = 0.0f;
static unsigned long  s_lastDbgMs    = 0;
#endif

static unsigned long  s_faceDownEnteredMs = 0;
static bool           s_faceDown          = false;

void motionBegin() {
  IMU.begin();
  // Seed gravity from one good sample so the first ticks don't false-fire.
  for (int i = 0; i < 8; i++) {
    if (IMU.accelerationAvailable()) {
      float ax, ay, az;
      IMU.readAcceleration(ax, ay, az);
      s_gravityEMA = sqrtf(ax*ax + ay*ay + az*az);
      break;
    }
    delay(10);
  }
}

void motionTick() {
  if (!IMU.accelerationAvailable()) return;
  float ax, ay, az;
  IMU.readAcceleration(ax, ay, az);

  // EMA pipeline (slow gravity baseline, kept dyn EMA around for telemetry).
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  s_gravityEMA = (1.0f - GRAVITY_EMA_ALPHA) * s_gravityEMA + GRAVITY_EMA_ALPHA * mag;
  float dyn = mag - s_gravityEMA;
  s_dynEMA = (1.0f - DYN_EMA_BETA) * s_dynEMA + DYN_EMA_BETA * dyn;

  unsigned long now = millis();

#ifdef DEBUG_MOTION
  float absDyn = dyn < 0 ? -dyn : dyn;
  if (absDyn > s_peakAbsDyn) s_peakAbsDyn = absDyn;

  // Nudge: one sample over a low threshold is enough. Refractory-limited
  // so carrying the device around does not fire it continuously.
  if (absDyn > NUDGE_THRESHOLD_G && now - s_lastNudgeMs >= NUDGE_REFRACTORY_MS) {
    s_nudgePending = true;
    s_lastNudgeMs  = now;
  }
  if (now - s_lastDbgMs >= 250) {
    Serial.printf("[motion] peak|dyn|=%.2fg  ax=%.2f ay=%.2f az=%.2f g=%.2f\n",
                  s_peakAbsDyn, ax, ay, az, s_gravityEMA);
    s_peakAbsDyn = 0.0f;
    s_lastDbgMs = now;
  }
#endif

  // --- Shake:
  //   A) one suprathreshold sample with |dyn| > SHAKE_INSTANT_G fires
  //      immediately (a single sharp jolt counts as shake).
  //   B) otherwise we count SIGN FLIPS of `dyn` once it exceeds
  //      SHAKE_THRESHOLD_G — a real wrist shake oscillates around 0g,
  //      so we want flips, not just same-direction crossings.
  bool instant = (dyn >  SHAKE_INSTANT_G) || (dyn < -SHAKE_INSTANT_G);

  int sign = 0;
  if (dyn >  SHAKE_THRESHOLD_G) sign =  1;
  else if (dyn < -SHAKE_THRESHOLD_G) sign = -1;

  if (sign != 0 && sign != s_prevSign) {
    s_crossings[s_crossIdx] = now;
    s_crossIdx = (s_crossIdx + 1) % (int)(sizeof(s_crossings)/sizeof(s_crossings[0]));
    if (s_crossCount < (int)(sizeof(s_crossings)/sizeof(s_crossings[0]))) s_crossCount++;
    s_prevSign = sign;
  }

  // How many crossings fall inside the recent window?
  int inWindow = 0;
  for (int i = 0; i < s_crossCount; i++) {
    if (now - s_crossings[i] <= SHAKE_WINDOW_MS) inWindow++;
  }

  bool fire = (inWindow >= SHAKE_CROSSINGS_NEEDED) || instant;
  if (fire && now - s_lastShakeMs >= SHAKE_REFRACTORY_MS) {
    s_pendingShake = true;
    s_lastShakeMs = now;
    // Clear the ring so we need a fresh full burst to refire.
    s_crossCount = 0;
    s_crossIdx   = 0;
    s_prevSign   = 0;
  }

  // --- Face-down: sustained az < threshold.
  bool downNow = (az < FACEDOWN_AZ_THRESHOLD);
  if (downNow) {
    if (s_faceDownEnteredMs == 0) s_faceDownEnteredMs = now;
    if (now - s_faceDownEnteredMs >= FACEDOWN_HOLD_MS) s_faceDown = true;
  } else {
    s_faceDownEnteredMs = 0;
    s_faceDown = false;
  }
}

bool motionConsumeShake() {
  if (!s_pendingShake) return false;
  s_pendingShake = false;
  return true;
}

bool motionConsumeNudge() {
  bool n = s_nudgePending;
  s_nudgePending = false;
  return n;
}

bool motionFaceDown() {
  return s_faceDown;
}
