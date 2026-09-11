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
static const float GAZE_X_PX    = 7.0f;   // px of travel per unit pupilX
static const float GAZE_Y_PX    = 6.0f;
static const float LID_TILT_RAD = 0.20f;  // radians per unit lidTilt
static const float HILITE_SIZE  = 11.0f;
static const float HILITE_RADIUS= 5.0f;
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
                    uint16_t dark, uint16_t white, uint16_t bg) {
  const float cx  = (float)baseX + p.pupilX * GAZE_X_PX;
  const float cy  = (float)EYE_CY + bounceOff + p.pupilY * GAZE_Y_PX;
  const float ang = (float)dir * p.lidTilt * LID_TILT_RAD;
  // Clamped so a nearly-closed eye stays a capsule instead of glitching.
  float r = EYE_RADIUS;
  if (r > h * 0.5f) r = h * 0.5f;
  if (r > w * 0.5f) r = w * 0.5f;

  const float ca = cosf(ang), sa = sinf(ang);

  // Hard drop shadow: the same shape, offset down in the eye's own frame.
  fillRotRoundRect(spr, cx - EYE_SHADOW_DY * sa, cy + EYE_SHADOW_DY * ca,
                   w, h, r, ang, dark);
  fillRotRoundRect(spr, cx, cy, w, h, r, ang, white);

  // Highlight: a square of the background cut into the eye, not a sheen.
  if (blink > 0.75f && h > 20.0f) {
    const float hx = w * 0.23f, hy = -h * 0.25f;
    fillRotRoundRect(spr, cx + hx * ca - hy * sa, cy + hx * sa + hy * ca,
                     HILITE_SIZE, HILITE_SIZE, HILITE_RADIUS, ang, bg);
  }
}

// blink: 1 = fully open, 0 = shut. Animation lands in a later commit.
static void faceDraw(LGFX_Sprite& spr, const FaceParams& p, float blink,
                     float bounceOff, float noseQuiver) {
  const uint16_t bg    = hsv565(p.bgHue,        0.78f, 0.95f);
  const uint16_t dark  = hsv565(p.bgHue - 8.0f, 0.88f, 0.40f);
  const uint16_t white = 0xFFFF;

  spr.fillSprite(bg);

  const float joyT   = (p.joy + 1.0f) * 0.5f;
  const float squash = lerpf(SQUASH_SAD, SQUASH_HAPPY, joyT);
  float h = EYE_BASE_H * p.eyeOpen * blink * squash;
  if (h < 5.0f) h = 5.0f;
  const float w = (EYE_BASE_W / squash) * (1.0f + 0.12f * (1.0f - blink));

  drawEye(spr, p, EYE_LEFT_X,  +1, w, h, bounceOff, blink, dark, white, bg);
  drawEye(spr, p, EYE_RIGHT_X, -1, w, h, bounceOff, blink, dark, white, bg);

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
static const float    GAZE_RANGE_X    = 0.45f;
static const float    GAZE_RANGE_Y    = 0.35f;
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
};

// Animation state. `blink` and the breathing offset are deliberately not
// FaceParams fields: the state table has no opinion on them and they must
// never be interpolated.
static FaceState  s_state  = FS_HAPPY;
static FaceParams s_target = STATE_TABLE[FS_HAPPY];
static FaceParams s_cur    = STATE_TABLE[FS_HAPPY];
static float      s_blink     = 1.0f;
static float      s_bounceOff = 0.0f;
static bool       s_seeded    = false;

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
  s_state  = st;
  s_target = STATE_TABLE[st];
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
static void updateGaze(uint32_t now) {
  if ((int32_t)(now - s_nextGazeMs) >= 0) {
    s_gazeTargetX = randf(GAZE_RANGE_X);
    s_gazeTargetY = randf(GAZE_RANGE_Y);
    const bool busy = (s_state == FS_BUSY);
    s_nextGazeMs = now + (uint32_t)random(busy ? GAZE_BUSY_MIN_MS : GAZE_MIN_MS,
                                          (busy ? GAZE_BUSY_MAX_MS
                                                : GAZE_MAX_MS) + 1);
  }
  s_cur.pupilX += (s_gazeTargetX - s_cur.pupilX) * GAZE_LERP;
  s_cur.pupilY += (s_gazeTargetY - s_cur.pupilY) * GAZE_LERP;
}

const FaceParams& faceTick(uint32_t nowMs) {
  if (!s_seeded) {
    randomSeed(esp_random());
    s_seeded       = true;
    s_nextBlinkMs  = nowMs + (uint32_t)random(BLINK_MIN_MS, BLINK_MAX_MS + 1);
    s_nextGazeMs   = nowMs;
  }

  // Everything except gaze eases toward the target; nothing ever snaps.
  s_cur.eyeOpen += (s_target.eyeOpen - s_cur.eyeOpen) * LERP_RATE;
  s_cur.lidTilt += (s_target.lidTilt - s_cur.lidTilt) * LERP_RATE;
  s_cur.joy     += (s_target.joy     - s_cur.joy)     * LERP_RATE;
  s_cur.bounce  += (s_target.bounce  - s_cur.bounce)  * LERP_RATE;
  s_cur.bgHue    = hueApproach(s_cur.bgHue, s_target.bgHue, LERP_RATE);

  updateBlink(nowMs);
  updateGaze(nowMs);
  s_bounceOff = s_cur.bounce *
                sinf((float)(nowMs % BREATH_MS) * TWO_PI_F / (float)BREATH_MS);
  return s_cur;
}

static void faceDrawFull(LGFX_Sprite& spr, const FaceParams& p, uint32_t nowMs) {
  // Nostril quiver while Claude is working. Nothing else animates them.
  const float quiver =
      (s_state == FS_BUSY)
          ? NOSE_QUIVER_AMP * sinf((float)(nowMs % NOSE_QUIVER_MS) *
                                   TWO_PI_F / (float)NOSE_QUIVER_MS)
          : 0.0f;
  faceDraw(spr, p, s_blink, s_bounceOff, quiver);
}

const Buddy FACE_BUDDY = { "face", nullptr, nullptr, faceDrawFull, true };
