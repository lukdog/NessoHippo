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

// Static for now: the parameter interpolation, the state table and the
// idle life (blink, breath, gaze) arrive in the next commit.
static const FaceParams FACE_HAPPY = {
  /*eyeOpen*/ 0.85f, /*lidTilt*/ -0.2f, /*pupilX*/ 0.0f, /*pupilY*/ 0.0f,
  /*joy*/ 0.9f, /*bgHue*/ 88.0f, /*bounce*/ 3.0f,
};

const FaceParams& faceTick(uint32_t nowMs) {
  (void)nowMs;
  return FACE_HAPPY;
}

static void faceDrawFull(LGFX_Sprite& spr, const FaceParams& p, uint32_t nowMs) {
  (void)nowMs;
  faceDraw(spr, p, 1.0f, 0.0f, 0.0f);
}

const Buddy FACE_BUDDY = { "face", nullptr, nullptr, faceDrawFull, true };
