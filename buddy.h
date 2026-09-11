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

// A Buddy is just a pair of callbacks. Keeping them as function pointers
// (rather than virtual methods) lets us hold a const Buddy in flash.
struct Buddy {
  const char* name;
  // Resolve the wall-clock millis() into a stable frame index for the
  // given state. ui.cpp uses this to detect "same frame as last time"
  // and skip the redraw + push entirely.
  int  (*frameIndex)(PersonaState state, unsigned long frameMs);
  // Draw `state`/`frameIdx` centered on `(cx, cy)` into `target`. Target
  // is an LGFXBase so the same code paints either the live panel or an
  // off-screen sprite — the latter is how ui.cpp gets flicker-free
  // double buffering.
  void (*drawFrame)(lgfx::LGFXBase& target,
                    PersonaState state, int frameIdx, int cx, int cy);
};

extern const Buddy CAT_BUDDY;

// Active buddy — for now, always the cat. Kept as a pointer to make
// pluggability cheap if a second buddy is added later.
extern const Buddy* g_activeBuddy;
