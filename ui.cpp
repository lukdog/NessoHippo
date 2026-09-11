// Screen composition: top HUD strip + central buddy + optional approval
// overlay.
//
// The buddy area is double-buffered through an LGFX_Sprite: each frame
// we clear the sprite, paint the buddy into it, and push it to the panel
// in a single DMA blit. No fillRect-on-panel between frames → no flicker.
// We also skip the push entirely when the (state, frameIdx) hasn't
// changed since the last render, so when the cat is on a steady frame
// the SPI bus stays idle.
//
// The HUD strip and approval overlay still draw directly on the panel
// since they update infrequently (HUD on real state change, overlay on
// prompt change).
//
// A persona may instead declare ownsFullScreen, in which case it gets the
// whole sprite in landscape and none of the above applies: no background
// fill, no ASCII draw, no HUD. ui.cpp owns the rotation and the sprite for
// both cases, so nothing outside this file may assume either is fixed.

#include "ui.h"
#include <Arduino_Nesso_N1.h>

extern NessoDisplay display;

// Portrait geometry — the frame-based buddies, the HUD and the stats panel.
static const int W = 135;
static const int H = 240;
// Landscape geometry — full-screen personas.
static const int FULL_W = 240;
static const int FULL_H = 135;
// The physical glass, which is what the touch controller reports in no
// matter what the display is doing. Not the same thing as the portrait
// screen width, even though the numbers coincide.
static const int PANEL_SHORT = 135;
static const int PANEL_LONG  = 240;

// Rotation 1 is the library's own verified landscape orientation; the
// ST7789 memory offsets are recomputed per rotation by the panel driver.
static const uint8_t ROT_PORTRAIT  = 0;
static const uint8_t ROT_LANDSCAPE = 1;

static const uint16_t COLOR_BLACK = 0x0000;
static const uint16_t COLOR_WHITE = 0xFFFF;
static const uint16_t COLOR_DIM   = 0x52AA;
static const uint16_t COLOR_ORANGE= 0xed03;

static const int HUD_H = 22;
static const int APPROVAL_H = 70;

// Buddy area is always the full strip below the HUD, full screen width.
// When the approval overlay is shown it just paints over the bottom of
// this area, so we never have to resize the sprite.
static const int BUDDY_TOP = HUD_H + 2;
static const int BUDDY_H   = H - BUDDY_TOP - 2;

// One sprite per persona, allocated on first activation and then kept.
// Freeing and reclaiming 63 KB on every persona switch is how a heap
// shared with NimBLE gets fragmented, and a failed reallocation would
// leave nothing to draw into.
static LGFX_Sprite  s_catSprite(&display);
static LGFX_Sprite  s_faceSprite(&display);
static LGFX_Sprite* s_sprite = nullptr;   // the active one, null if unusable

// What is actually on the panel right now. The stats panel borrows
// portrait from a landscape persona, so this cannot be derived from the
// active buddy.
static uint8_t s_panelRotation = ROT_PORTRAIT;

// Cached HUD state so we only repaint when something changes.
static int       s_lastRunning = -1;
static int       s_lastWaiting = -1;
static String    s_lastMsg;
static bool      s_lastUsb     = false;
static uint16_t  s_lastBat     = 0xFFFF;
static bool      s_lastMuted   = false;
static bool      s_dirty       = true;
static bool      s_lastHadPrompt = false;
static String    s_lastPromptId;

// Cached buddy frame so we don't repush an identical sprite.
static PersonaState s_lastBuddyState   = (PersonaState)-1;
static int          s_lastBuddyFrame   = -1;
static bool         s_buddyForceRedraw = true;

// Allocate the sprite once per persona activation — 240x135x16bpp is
// ~63 KB, far too much to churn per frame.
static void setPanelRotation(uint8_t r) {
  s_panelRotation = r;
  display.setRotation(r);
}

void uiSetBuddy(const Buddy* b) {
  if (!b) return;
  g_activeBuddy = b;

  LGFX_Sprite* spr = b->ownsFullScreen ? &s_faceSprite : &s_catSprite;
  const int w = b->ownsFullScreen ? FULL_W : W;
  const int h = b->ownsFullScreen ? FULL_H : BUDDY_H;
  if (spr->width() != w) {
    spr->setColorDepth(16);
    spr->createSprite(w, h);
    if (spr->width() != w) {
      Serial.printf("uiSetBuddy: %dx%d sprite alloc failed, "
                    "largest free block %u B\n",
                    w, h, (unsigned)ESP.getMaxAllocHeap());
      spr = nullptr;
    }
  }
  s_sprite = spr;

  setPanelRotation(b->ownsFullScreen ? ROT_LANDSCAPE : ROT_PORTRAIT);
  uiClear();
}

void uiBegin(const Buddy* b) {
  uiSetBuddy(b);
}

// NessoTouch (FT6x36) reports raw portrait panel coordinates and knows
// nothing about the display rotation, so a landscape persona has to remap
// or every pet lands in the wrong place.
bool uiMapTouch(int16_t rawX, int16_t rawY, int16_t& x, int16_t& y) {
  // Validate the raw ranges: after mapping, any raw input is in bounds by
  // construction, so a post-hoc check would catch nothing.
  if (rawX < 0 || rawY < 0 || rawX >= PANEL_SHORT || rawY >= PANEL_LONG) {
    return false;
  }
  if (s_panelRotation == ROT_LANDSCAPE) {
    x = rawY;
    y = (int16_t)(PANEL_SHORT - 1 - rawX);
  } else {
    x = rawX;
    y = rawY;
  }
  return true;
}

void uiClear() {
  const bool fullScreen = g_activeBuddy && g_activeBuddy->ownsFullScreen;
  setPanelRotation(fullScreen ? ROT_LANDSCAPE : ROT_PORTRAIT);
  // A full-screen persona repaints every pixel on its next frame, so
  // blanking first would only flash black over a bright background.
  if (!fullScreen) display.fillScreen(COLOR_BLACK);
  s_lastRunning = -1;
  s_lastWaiting = -1;
  s_lastMsg = "";
  s_lastUsb = false;
  s_lastBat = 0xFFFF;
  s_lastMuted = false;
  s_lastHadPrompt = false;
  s_lastPromptId = "";
  s_dirty = true;
  s_buddyForceRedraw = true;
}

// Small speaker-with-slash icon used as the mute indicator in the HUD.
// Drawn around (cx, cy) in an ~11x11 box. The speaker is white, the
// slash is red, so the muted state is unmistakable in peripheral vision.
static void drawMuteIcon(int cx, int cy) {
  const uint16_t COLOR_RED = 0xF800;
  // Speaker body: 3px wide rect on the left.
  display.fillRect(cx - 4, cy - 2, 3, 5, COLOR_WHITE);
  // Speaker cone: triangle to the right.
  display.fillTriangle(cx - 1, cy - 4,
                       cx - 1, cy + 4,
                       cx + 3, cy,      COLOR_WHITE);
  // Red diagonal slash, drawn twice for a 2px line.
  display.drawLine(cx - 5, cy + 5, cx + 5, cy - 5, COLOR_RED);
  display.drawLine(cx - 5, cy + 4, cx + 4, cy - 5, COLOR_RED);
}

static void drawHUD(const Snapshot& snap, bool usbPlugged, uint16_t batPct) {
  display.fillRect(0, 0, W, HUD_H, COLOR_BLACK);
  display.setTextSize(1);

  // Left: "R:1 W:0"
  char buf[24];
  snprintf(buf, sizeof(buf), "R:%d W:%d", snap.running, snap.waiting);
  display.setTextColor(COLOR_WHITE, COLOR_BLACK);
  display.drawString(buf, 4, 4);

  // Right: battery + optional USB bolt.
  char bbuf[8];
  snprintf(bbuf, sizeof(bbuf), "%u%%", (unsigned)batPct);
  int tw = display.textWidth(bbuf);
  int x = W - 4 - tw;
  display.setTextColor(usbPlugged ? COLOR_ORANGE : COLOR_WHITE, COLOR_BLACK);
  display.drawString(bbuf, x, 4);
  if (usbPlugged) {
    int bx = x - 6;
    int by = 8;
    display.fillTriangle(bx, by - 2, bx + 2, by - 5, bx - 2, by + 1, COLOR_ORANGE);
    display.fillTriangle(bx, by - 2, bx - 2, by + 1, bx, by + 1, COLOR_ORANGE);
    display.fillTriangle(bx, by - 2, bx, by + 1, bx - 2, by + 4, COLOR_ORANGE);
  }

  // Mute icon, drawn to the LEFT of either the USB bolt (if present) or
  // the battery %. Stays out of the way of both pieces of text.
  if (snap.muted) {
    int anchor = usbPlugged ? (x - 6) : x;
    drawMuteIcon(anchor - 12, 8);
  }

  // Bottom row: scrolling-ish msg (truncated). Only one line.
  if (snap.msg.length() > 0) {
    String m = snap.msg;
    if (m.length() > 22) m = m.substring(0, 21) + ".";
    display.setTextColor(COLOR_DIM, COLOR_BLACK);
    display.drawString(m, 4, 13);
  }
}

static void drawApprovalOverlay(const Snapshot& snap) {
  int top = H - APPROVAL_H;
  display.fillRect(0, top, W, APPROVAL_H, COLOR_BLACK);
  display.drawFastHLine(0, top, W, COLOR_ORANGE);

  display.setTextSize(2);
  display.setTextColor(COLOR_ORANGE, COLOR_BLACK);
  String tool = snap.promptTool.length() ? snap.promptTool : String("?");
  if (tool.length() > 10) tool = tool.substring(0, 10);
  int tw = display.textWidth(tool);
  display.drawString(tool, (W - tw) / 2, top + 6);

  display.setTextSize(1);
  display.setTextColor(COLOR_WHITE, COLOR_BLACK);
  String hint = snap.promptHint;
  String l1, l2;
  if (hint.length() <= 22) {
    l1 = hint;
  } else {
    int br = hint.lastIndexOf(' ', 22);
    if (br < 8) br = 22;
    l1 = hint.substring(0, br);
    l2 = hint.substring(br + 1);
    if (l2.length() > 22) l2 = l2.substring(0, 21) + ".";
  }
  display.drawString(l1, 4, top + 28);
  if (l2.length()) display.drawString(l2, 4, top + 38);

  display.setTextColor(COLOR_DIM, COLOR_BLACK);
  display.drawString("K1=approve K2=deny", 4, top + APPROVAL_H - 10);
}

void uiRender(PersonaState state,
              const Snapshot& snap,
              bool usbPlugged,
              uint16_t batPct,
              const FaceParams* face) {

  // Full-screen persona: it owns every pixel, so skip the background
  // fill, the ASCII draw and the HUD entirely and just hand over the
  // sprite. It repaints every frame, so there is no frame-skip cache.
  if (g_activeBuddy && g_activeBuddy->ownsFullScreen) {
    if (!s_sprite || !g_activeBuddy->drawFull || !face) return;
    g_activeBuddy->drawFull(*s_sprite, *face, (uint32_t)millis());
    s_sprite->pushSprite(0, 0);
    return;   // leave s_dirty alone: the cat path owes a fillScreen
  }

  bool hudChanged =
      (snap.running != s_lastRunning) ||
      (snap.waiting != s_lastWaiting) ||
      (snap.msg     != s_lastMsg)     ||
      (usbPlugged   != s_lastUsb)     ||
      (batPct       != s_lastBat)     ||
      (snap.muted   != s_lastMuted);

  if (s_dirty) {
    display.fillScreen(COLOR_BLACK);
    s_dirty = false;
    hudChanged = true;
    s_buddyForceRedraw = true;
  }

  if (hudChanged) {
    drawHUD(snap, usbPlugged, batPct);
    s_lastRunning = snap.running;
    s_lastWaiting = snap.waiting;
    s_lastMsg     = snap.msg;
    s_lastUsb     = usbPlugged;
    s_lastBat     = batPct;
    s_lastMuted   = snap.muted;
  }

  // Buddy: double-buffered through the sprite. Compute the frame index
  // first so we can skip the whole pipeline when nothing's changed.
  int frameIdx = 0;
  if (g_activeBuddy && g_activeBuddy->frameIndex) {
    frameIdx = g_activeBuddy->frameIndex(state, millis());
  }
  bool buddyChanged = s_buddyForceRedraw ||
                      state    != s_lastBuddyState ||
                      frameIdx != s_lastBuddyFrame;

  if (buddyChanged && s_sprite && g_activeBuddy && g_activeBuddy->drawFrame) {
    s_sprite->fillSprite(COLOR_BLACK);
    int cx = W / 2;
    int cy = BUDDY_H / 2;
    g_activeBuddy->drawFrame(*s_sprite, state, frameIdx, cx, cy);
    s_sprite->pushSprite(0, BUDDY_TOP);
    s_lastBuddyState   = state;
    s_lastBuddyFrame   = frameIdx;
    s_buddyForceRedraw = false;
  }

  // Approval overlay, drawn over the bottom of the just-pushed buddy.
  if (state == PS_ATTENTION && snap.hasPrompt) {
    // Repaint the overlay only when the prompt actually changed; the
    // sprite push above won't touch the overlay region after the first
    // paint because we re-paint it here whenever we push.
    bool promptChanged = (snap.promptId != s_lastPromptId) || !s_lastHadPrompt;
    if (promptChanged || buddyChanged) {
      drawApprovalOverlay(snap);
    }
    s_lastHadPrompt = true;
    s_lastPromptId  = snap.promptId;
  } else if (s_lastHadPrompt) {
    // Prompt just cleared — the next sprite push will repaint the area
    // it covers, but the overlay extended below the sprite (down to H),
    // so clear those last 2 px too.
    display.fillRect(0, H - 2, W, 2, COLOR_BLACK);
    s_lastHadPrompt = false;
    s_lastPromptId  = "";
    s_buddyForceRedraw = true; // force one full repaint
  }
}

// Format a duration in seconds into a compact string: "5s", "2m 17s",
// "1h 04m". Sub-second precision isn't useful for nap-tracking and
// would just clutter the panel.
static void fmtDuration(uint32_t sec, char* buf, size_t bufsz) {
  uint32_t h = sec / 3600;
  uint32_t m = (sec / 60) % 60;
  uint32_t s = sec % 60;
  if (h > 0)      snprintf(buf, bufsz, "%uh %02um", (unsigned)h, (unsigned)m);
  else if (m > 0) snprintf(buf, bufsz, "%um %02us", (unsigned)m, (unsigned)s);
  else            snprintf(buf, bufsz, "%us",       (unsigned)s);
}

void uiRenderStats(const Snapshot& snap,
                   uint16_t batPct,
                   bool usbPlugged,
                   uint16_t level) {
  // The panel layout is portrait-only. A landscape persona lends it the
  // rotation for as long as it is up; uiClear() on dismissal puts the
  // persona's own rotation back.
  setPanelRotation(ROT_PORTRAIT);
  display.fillScreen(COLOR_BLACK);

  // Title bar.
  display.setTextColor(COLOR_ORANGE, COLOR_BLACK);
  display.setTextSize(2);
  const char* title = "NessoBuddy";
  int tw = display.textWidth(title);
  display.drawString(title, (W - tw) / 2, 8);
  display.drawFastHLine(8, 30, W - 16, COLOR_ORANGE);

  // Body — size-1 text, two columns (label, value).
  display.setTextSize(1);
  int y = 38;
  const int LH = 11; // line height at size 1
  const int VX = 64; // value column x

  auto row = [&](const char* label, const String& value, uint16_t labelColor) {
    display.setTextColor(labelColor, COLOR_BLACK);
    display.drawString(label, 6, y);
    display.setTextColor(COLOR_WHITE, COLOR_BLACK);
    display.drawString(value, VX, y);
    y += LH;
  };

  row("Name",  snap.buddyName.length() ? snap.buddyName : String("(none)"),
       COLOR_DIM);
  row("Owner", snap.ownerName.length() ? snap.ownerName : String("(none)"),
       COLOR_DIM);

  y += 4;
  display.drawFastHLine(8, y, W - 16, COLOR_DIM);
  y += 6;

  char buf[32];
  snprintf(buf, sizeof(buf), "%u", (unsigned)snap.approveCount);
  row("Approve", String(buf), COLOR_ORANGE);
  snprintf(buf, sizeof(buf), "%u", (unsigned)snap.denyCount);
  row("Deny",    String(buf), COLOR_ORANGE);
  snprintf(buf, sizeof(buf), "%u", (unsigned)snap.shakeCount);
  row("Shake",   String(buf), COLOR_ORANGE);
  snprintf(buf, sizeof(buf), "%u", (unsigned)snap.petCount);
  row("Pets",    String(buf), 0xF81F); // magenta — matches the heart color
  fmtDuration(snap.napSeconds, buf, sizeof(buf));
  row("Nap",     String(buf), COLOR_ORANGE);

  y += 4;
  display.drawFastHLine(8, y, W - 16, COLOR_DIM);
  y += 6;

  snprintf(buf, sizeof(buf), "%u", (unsigned)level);
  row("Level", String(buf), COLOR_WHITE);
  fmtDuration((uint32_t)(millis() / 1000), buf, sizeof(buf));
  row("Up",    String(buf), COLOR_DIM);
  snprintf(buf, sizeof(buf), "%u KB", (unsigned)(ESP.getFreeHeap() / 1024));
  row("Heap",  String(buf), COLOR_DIM);
  snprintf(buf, sizeof(buf), "%u%%%s", (unsigned)batPct, usbPlugged ? " +" : "");
  row("Bat",   String(buf), COLOR_DIM);

  // Footer hint.
  display.setTextColor(COLOR_DIM, COLOR_BLACK);
  const char* hint = "K2 long: back";
  int hw = display.textWidth(hint);
  display.drawString(hint, (W - hw) / 2, H - 14);
}

void uiBacklight(bool on) {
  digitalWrite(LCD_BACKLIGHT, on ? HIGH : LOW);
}
