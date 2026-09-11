#pragma once

#include <Arduino.h>
#include <Arduino_Nesso_N1.h>  // pulls in M5GFX (LovyanGFX) — needed for LGFXBase

// One persona state per upstream's seven-frame animation taxonomy.
enum PersonaState {
  PS_SLEEP,
  PS_IDLE,
  PS_BUSY,
  PS_ATTENTION,
  PS_CELEBRATE,
  PS_DIZZY,
  PS_HEART,
};

// Parameter vector for full-screen parametric buddies. Defined in face.h;
// only ever passed around by reference here.
struct FaceParams;

// Resolve the wall-clock millis() into a stable frame index for the
// given state. ui.cpp uses this to detect "same frame as last time"
// and skip the redraw + push entirely.
typedef int (*BuddyFrameIndexFn)(PersonaState state, unsigned long frameMs);

// Draw `state`/`frameIdx` centered on `(cx, cy)` into `target`. Target
// is an LGFXBase so the same code paints either the live panel or an
// off-screen sprite — the latter is how ui.cpp gets flicker-free
// double buffering.
typedef void (*BuddyDrawFrameFn)(lgfx::LGFXBase& target,
                                 PersonaState state, int frameIdx,
                                 int cx, int cy);

// Full-screen variant: gets the whole sprite and owns it, clear included.
// Frame-based buddies leave this null.
typedef void (*BuddyDrawFullFn)(LGFX_Sprite& spr, const FaceParams& p,
                                uint32_t nowMs);

// A Buddy is just a set of callbacks. Keeping them as function pointers
// (rather than virtual methods) lets us hold a const Buddy in flash.
// A buddy is either frame-based (frameIndex + drawFrame, the ASCII pets)
// or full-screen parametric (drawFull + ownsFullScreen); never both.
struct Buddy {
  const char*       name;
  BuddyFrameIndexFn frameIndex;      // null for full-screen buddies
  BuddyDrawFrameFn  drawFrame;       // null for full-screen buddies
  BuddyDrawFullFn   drawFull;        // null for frame-based buddies
  bool              ownsFullScreen;  // skip bg fill + HUD, hand over the sprite
};

extern const Buddy CAT_BUDDY;
extern const Buddy FACE_BUDDY;

// Active buddy. Never assign to this directly — go through uiSetBuddy(),
// which also owns the sprite and the display rotation.
extern const Buddy* g_activeBuddy;
