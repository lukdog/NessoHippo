// NessoFace — a full-screen parametric face, drawn every frame from the
// seven floats of FaceParams. Two white squircle eyes and two nostrils on
// a flat colored background: the hint of a hippo, never the drawing of
// one. No mouth, no ears, no head outline.
//
// Hard rules (spec section 2): flat fills only, no gradients, no blur.
// The drop shadow is a hard-edged copy of the eye offset downward, the
// highlight is a square of the *background* cut into the eye. Only three
// colors reach the screen, all derived from one hue. This is why the eyes
// go through the rasterizer below instead of fillSmoothRoundRect(), which
// antialiases its edges.

#include "face.h"

#include <math.h>

// ---- geometry (spec section 7), landscape 240x135 ----
static const int   EYE_LEFT_X   = 74;
static const int   EYE_RIGHT_X  = 166;
static const int   EYE_CY       = 62;
static const float EYE_BASE_H   = 56.0f;
static const float EYE_BASE_W   = 50.0f;
static const float EYE_RADIUS   = 17.0f;
static const float EYE_SHADOW_DY= 4.0f;
static const float GAZE_X_PX    = 15.0f;  // px of travel per unit pupilX
static const float GAZE_Y_PX    = 11.0f;
static const float LID_TILT_RAD = 0.20f;  // radians per unit lidTilt
// A specular streak, like a window reflected in something curved, rather
// than a round dot. Position is a share of the eye, so it tracks the
// squash instead of drifting when the eye changes shape.
static const float HILITE_W     = 15.0f;
static const float HILITE_H     = 6.0f;
static const float HILITE_RADIUS= 3.0f;
static const float HILITE_FX    =  0.19f;
static const float HILITE_FY    = -0.27f;
static const int   NOSE_CX      = 120;
static const int   NOSE_DX      = 17;
static const int   NOSE_CY      = 116;
static const float NOSE_H       = 15.0f;
static const float NOSE_RADIUS  = 5.0f;
static const float NOSE_BASE_W  = 9.0f;
static const float NOSE_MAX_FLARE = 3.0f;

// Eye squash: a happy eye is wide and short, a sad one narrow and tall.
// With no mouth to bend, this is what carries the mood.
static const float SQUASH_SAD   = 1.14f;
static const float SQUASH_HAPPY = 0.86f;

static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// -1 = both eyes follow eyeOpen; 0 or 1 = only that eye opens, the other
// stays shut. Set when entering FS_PEEK; the drawing code needs to know
// which eye is closed, not what state the face is in.
static int s_openEyeOnly = -1;

// HSV -> RGB565. The only color math in the file; there is no gradient
// code anywhere.
static uint16_t hsv565(float h, float s, float v) {
  h = fmodf(h, 360.0f);
  if (h < 0) h += 360.0f;
  float c = v * s;
  float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
  float m = v - c;
  float r, g, b;
  switch ((int)(h / 60.0f) % 6) {
    case 0:  r = c; g = x; b = 0; break;
    case 1:  r = x; g = c; b = 0; break;
    case 2:  r = 0; g = c; b = x; break;
    case 3:  r = 0; g = x; b = c; break;
    case 4:  r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;
  }
  uint8_t R = (uint8_t)((r + m) * 255.0f);
  uint8_t G = (uint8_t)((g + m) * 255.0f);
  uint8_t B = (uint8_t)((b + m) * 255.0f);
  return (uint16_t)(((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3));
}

// Filled rounded rect, rotated about its center, hard-edged.
//
// The ESP32-C6 has no FPU, so the per-pixel test is integer Q16: for a
// fixed row the local coordinates advance by a constant step per column,
// which turns the rotation into two adds. The shape is convex, so each
// row is one contiguous span and we can stop at its right edge.
static void fillRotRoundRect(LGFX_Sprite& spr, float cx, float cy,
                             float w, float h, float r, float ang,
                             uint16_t color) {
  float hwf = w * 0.5f, hhf = h * 0.5f;
  if (hwf < 0.5f || hhf < 0.5f) return;
  if (r > hwf) r = hwf;
  if (r > hhf) r = hhf;

  const float caf = cosf(ang), saf = sinf(ang);
  const int32_t ca  = (int32_t)(caf * 65536.0f);   // per-column step
  const int32_t sa  = (int32_t)(saf * 65536.0f);
  const int32_t hw  = (int32_t)(hwf * 65536.0f);
  const int32_t hh  = (int32_t)(hhf * 65536.0f);
  const int32_t inx = (int32_t)((hwf - r) * 65536.0f);  // straight-edge
  const int32_t iny = (int32_t)((hhf - r) * 65536.0f);  // extents
  const int32_t r2  = (int32_t)(r * 256.0f) * (int32_t)(r * 256.0f);

  const int reach = (int)sqrtf(hwf * hwf + hhf * hhf) + 2;
  const int x0 = max(0, (int)cx - reach);
  const int x1 = min((int)spr.width()  - 1, (int)cx + reach);
  const int y0 = max(0, (int)cy - reach);
  const int y1 = min((int)spr.height() - 1, (int)cy + reach);

  for (int y = y0; y <= y1; y++) {
    // Inverse-rotate the row's first pixel, then step along the row.
    const float dy = (float)y - cy, dx0 = (float)x0 - cx;
    int32_t lx = (int32_t)((dx0 * caf + dy * saf) * 65536.0f);
    int32_t ly = (int32_t)((dy * caf - dx0 * saf) * 65536.0f);
    int first = -1, last = -1;
    for (int x = x0; x <= x1; x++, lx += ca, ly -= sa) {
      const int32_t ax = lx < 0 ? -lx : lx;
      const int32_t ay = ly < 0 ? -ly : ly;
      bool in;
      if (ax > hw || ay > hh)          in = false;
      else if (ax <= inx || ay <= iny) in = true;
      else {
        const int32_t px = (ax - inx) >> 8;  // Q8, so the squares fit int32
        const int32_t py = (ay - iny) >> 8;
        in = (px * px + py * py) <= r2;
      }
      if (in) {
        if (first < 0) first = x;
        last = x;
      } else if (first >= 0) {
        break;  // convex: this row's span has ended
      }
    }
    if (first >= 0) spr.drawFastHLine(first, y, last - first + 1, color);
  }
}

static void drawEye(LGFX_Sprite& spr, const FaceParams& p, int baseX, int dir,
                    float w, float h, float bounceOff, float blink,
                    float lidTilt,
                    uint16_t dark, uint16_t white, uint16_t bg) {
  const float cx  = (float)baseX + p.pupilX * GAZE_X_PX;
  const float cy  = (float)EYE_CY + bounceOff + p.pupilY * GAZE_Y_PX;
  const float ang = (float)dir * lidTilt * LID_TILT_RAD;
  // Clamped so a nearly-closed eye stays a capsule instead of glitching.
  float r = EYE_RADIUS;
  if (r > h * 0.5f) r = h * 0.5f;
  if (r > w * 0.5f) r = w * 0.5f;

  const float ca = cosf(ang), sa = sinf(ang);

  // Hard drop shadow: the same shape, offset straight down in screen
  // space. Rotating the offset with the eye would swing the light
  // direction around, which is exactly what a flat shadow must not do.
  fillRotRoundRect(spr, cx, cy + EYE_SHADOW_DY, w, h, r, ang, dark);
  fillRotRoundRect(spr, cx, cy, w, h, r, ang, white);

  // Highlight: a square of the background cut into the eye, not a sheen,
  // and deliberately fixed. Tried tracking it to the gaze so it would read
  // as a pupil and point the look somewhere: it works, but the eye stops
  // reading as glossy and the face loses its cartoon character. Not worth
  // it. The gaze is carried by the eye translating instead.
  if (blink > 0.75f && h > 20.0f) {
    const float hx = w * HILITE_FX, hy = h * HILITE_FY;
    fillRotRoundRect(spr, cx + hx * ca - hy * sa, cy + hx * sa + hy * ca,
                     HILITE_W, HILITE_H, HILITE_RADIUS, ang, bg);
  }
}

// blink: 1 = fully open, 0 = shut. Animation lands in a later commit.
static void faceDraw(LGFX_Sprite& spr, const FaceParams& p, float blink,
                     float bounceOff, float noseQuiver, float tiltRock) {
  const uint16_t bg    = hsv565(p.bgHue,        0.78f, 0.95f);
  const uint16_t dark  = hsv565(p.bgHue - 8.0f, 0.88f, 0.40f);
  const uint16_t white = 0xFFFF;

  spr.fillSprite(bg);

  const float joyT   = (p.joy + 1.0f) * 0.5f;
  const float squash = lerpf(SQUASH_SAD, SQUASH_HAPPY, joyT);
  const float hOpen  = EYE_BASE_H * p.eyeOpen * blink * squash;
  const float w = (EYE_BASE_W / squash) * (1.0f + 0.12f * (1.0f - blink));

  // A peek opens one eye and leaves the other shut. The clamp is per eye,
  // so the closed one collapses to the same flat capsule as a shut eye.
  float hL = (s_openEyeOnly == 1) ? 0.0f : hOpen;
  float hR = (s_openEyeOnly == 0) ? 0.0f : hOpen;
  if (hL < 5.0f) hL = 5.0f;
  if (hR < 5.0f) hR = 5.0f;

  const float lidTilt = p.lidTilt + tiltRock;
  drawEye(spr, p, EYE_LEFT_X,  +1, w, hL, bounceOff, blink, lidTilt,
          dark, white, bg);
  drawEye(spr, p, EYE_RIGHT_X, -1, w, hR, bounceOff, blink, lidTilt,
          dark, white, bg);

  const float noseW = NOSE_BASE_W + lerpf(0.0f, NOSE_MAX_FLARE, joyT)
                      + noseQuiver;
  for (int d = -1; d <= 1; d += 2) {
    fillRotRoundRect(spr, (float)(NOSE_CX + d * NOSE_DX),
                     (float)NOSE_CY + bounceOff,
                     noseW, NOSE_H, NOSE_RADIUS, 0.0f, dark);
  }
}

// ---- animation tuning (spec sections 6 and 9) ----
static const float    LERP_RATE       = 0.05f;  // ~1 s settle at 30 fps
static const float    GAZE_LERP       = 0.30f;  // deliberately snappy
static const float    FRAME_REF_MS    = 33.0f;  // the fps those rates assume
static const uint32_t BLINK_MIN_MS    = 2600;
static const uint32_t BLINK_MAX_MS    = 6000;
static const uint32_t BLINK_MS        = 150;
static const uint32_t BLINK_DOUBLE_MS = 300;
static const int      BLINK_DOUBLE_PCT= 12;
static const uint32_t BREATH_MS       = 3000;
static const uint32_t GAZE_MIN_MS     = 1900;
static const uint32_t GAZE_MAX_MS     = 4300;
static const uint32_t GAZE_BUSY_MIN_MS= 800;
static const uint32_t GAZE_BUSY_MAX_MS= 3200;
static const float    GAZE_RANGE_X    = 0.60f;
static const float    GAZE_RANGE_Y    = 0.50f;
// BUSY reads a line rather than wandering: fixed stops left to right, then
// one more phase that flicks back to the start.
static const uint32_t READ_STEP_MS    = 420;
static const float    READ_STOPS[]    = { -0.85f, -0.45f, -0.05f,
                                           0.35f,  0.75f };
static const float    READ_LINE_Y     = 0.10f;
static const float    READ_RETURN_Y   = 0.18f;
// CURIOUS rocks the lids back and forth, which reads as a head tilt.
static const float    TILT_ROCK_AMP   = 0.28f;
static const uint32_t TILT_ROCK_MS    = 2600;
static const uint32_t NOSE_QUIVER_MS  = 300;
static const float    NOSE_QUIVER_AMP = 1.2f;

static const float TWO_PI_F = 6.2831853f;

//                                eyeOpen lidTilt pupilX pupilY   joy    hue  bounce
static const FaceParams STATE_TABLE[FS_COUNT] = {
  /* FS_ASLEEP  */ {  0.05f,   0.3f,  0.0f,  0.0f,  0.0f, 228.0f, 1.0f },
  /* FS_SAD     */ {  0.70f,   0.8f,  0.0f,  0.0f, -0.8f, 210.0f, 0.0f },
  /* FS_HAPPY   */ {  0.85f,  -0.2f,  0.0f,  0.0f,  0.9f,  88.0f, 3.0f },
  /* FS_BUSY    */ {  0.60f,  -0.5f,  0.0f,  0.0f,  0.2f,  34.0f, 2.0f },
  /* FS_CURIOUS */ {  1.00f,   0.0f,  0.0f,  0.0f,  0.4f, 168.0f, 2.0f },
  /* FS_DIZZY   */ {  1.00f,   0.0f,  0.0f,  0.0f,  0.0f, 295.0f, 0.0f },
  /* FS_HEART   */ {  0.60f,  -0.3f,  0.0f,  0.0f,  1.0f, 330.0f, 3.0f },
  // Keeps the ASLEEP hue: it is still the middle of the night, the face is
  // just checking who that was.
  /* FS_PEEK    */ {  0.95f,   0.1f,  0.0f,  0.0f,  0.0f, 228.0f, 1.0f },
};

// Animation state. `blink` and the breathing offset are deliberately not
// FaceParams fields: the state table has no opinion on them and they must
// never be interpolated.
static FaceState  s_state  = FS_HAPPY;
static FaceParams s_target = STATE_TABLE[FS_HAPPY];
static FaceParams s_cur    = STATE_TABLE[FS_HAPPY];
static float      s_blink     = 1.0f;
static float      s_bounceOff = 0.0f;
static float      s_tiltRock  = 0.0f;
static float      s_tiltEnv   = 0.0f;   // eased 0..1, so the rock never snaps
static bool       s_seeded    = false;
static uint32_t   s_lastTickMs = 0;

static uint32_t s_nextBlinkMs  = 0;
static uint32_t s_blinkStartMs = 0;
static uint32_t s_blinkDurMs   = 0;
static bool     s_blinking     = false;
static uint32_t s_nextGazeMs   = 0;
static float    s_gazeTargetX  = 0.0f;
static float    s_gazeTargetY  = 0.0f;

static inline float randf(float range) {
  return (float)random(-1000, 1001) / 1000.0f * range;
}

// Shortest way around the hue wheel, so 350 -> 10 crosses zero instead of
// sweeping backwards through the whole spectrum.
static float hueApproach(float from, float to, float t) {
  const float d = fmodf(to - from + 540.0f, 360.0f) - 180.0f;
  float h = fmodf(from + d * t, 360.0f);
  return h < 0.0f ? h + 360.0f : h;
}

void faceSetState(FaceState st) {
  if (st == s_state || st < 0 || st >= FS_COUNT) return;
  s_openEyeOnly = (st == FS_PEEK) ? (int)random(2) : -1;
  s_state  = st;
  s_target = STATE_TABLE[st];
}

// The resting state. Parks s_state at FS_HAPPY so the gaze cadence and the
// nostrils use their calm timings rather than the BUSY ones; the discrete
// state machine never asks for FS_HAPPY or FS_SAD directly, they exist
// only as the endpoints of this blend.
void faceSetMood(float mood) {
  if (mood < 0.0f) mood = 0.0f;
  if (mood > 1.0f) mood = 1.0f;
  const FaceParams& sad = STATE_TABLE[FS_SAD];
  const FaceParams& happy = STATE_TABLE[FS_HAPPY];
  s_state = FS_HAPPY;
  s_openEyeOnly = -1;
  s_target.eyeOpen = lerpf(sad.eyeOpen, happy.eyeOpen, mood);
  s_target.lidTilt = lerpf(sad.lidTilt, happy.lidTilt, mood);
  s_target.joy     = lerpf(sad.joy,     happy.joy,     mood);
  s_target.bounce  = lerpf(sad.bounce,  happy.bounce,  mood);
  s_target.bgHue   = lerpf(sad.bgHue,   happy.bgHue,   mood);  // 210->88
}

// Squash-and-stretch, not a fade: the blink also widens the eye by 12%
// (see the w formula), which is what makes it read as modern.
static void updateBlink(uint32_t now) {
  if (s_blinking) {
    const uint32_t elapsed = now - s_blinkStartMs;
    if (elapsed >= s_blinkDurMs) {
      s_blinking = false;
      s_blink    = 1.0f;
      s_nextBlinkMs = now + (uint32_t)random(BLINK_MIN_MS, BLINK_MAX_MS + 1);
      return;
    }
    float p = (float)elapsed / (float)s_blinkDurMs;
    if (s_blinkDurMs == BLINK_DOUBLE_MS) p = fmodf(p * 2.0f, 1.0f);
    s_blink = fabsf(1.0f - 2.0f * p);
  } else if ((int32_t)(now - s_nextBlinkMs) >= 0) {
    s_blinking     = true;
    s_blinkStartMs = now;
    s_blinkDurMs   = (random(100) < BLINK_DOUBLE_PCT) ? BLINK_DOUBLE_MS
                                                      : BLINK_MS;
  }
}

// A snappy saccade looks alive; a slow glide looks sleepy. Hence the fast
// lerp, and hence gaze being excluded from the parameter interpolation.
static void updateGaze(uint32_t now, float rate) {
  if (s_state == FS_BUSY) {
    // Stepping along a line is the one thing that reads as an activity
    // rather than a mood. Keep s_nextGazeMs current so leaving BUSY
    // retargets immediately instead of finishing a stale timer.
    const int n = (int)(sizeof(READ_STOPS) / sizeof(READ_STOPS[0]));
    const int i = (int)((now / READ_STEP_MS) % (uint32_t)(n + 1));
    s_gazeTargetX = READ_STOPS[i == n ? 0 : i];
    s_gazeTargetY = (i == n) ? READ_RETURN_Y : READ_LINE_Y;
    s_nextGazeMs  = now;
  } else if ((int32_t)(now - s_nextGazeMs) >= 0) {
    s_gazeTargetX = randf(GAZE_RANGE_X);
    s_gazeTargetY = randf(GAZE_RANGE_Y);
    const bool busy = (s_state == FS_BUSY);
    s_nextGazeMs = now + (uint32_t)random(busy ? GAZE_BUSY_MIN_MS : GAZE_MIN_MS,
                                          (busy ? GAZE_BUSY_MAX_MS
                                                : GAZE_MAX_MS) + 1);
  }
  s_cur.pupilX += (s_gazeTargetX - s_cur.pupilX) * rate;
  s_cur.pupilY += (s_gazeTargetY - s_cur.pupilY) * rate;
}

const FaceParams& faceTick(uint32_t nowMs) {
  if (!s_seeded) {
    randomSeed(esp_random());
    s_seeded       = true;
    s_lastTickMs   = nowMs;
    s_nextBlinkMs  = nowMs + (uint32_t)random(BLINK_MIN_MS, BLINK_MAX_MS + 1);
    s_nextGazeMs   = nowMs;
  }

  // The rates above are per frame at 30 fps, but a full-sprite push plus
  // the loop period lands nearer 25, and BLE traffic moves it around. So
  // scale by how long this frame actually took: the settle time stays ~1 s
  // whatever the panel manages.
  float dt = (float)(nowMs - s_lastTickMs) / FRAME_REF_MS;
  s_lastTickMs = nowMs;
  if (dt < 0.1f) dt = 0.1f;
  if (dt > 3.0f) dt = 3.0f;   // after a stall, ease in rather than snap
  float k = LERP_RATE * dt;   if (k > 1.0f) k = 1.0f;
  float g = GAZE_LERP * dt;   if (g > 1.0f) g = 1.0f;

  // Everything except gaze eases toward the target; nothing ever snaps.
  s_cur.eyeOpen += (s_target.eyeOpen - s_cur.eyeOpen) * k;
  s_cur.lidTilt += (s_target.lidTilt - s_cur.lidTilt) * k;
  s_cur.joy     += (s_target.joy     - s_cur.joy)     * k;
  s_cur.bounce  += (s_target.bounce  - s_cur.bounce)  * k;
  s_cur.bgHue    = hueApproach(s_cur.bgHue, s_target.bgHue, k);

  updateBlink(nowMs);
  updateGaze(nowMs, g);
  s_bounceOff = s_cur.bounce *
                sinf((float)(nowMs % BREATH_MS) * TWO_PI_F / (float)BREATH_MS);

  // Head tilt, kept out of FaceParams like blink and the breathing offset:
  // it is a per-frame modifier and must never be interpolated. The
  // envelope eases the whole oscillation in and out, so entering or
  // leaving CURIOUS mid-swing does not jump the lids.
  const float tiltWanted = (s_state == FS_CURIOUS) ? 1.0f : 0.0f;
  s_tiltEnv += (tiltWanted - s_tiltEnv) * k;
  s_tiltRock = TILT_ROCK_AMP * s_tiltEnv *
               sinf((float)(nowMs % TILT_ROCK_MS) * TWO_PI_F /
                    (float)TILT_ROCK_MS);
  return s_cur;
}

static void faceDrawFull(LGFX_Sprite& spr, const FaceParams& p, uint32_t nowMs) {
  // Nostril quiver while Claude is working. Nothing else animates them.
  const float quiver =
      (s_state == FS_BUSY)
          ? NOSE_QUIVER_AMP * sinf((float)(nowMs % NOSE_QUIVER_MS) *
                                   TWO_PI_F / (float)NOSE_QUIVER_MS)
          : 0.0f;
  faceDraw(spr, p, s_blink, s_bounceOff, quiver, s_tiltRock);
}

const Buddy FACE_BUDDY = { "face", nullptr, nullptr, faceDrawFull, true };
