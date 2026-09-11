// Mood from tokens.
//
//   mood = clamp(tokens_today / baseline, 0, MOOD_CAP) / MOOD_CAP
//
// The baseline is an exponential moving average of the owner's own
// previous daily totals, so the face reacts to "busier than usual for
// you", not to some global idea of a heavy day. That relativity is the
// whole point: do not replace the baseline with a hardcoded token target.

#include "mood.h"
#include "persist.h"

static const float BASELINE_ALPHA = 0.2f;      // weight of the day just ended
static const float BASELINE_SEED  = 50000.0f;  // so day one still means something
static const float MOOD_CAP       = 1.5f;      // 150% of average reads as maxed

static const uint32_t SECS_PER_DAY = 86400;

void moodOnTime(uint32_t unixSec, int32_t tzOffsetSec, Snapshot& s) {
  const uint32_t day =
      (uint32_t)(((int64_t)unixSec + (int64_t)tzOffsetSec) / (int64_t)SECS_PER_DAY);

  if (s.tokenBaseline <= 0.0f) {
    // First run: seed rather than starting from zero, which would peg the
    // face to delighted on the very first token.
    s.tokenBaseline = BASELINE_SEED;
    s.lastDayIndex  = day;
    persistMarkDirty();
    return;
  }

  if (day == s.lastDayIndex) return;

  // Local midnight. tokensToday still holds the total for the day that
  // just ended, unless the desktop happened to reset it first — one
  // slightly-off day is not worth tracking wall-clock ourselves.
  s.tokensYesterday = s.tokensToday;
  s.tokenBaseline   = (1.0f - BASELINE_ALPHA) * s.tokenBaseline +
                      BASELINE_ALPHA * (float)s.tokensYesterday;
  s.lastDayIndex    = day;
  persistMarkDirty();
}

float moodValue(const Snapshot& s) {
  float baseline = s.tokenBaseline;
  if (baseline <= 0.0f) baseline = BASELINE_SEED;   // no time sync yet
  float ratio = (float)s.tokensToday / baseline;
  if (ratio < 0.0f)     ratio = 0.0f;
  if (ratio > MOOD_CAP) ratio = MOOD_CAP;
  return ratio / MOOD_CAP;
}
