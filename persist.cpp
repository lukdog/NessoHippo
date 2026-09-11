// NVS-backed persistence for stats and identity fields.
//
// Why coalesced writes: NVS is in flash and has finite erase cycles. The
// hot path (KEY1 → approveCount++ → markDirty) might fire many times in
// a session, but we only actually putUInt() once per FLUSH_INTERVAL_MS,
// or when something forces a flush (disconnect, reboot path, etc.).
//
// Why a separate "namespace" string: Preferences keys the NVS partition
// by namespace so different sketches on the same chip don't collide.

#include "persist.h"

#include <Preferences.h>
#include <math.h>

static Preferences   s_prefs;
static bool          s_open        = false;
static bool          s_dirty       = false;
static unsigned long s_lastFlushMs = 0;

#define NS_NAME "NessoBuddy"

// Don't flush more often than this. Most sessions will see one flush at
// most before the next disconnect; if there's a sustained stream of
// updates (e.g. spamming KEY1) we still coalesce them.
#define FLUSH_INTERVAL_MS 30000UL

void persistBegin(Snapshot& s) {
  if (!s_open) {
    s_prefs.begin(NS_NAME, /*readOnly=*/false);
    s_open = true;
  }
  s.approveCount = s_prefs.getUInt("appr",  0);
  s.denyCount    = s_prefs.getUInt("deny",  0);
  s.shakeCount   = s_prefs.getUInt("shake", 0);
  s.napSeconds   = s_prefs.getUInt("nap",   0);
  s.petCount     = s_prefs.getUInt("pets",  0);
  s.muted        = s_prefs.getBool("muted", false);
  s.persona      = s_prefs.getUChar("persona", 1);   // NessoFace by default

  s.tokenBaseline   = s_prefs.getFloat("base",  0.0f);
  s.lastDayIndex    = s_prefs.getUInt ("day",   0);
  s.tokensYesterday = (long)s_prefs.getUInt("tokyd", 0);

  String n = s_prefs.getString("name",  "");
  if (n.length() > 0) s.buddyName = n;
  String o = s_prefs.getString("owner", "");
  if (o.length() > 0) s.ownerName = o;

  s_dirty       = false;
  s_lastFlushMs = millis();
}

void persistMarkDirty() {
  s_dirty = true;
}

void persistFlush(const Snapshot& s) {
  if (!s_open || !s_dirty) return;
  // putUInt / putString already no-op when the value is identical to
  // what's stored, so this is safe to call indiscriminately.
  s_prefs.putUInt("appr",  s.approveCount);
  s_prefs.putUInt("deny",  s.denyCount);
  s_prefs.putUInt("shake", s.shakeCount);
  s_prefs.putUInt("nap",   s.napSeconds);
  s_prefs.putUInt("pets",  s.petCount);
  s_prefs.putBool("muted", s.muted);
  s_prefs.putUChar("persona", s.persona);
  s_prefs.putFloat("base",  s.tokenBaseline);
  s_prefs.putUInt ("day",   s.lastDayIndex);
  s_prefs.putUInt ("tokyd", (uint32_t)(s.tokensYesterday < 0 ? 0
                                                             : s.tokensYesterday));
  s_prefs.putString("name",  s.buddyName);
  s_prefs.putString("owner", s.ownerName);
  s_dirty       = false;
  s_lastFlushMs = millis();
}

void persistTick(const Snapshot& s) {
  if (!s_dirty) return;
  if (millis() - s_lastFlushMs < FLUSH_INTERVAL_MS) return;
  persistFlush(s);
}

uint16_t persistLevel(const Snapshot& s) {
  // Shakes and pets count for ~quarter weight so the level doesn't
  // shoot up just from poking the device. Real approves/denies are
  // the heavy contributors.
  uint32_t total = s.approveCount + s.denyCount +
                   (s.shakeCount / 4) + (s.petCount / 4);
  uint16_t lvl = (uint16_t)(1 + (uint32_t)sqrtf((float)total));
  return lvl;
}
