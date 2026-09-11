// Cat buddy — seven ASCII state animations.
//
// Frames are drawn with the default GFX font at size 2 (12 px per char).
// At 135 px wide that leaves ~11 chars per line, so frames are kept narrow.
// We pick a frame from the array based on (frameMs / FRAME_PERIOD_MS) so
// each state's loop runs at the same cadence without any per-state state.
//
// drawFrame() paints into a caller-supplied LGFXBase so ui.cpp can route
// the drawing into an off-screen LGFX_Sprite (eliminating flicker) — the
// same code paths work for the live panel and for sprites.

#include "buddy.h"
#include <math.h>

// One frame's lines as a single multi-line string. Splitting happens at
// draw time on '\n'. Lines never include a trailing '\n'.
struct Frame {
  const char* lines;
};

#define FRAMES(name) static const Frame name[]

FRAMES(F_SLEEP) = {
  { " /\\_/\\\n( -.- )\n o_(\")(\")\n   z z z" },
  { " /\\_/\\\n( -.- )\n o_(\")(\")\n  z Z z" },
  { " /\\_/\\\n( -.- )\n o_(\")(\")\n   z z Z" },
};
FRAMES(F_IDLE) = {
  { " /\\_/\\\n( o.o )\n > ^ <" },
  { " /\\_/\\\n( -.o )\n > ^ <" },
  { " /\\_/\\\n( o.o )\n > ^ <" },
  { " /\\_/\\\n( o.- )\n > ^ <" },
};
// BUSY = Claude session is active (thinking, tool-calling, generating).
// The animated dots are the universal "working" indicator — covers
// thinking and typing alike without lying about what's happening.
// Each frame's dot row uses an ODD number of glyphs (1/3/5) with equal
// padding on both sides, so per-line centering puts the dots at cx.
FRAMES(F_BUSY) = {
  { " /\\_/\\\n( o.o )\n  ~~~ \n   .   " },
  { " /\\_/\\\n( -.o )\n  ~~~ \n  ...  " },
  { " /\\_/\\\n( o.- )\n  ~~~ \n ..... " },
  { " /\\_/\\\n( o.o )\n  ~~~ \n  ...  " },
};
FRAMES(F_ATTENTION) = {
  { "  !  \n /\\_/\\\n( O.O )\n > ^ <" },
  { "  !  \n /\\_/\\\n( O_O )\n > ^ <" },
};
FRAMES(F_CELEBRATE) = {
  { " \\o/\n /\\_/\\\n( ^.^ )\n > ^ <" },
  { " \\O/\n /\\_/\\\n( ^o^ )\n > ^ <" },
  { " \\o/\n /\\_/\\\n( ^.^ )\n > ^ <" },
};
FRAMES(F_DIZZY) = {
  { " /\\_/\\\n( @.@ )\n > * <" },
  { " /\\_/\\\n( @_@ )\n > * <" },
  { " /\\_/\\\n( x.x )\n > * <" },
  { " /\\_/\\\n( @_@ )\n > * <" },
};
// HEART body is just the happy cat — the hearts themselves are drawn as
// real pixel shapes (filled circles + triangle) in drawHearts() so they
// actually read as hearts at 135 px wide.
FRAMES(F_HEART) = {
  { " /\\_/\\\n( ^.^ )\n > * <" },
  { " /\\_/\\\n( o.o )\n > * <" },
};

static const Frame* framesFor(PersonaState s, size_t& count) {
  switch (s) {
    case PS_SLEEP:     count = sizeof(F_SLEEP)/sizeof(Frame);     return F_SLEEP;
    case PS_IDLE:      count = sizeof(F_IDLE)/sizeof(Frame);      return F_IDLE;
    case PS_BUSY:      count = sizeof(F_BUSY)/sizeof(Frame);      return F_BUSY;
    case PS_ATTENTION: count = sizeof(F_ATTENTION)/sizeof(Frame); return F_ATTENTION;
    case PS_CELEBRATE: count = sizeof(F_CELEBRATE)/sizeof(Frame); return F_CELEBRATE;
    case PS_DIZZY:     count = sizeof(F_DIZZY)/sizeof(Frame);     return F_DIZZY;
    case PS_HEART:     count = sizeof(F_HEART)/sizeof(Frame);     return F_HEART;
  }
  count = 0;
  return nullptr;
}

// Period per frame in ms. SLEEP / IDLE breathe slowly; the energetic
// states (BUSY/DIZZY/CELEBRATE) flip faster for a livelier read.
// HEART runs much faster than the other states because its frame index
// drives a smooth iridescent hue cycle (see catFrameIndex / catDrawFrame).
static unsigned long periodFor(PersonaState s) {
  switch (s) {
    case PS_SLEEP:     return 700;
    case PS_IDLE:      return 600;
    case PS_BUSY:      return 180;
    case PS_ATTENTION: return 280;
    case PS_CELEBRATE: return 160;
    case PS_DIZZY:     return 140;
    case PS_HEART:     return 50;  // 20 fps → smooth shimmer
  }
  return 500;
}

// Number of distinct frames in the HEART cycle. 40 frames × 50 ms = 2 s
// per full hue rotation, which matches ONE_SHOT_MS so one pet/owner-
// change shows exactly one full color sweep.
#define HEART_FRAMES 40

// HSV → RGB565 conversion. h in degrees (auto-wrapped), s & v in 0..1.
// This is the bog-standard sector formula; the float cost is fine since
// we only call it twice per HEART frame.
static uint16_t hsv565(float h, float s, float v) {
  while (h < 0)        h += 360.0f;
  while (h >= 360.0f)  h -= 360.0f;
  float c = v * s;
  float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
  float m = v - c;
  float r=0, g=0, b=0;
  int sector = (int)(h / 60.0f);
  switch (sector) {
    case 0: r = c; g = x; b = 0; break;
    case 1: r = x; g = c; b = 0; break;
    case 2: r = 0; g = c; b = x; break;
    case 3: r = 0; g = x; b = c; break;
    case 4: r = x; g = 0; b = c; break;
    default:r = c; g = 0; b = x; break;
  }
  uint8_t R = (uint8_t)((r + m) * 255.0f);
  uint8_t G = (uint8_t)((g + m) * 255.0f);
  uint8_t B = (uint8_t)((b + m) * 255.0f);
  return (uint16_t)(((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3));
}

static uint16_t colorFor(PersonaState s) {
  // 0xed03 = orange used by NessoContapassi / NessoHourglass.
  switch (s) {
    case PS_SLEEP:     return 0x52AA; // muted blue-gray
    case PS_ATTENTION: return 0xFFE0; // yellow
    case PS_CELEBRATE: return 0xed03;
    case PS_DIZZY:     return 0xF800; // red
    case PS_HEART:     return 0xF81F; // magenta
    case PS_BUSY:      return 0x07FF; // cyan
    case PS_IDLE:
    default:           return 0xed03;
  }
}

static int catFrameIndex(PersonaState state, unsigned long frameMs) {
  if (state == PS_HEART) {
    // HEART's frame index doesn't map directly to a body frame — it's a
    // position along the hue cycle. We override the framesFor()-derived
    // modulo so the sprite renderer redraws every 50 ms for smooth color.
    return (int)((frameMs / periodFor(PS_HEART)) % (unsigned long)HEART_FRAMES);
  }
  size_t n = 0;
  framesFor(state, n);
  if (n == 0) return 0;
  return (int)((frameMs / periodFor(state)) % n);
}

static void drawFrameLines(lgfx::LGFXBase& t, const Frame& f, int cx, int cy,
                           uint16_t color) {
  t.setTextColor(color, 0x0000);
  t.setTextSize(2);

  const int LINE_H = t.fontHeight();
  const char* p = f.lines;

  // Count lines first so we can center vertically.
  int lineCount = 1;
  for (const char* q = p; *q; q++) if (*q == '\n') lineCount++;

  int totalH = lineCount * LINE_H;
  int y = cy - totalH / 2;

  char buf[32];
  while (*p) {
    const char* nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, p, len);
    buf[len] = 0;

    int tw = t.textWidth(buf);
    t.drawString(buf, cx - tw / 2, y);
    y += LINE_H;
    if (!nl) break;
    p = nl + 1;
  }
}

// Filled-pixel heart, scaleable. Two top lobes (filled circles) plus a
// triangle pointing down. `r` controls the radius of the lobes; the
// triangle's width and depth scale with it so the shape stays balanced.
static void drawHeart(lgfx::LGFXBase& t, int cx, int cy, int r, uint16_t color) {
  t.fillCircle(cx - r,     cy,         r, color);
  t.fillCircle(cx + r,     cy,         r, color);
  // Triangle from the outer sides of the lobes down to a point.
  // Slight overlap with the circles so there's no seam.
  t.fillTriangle(cx - 2*r,  cy,
                 cx + 2*r,  cy,
                 cx,        cy + 2*r + r/2 + 2,
                 color);
}

static void catDrawFrame(lgfx::LGFXBase& t, PersonaState state, int frameIdx,
                         int cx, int cy) {
  size_t n = 0;
  const Frame* frames = framesFor(state, n);
  if (!frames || n == 0) return;

  if (state == PS_HEART) {
    // Iridescent / "cangiante" effect: hue rotates smoothly through the
    // full color wheel over the 2-second one-shot. Both hearts and the
    // cat body shimmer together, with the hearts offset ±30° so they're
    // never the same color simultaneously — gives a nice depth feel.
    float hue = (float)frameIdx * (360.0f / (float)HEART_FRAMES);
    uint16_t bodyColor  = hsv565(hue,         1.0f, 1.0f);
    uint16_t leftColor  = hsv565(hue + 30.0f, 1.0f, 1.0f);
    uint16_t rightColor = hsv565(hue - 30.0f, 1.0f, 1.0f);

    // Body: alternate between the two cat-face frames at half-cycle
    // intervals so the face still feels alive.
    int bodyIdx = (frameIdx / (HEART_FRAMES / 2)) % (int)n;
    drawFrameLines(t, frames[bodyIdx], cx, cy + 14, bodyColor);

    // Pulsing radius: 4 → 5 → 5 → 4 every 4 frames (200 ms) so the
    // hearts visibly "beat" alongside the color cycle.
    int r = ((frameIdx / 2) % 2 == 0) ? 4 : 5;
    int heartY = cy - 22;
    drawHeart(t, cx - 22, heartY, r, leftColor);
    drawHeart(t, cx + 22, heartY, r, rightColor);
    return;
  }

  if (frameIdx < 0) frameIdx = 0;
  if ((size_t)frameIdx >= n) frameIdx = (int)(n - 1);
  drawFrameLines(t, frames[frameIdx], cx, cy, colorFor(state));
}

const Buddy CAT_BUDDY = { "cat", catFrameIndex, catDrawFrame };
