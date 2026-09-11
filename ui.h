#pragma once

#include <Arduino.h>
#include "buddy.h"
#include "protocol.h"

// Pass the persona to start with; also allocates its sprite.
void uiBegin(const Buddy* b);
void uiClear();

// Activate a persona: sets the display rotation it needs and (re)allocates
// the buddy sprite at the matching size. ui.cpp owns both, so nothing else
// may assume a fixed rotation or sprite geometry.
void uiSetBuddy(const Buddy* b);

// One screen-full draw. Caller controls the cadence (e.g. ~50 ms in loop()).
// `face` is only read by full-screen personas; frame-based ones get nullptr.
void uiRender(PersonaState state,
              const Snapshot& snap,
              bool usbPlugged,
              uint16_t batPct,
              const FaceParams* face);

// Map a raw NessoTouch coordinate into the active persona's screen frame.
// Returns false if the point falls outside the visible area.
bool uiMapTouch(int16_t rawX, int16_t rawY, int16_t& x, int16_t& y);

// Full-screen stats panel: name/owner, approve/deny/shake/nap counters,
// derived level, uptime, free heap. Drawn opaque, so the buddy area is
// hidden while this is up.
void uiRenderStats(const Snapshot& snap,
                   uint16_t batPct,
                   bool usbPlugged,
                   uint16_t level,
                   unsigned long gapMs);

void uiBacklight(bool on);
