#pragma once

#include "protocol.h"

// Fold a {"time":[unix, tzOffsetSec]} message into the day tracker. On a
// local-midnight rollover this is what advances the baseline.
void moodOnTime(uint32_t unixSec, int32_t tzOffsetSec, Snapshot& s);

// 0.0 miserable .. 1.0 delighted. Today's token use measured against the
// owner's own recent daily average — never against a fixed target.
float moodValue(const Snapshot& s);
