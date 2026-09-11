#pragma once

#include <Arduino.h>
#include "buddy.h"
#include "protocol.h"

void uiBegin();
void uiClear();

// One screen-full draw. Caller controls the cadence (e.g. ~50 ms in loop()).
void uiRender(PersonaState state,
              const Snapshot& snap,
              bool usbPlugged,
              uint16_t batPct);

// Full-screen stats panel: name/owner, approve/deny/shake/nap counters,
// derived level, uptime, free heap. Drawn opaque, so the buddy area is
// hidden while this is up.
void uiRenderStats(const Snapshot& snap,
                   uint16_t batPct,
                   bool usbPlugged,
                   uint16_t level);

void uiBacklight(bool on);
