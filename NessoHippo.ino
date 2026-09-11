/*
  NessoHippo - NessoBuddy plus NessoFace, on the Arduino Nesso N1.

  Connects over Bluetooth LE (Nordic UART Service) to a Claude desktop
  running in developer mode. Two personas share the same session state:
  the original ASCII cat, and NessoFace - a full-screen parametric face
  whose mood tracks today's token use against the owner's own average.
  KEY1/KEY2 approve or deny pending tool prompts.

  Hardware mapping vs. upstream M5StickC Plus:
    KEY1 (front)   -> approve  (button A)
    KEY2 (side)    -> deny     (button B)
    BEEP_PIN       -> celebrate melody / attention beep
    IMU shake      -> DIZZY one-shot
    IMU face-down  -> SLEEP

  Out of scope (vs. upstream): GIF character packs, folder-push xfer,
  persistent NVS stats, on-device menus.

  Compile: arduino-cli compile -b esp32:esp32:arduino_nesso_n1 NessoHippo
  Upload:  arduino-cli upload  -b esp32:esp32:arduino_nesso_n1 \
                                -p /dev/cu.usbmodem1101 NessoHippo
*/

#include <Arduino_Nesso_N1.h>

#include "ble_bridge.h"
#include "protocol.h"
#include "buddy.h"
#include "face.h"
#include "motion.h"
#include "ui.h"
#include "persist.h"
#include "mood.h"

NessoDisplay display;
NessoBattery battery;
NessoTouch   touch;

static Snapshot      snap;
static ProtocolFx    fx;

// One-shot animation overlay (DIZZY/CELEBRATE/HEART). Lasts ONE_SHOT_MS.
static unsigned long s_oneShotStartMs = 0;
static PersonaState  s_oneShotState   = PS_IDLE;
static bool          s_oneShotActive  = false;
#define ONE_SHOT_MS 2000UL

// Battery snapshot, refreshed every 2 s.
static unsigned long s_lastBatteryPollMs = 0;
static uint16_t      s_batPct = 100;
static bool          s_usbPlugged = false;
#define BATTERY_POLL_MS 2000UL

// Auto-off (backlight only). Triggers after 30 s with no attention and no
// USB. Matches the spirit of upstream's auto-off.
static unsigned long s_lastInteractionMs = 0;
static bool          s_backlightOn       = true;
#define AUTO_OFF_MS 30000UL

// Waking back up. The screen used to be a one-way trip: the only things
// that could switch it back on were a pending prompt or USB power, and
// with the desk computer asleep neither can happen — so on battery it
// stayed dark until a button press. A touch, a nudge or a resumed
// heartbeat now bring it back too.
//
// When the computer is away the wake is deliberately half-hearted: crack
// one eye open, then go back to sleep, rather than burning 30 s of
// backlight for someone who only knocked the desk. PEEK_CLOSE_MS exists
// so the eye is seen closing instead of the screen cutting out mid-blink.
#define PEEK_OPEN_MS   1800UL
#define PEEK_CLOSE_MS  1200UL
#define PEEK_COOLDOWN_MS 6000UL
static unsigned long s_peekUntilMs   = 0;   // one eye open until here
static unsigned long s_peekOffAtMs   = 0;   // backlight off at here
static unsigned long s_lastPeekEndMs = 0;

// Nap-time accounting. While the device is face-down (SLEEP), we
// accumulate elapsed seconds into snap.napSeconds at 1 Hz granularity.
static unsigned long s_napAccumMs        = 0;  // sub-second carry
static bool          s_wasFaceDown       = false;
static unsigned long s_lastLoopMs        = 0;  // measured loop period

// On-device stats overlay: KEY2 long-press toggles a fullscreen panel
// with the persisted counters, level, uptime, heap and battery. Painted
// once on entry (no continuous repaint, so no flicker), dismissed by a
// short press on either button, auto-timeout, or an incoming prompt.
static bool          s_statsOverlay      = false;
static unsigned long s_statsEnteredMs    = 0;
#define STATS_AUTO_DISMISS_MS  20000UL

// Touchscreen pet detection. The FT6x36 is single-touch — we just
// edge-detect "finger landed" and trigger one HEART one-shot per
// touch. PET_REFRACTORY_MS keeps rapid repeated touches from spamming
// the counter (still feels responsive at ~3 pets/sec).
// The controller reports raw portrait coordinates regardless of the
// display rotation, so every tap goes through uiMapTouch(). Verified on
// hardware in landscape: tapping the two eyes and a nostril lands within
// 2-7 px on x, and the nostril reads near the bottom of the screen rather
// than the top, which is what pins down the y direction. Set TOUCH_DEBUG
// to 1 to log raw and mapped coordinates again.
#define TOUCH_DEBUG 0
// Traces the resolved face state and mood to serial. Verification aid for
// the section 14 acceptance run; off in normal use.
#define FACE_DEBUG 0
// Logs BLE connection transitions and heartbeat age. Diagnostic for the
// "does not come back after the Mac locks" case.
#define BLE_DEBUG 1
static bool          s_wasTouched         = false;
static unsigned long s_lastPetMs          = 0;
static bool          s_touchReady         = false;
#define PET_REFRACTORY_MS       300UL

// Long-press machines for both buttons. The short press still does its
// normal action on RELEASE, the long-press fires while still held and
// is then "consumed" so the release doesn't double-fire.
//   KEY1 long  — toggle mute
//   KEY2 long  — toggle stats overlay
static unsigned long s_key1DownMs        = 0;
static bool          s_key1LongFired     = false;
static unsigned long s_key2DownMs        = 0;
static bool          s_key2LongFired     = false;
#define LONG_PRESS_MS          1500UL
// Track previous BLE connection state so we can flush stats on disconnect
// (flash wear: better to write once on disconnect than on every event).
static bool          s_wasConnected      = false;

// Full-screen personas repaint every pixel, so they get an explicit frame
// clock instead of riding the loop period: the parameter interpolation is
// tuned for 30 fps and a 64 KB sprite push costs ~13 ms over SPI.
#define FACE_FRAME_MS          33UL
static unsigned long s_lastFaceFrameMs  = 0;

// NessoFace's ASLEEP rule is "the owner is not at the desk", approximated
// by the BLE heartbeat: no snapshot for HEARTBEAT_STALE_MS means the
// desktop is asleep or out of range. Coming back after a long silence
// earns a brief "oh, you're back" CURIOUS.
#define HEARTBEAT_STALE_MS  30000UL
// Spec section 11 asks for the "oh, you're back" chirp after >2 minutes of
// silence, but section 14 step 7 expects it after the 30 s that put the
// face to sleep. Waking from ASLEEP is the coherent trigger, so this
// matches HEARTBEAT_STALE_MS; set it to 120000 for the stricter reading.
#define WELCOME_BACK_MS     HEARTBEAT_STALE_MS
static unsigned long s_lastHeartbeatMs   = 0;
static unsigned long s_faceCuriousUntilMs = 0;

// Returned by faceStateFor() to mean "no discrete state applies — use the
// mood-driven resting blend".
static const FaceState FS_MOOD_IDLE = FS_COUNT;

// Buzzer state machine (copied pattern from NessoHourglass).
struct Note { uint16_t freq; uint16_t durMs; };
static const Note CELEBRATE_MELODY[] = {
  { 523, 100 }, { 659, 100 }, { 784, 100 }, { 1047, 280 }, { 0, 0 }
};
// Two-tone rising "alert" chirp — used when a permission prompt arrives.
// Kept short so it isn't obnoxious when repeated.
static const Note ATTENTION_CHIRP[] = {
  { 880, 90 }, { 1320, 110 }, { 0, 0 }
};
// While the prompt is still pending, replay the chirp this often so the
// user can't miss it. Stops the moment KEY1/KEY2 clears snap.hasPrompt.
#define ATTENTION_REPEAT_MS 5000UL
static unsigned long s_lastAttentionChirpMs = 0;
static bool          s_lastHadPrompt        = false;
static const Note* s_melodyPtr        = nullptr;
static int         s_melodyIdx        = 0;
static unsigned long s_melodyNoteStartMs = 0;
static Note        s_oneShotNote[2];
#define BUZZER_DUTY 32

static void setupBuzzer() {
  ledcAttach(BEEP_PIN, 4000, 8);
  ledcWrite(BEEP_PIN, 0);
}

static void startMelody(const Note* notes) {
  // Single chokepoint for every sound the device makes: bail out cleanly
  // when muted so any caller (attention chirp, celebrate jingle, deny
  // beep, pet beep, melody updates already in flight) goes silent.
  if (snap.muted) {
    ledcWrite(BEEP_PIN, 0);
    s_melodyPtr = nullptr;
    return;
  }
  s_melodyPtr = notes;
  s_melodyIdx = 0;
  s_melodyNoteStartMs = millis();
  if (notes[0].freq > 0) {
    ledcWriteTone(BEEP_PIN, notes[0].freq);
    ledcWrite(BEEP_PIN, BUZZER_DUTY);
  } else {
    ledcWrite(BEEP_PIN, 0);
  }
}

static void playBeep(uint16_t freq, uint16_t durMs) {
  s_oneShotNote[0] = { freq, durMs };
  s_oneShotNote[1] = { 0, 0 };
  startMelody(s_oneShotNote);
}

static void updateMelody() {
  if (!s_melodyPtr) return;
  if (millis() - s_melodyNoteStartMs < s_melodyPtr[s_melodyIdx].durMs) return;
  s_melodyIdx++;
  s_melodyNoteStartMs = millis();
  const Note& n = s_melodyPtr[s_melodyIdx];
  if (n.freq == 0 && n.durMs == 0) {
    ledcWrite(BEEP_PIN, 0);
    s_melodyPtr = nullptr;
    return;
  }
  if (n.freq > 0) {
    ledcWriteTone(BEEP_PIN, n.freq);
    ledcWrite(BEEP_PIN, BUZZER_DUTY);
  } else {
    ledcWrite(BEEP_PIN, 0);
  }
}

// The desktop is asleep, locked, or out of range.
static bool computerAway(unsigned long now) {
  return !bleIsConnected() || (now - s_lastHeartbeatMs >= HEARTBEAT_STALE_MS);
}

// Bring the screen back. `allowPeek` asks for the half-awake version, but
// only counts when the screen was actually off: waking a screen that is
// already on must not cut its remaining time from 30 s down to three.
static void wakeScreen(unsigned long now, bool allowPeek) {
  const bool wasOff = !s_backlightOn;
  if (wasOff) {
    uiBacklight(true);
    s_backlightOn = true;
  }
  s_lastInteractionMs = now;
  if (allowPeek && wasOff) {
    s_peekUntilMs = now + PEEK_OPEN_MS;
    s_peekOffAtMs = now + PEEK_OPEN_MS + PEEK_CLOSE_MS;
  } else {
    s_peekUntilMs = 0;
    s_peekOffAtMs = 0;
  }
}

static void triggerOneShot(PersonaState s) {
  s_oneShotActive  = true;
  s_oneShotState   = s;
  s_oneShotStartMs = millis();
}

static bool isCharging() {
  NessoBattery::ChargeStatus cs = battery.getChargeStatus();
  return (cs == NessoBattery::CHARGING || cs == NessoBattery::PRE_CHARGE);
}

static void pollBatteryIfDue() {
  unsigned long now = millis();
  if (now - s_lastBatteryPollMs < BATTERY_POLL_MS) return;
  s_lastBatteryPollMs = now;
  uint16_t l = battery.getChargeLevel();
  if (l > 100) l = 100;
  s_batPct = l;
  s_usbPlugged = isCharging();
}

// Derive the persona state from the snapshot + motion + connection.
// One-shots (DIZZY/CELEBRATE/HEART) are layered on top of the base state.
static PersonaState deriveBase() {
  if (!bleIsConnected())  return PS_SLEEP;
  if (motionFaceDown())   return PS_SLEEP;
  if (snap.hasPrompt)     return PS_ATTENTION;
  if (snap.running > 0)   return PS_BUSY;
  return PS_IDLE;
}

// NessoFace's priority order (spec section 11), mapped off the persona
// state machine above. The face has a different vocabulary from the cat:
// no CELEBRATE, no ATTENTION overlay, and a resting state that is a blend
// rather than a table row. ASLEEP is checked first and independently, so
// it outranks the one-shots.
static FaceState faceStateFor(PersonaState st, unsigned long now) {
  if (computerAway(now) || motionFaceDown() || !s_backlightOn) {
    // Nudged or petted with nobody at the desk: one eye, then back down.
    if (s_peekUntilMs && now < s_peekUntilMs && !motionFaceDown()) {
      return FS_PEEK;
    }
    return FS_ASLEEP;
  }
  switch (st) {
    case PS_DIZZY:     return FS_DIZZY;
    case PS_HEART:     return FS_HEART;
    // Approving something is a positive beat and HEART is the face's only
    // positive one-shot, so the celebrate jingle is not left silent.
    case PS_CELEBRATE: return FS_HEART;
    case PS_ATTENTION: return FS_CURIOUS;
    case PS_BUSY:      return FS_BUSY;
    default:           break;
  }
  if (snap.waiting > 0)               return FS_CURIOUS;
  if (now < s_faceCuriousUntilMs)     return FS_CURIOUS;
  return FS_MOOD_IDLE;
}

static PersonaState currentState() {
  if (s_oneShotActive) {
    if (millis() - s_oneShotStartMs < ONE_SHOT_MS) return s_oneShotState;
    s_oneShotActive = false;
  }
  return deriveBase();
}

void setup() {
  Serial.begin(115200);

  display.begin();
  display.fillScreen(0x0000);
  // Rotation belongs to ui.cpp: it depends on which persona is active.

  motionBegin();

  // The FT6x36 lives on the same I2C bus as everything else. begin()
  // returns false if the IC isn't acking — in that case we silently
  // disable pet detection so a misprobing board doesn't break the rest
  // of the sketch.
  s_touchReady = touch.begin();

  battery.begin();
  battery.enableCharge();
  uint16_t l = battery.getChargeLevel();
  if (l > 100) l = 100;
  s_batPct = l;
  s_usbPlugged = isCharging();
  s_lastBatteryPollMs = millis();

  setupBuzzer();

  // Load persisted state BEFORE BLE comes up, so the first heartbeat /
  // status request already sees the prior totals + buddy/owner names.
  // Also before uiBegin(), which needs the persona to size its sprite.
  persistBegin(snap);

  uiBegin(snap.persona ? &FACE_BUDDY : &CAT_BUDDY);

  bleBegin("Claude");

  s_lastInteractionMs = millis();
}

void loop() {
  unsigned long now = millis();

  // 1. Drain BLE lines → protocol. bleTick() first, so a stale connection
  //    flag is repaired before anything reads it this iteration.
  bleTick();
  String line;
  while (blePopLine(line)) {
    fx = ProtocolFx{};
    protocolHandleLine(line, snap, fx);

    if (fx.heartbeat) {
      if (s_lastHeartbeatMs != 0 &&
          now - s_lastHeartbeatMs >= WELCOME_BACK_MS) {
        s_faceCuriousUntilMs = now + ONE_SHOT_MS;
      }
      s_lastHeartbeatMs = now;
      // The computer is being used again: come properly awake.
      wakeScreen(now, false);
    }
    if (fx.statusRequested) {
      bleSendLine(protocolBuildStatus(snap, s_usbPlugged, s_batPct));
    }
    if (fx.ownerChanged) {
      triggerOneShot(PS_HEART);
    }
    if (fx.unpaired) {
      // Drop the session so we go back to SLEEP via the disconnect path,
      // but keep what is persisted — stats, identity and the mood
      // baseline. A whole-Snapshot reset would zero those and the next
      // flush would write the zeros to flash.
      snap.total = snap.running = snap.waiting = 0;
      snap.msg = "";
      snap.tokens = snap.tokensToday = 0;
      snap.hasPrompt = false;
      snap.promptId = snap.promptTool = snap.promptHint = "";
    }
    s_lastInteractionMs = now;
  }

  // 1b. Attention chirp: fire once when a prompt first appears, then
  //     re-fire every ATTENTION_REPEAT_MS until snap.hasPrompt clears.
  //     This is intentionally placed AFTER the BLE drain so a prompt
  //     that just arrived in this iteration gets immediate audio.
  if (snap.hasPrompt) {
    if (!s_lastHadPrompt ||
        now - s_lastAttentionChirpMs >= ATTENTION_REPEAT_MS) {
      startMelody(ATTENTION_CHIRP);
      s_lastAttentionChirpMs = now;
    }
  }
  s_lastHadPrompt = snap.hasPrompt;

  // 2. Motion.
  motionTick();
  if (motionConsumeShake()) {
    triggerOneShot(PS_DIZZY);
    snap.shakeCount++;
    persistMarkDirty();
    wakeScreen(now, computerAway(now));
  }
  // A nudge only wakes the screen — no stat, no one-shot. The cooldown is
  // on top of the detector's own refractory, so a fidgety desk cannot make
  // the face peek every few seconds.
  if (motionConsumeNudge()) {
    const bool tooSoon = !s_backlightOn &&
                         (now - s_lastPeekEndMs < PEEK_COOLDOWN_MS);
    if (!tooSoon) wakeScreen(now, computerAway(now));
  }

  // 2b. Touchscreen "pet" detection. Rising edge of touch → one HEART
  //     one-shot + pet count. We ignore touches while the stats overlay
  //     is up (a tap there means "dismiss", handled below) and while a
  //     permission prompt is pending (don't bury the user's intent to
  //     hit a physical button by playing the heart melody).
  bool touchedNow = s_touchReady && touch.isTouched();
  if (touchedNow && !s_wasTouched && !s_statsOverlay && !snap.hasPrompt) {
    // read() only once, and only after the edge: it fetches the coordinate
    // registers before the status register, so a finger that lands between
    // the two transactions yields the previous touch point.
    int16_t rawX = 0, rawY = 0, tx = 0, ty = 0;
    bool onScreen = touch.read(rawX, rawY) && uiMapTouch(rawX, rawY, tx, ty);
#if TOUCH_DEBUG
    Serial.printf("tap raw=(%d,%d) -> screen=(%d,%d) %s\n",
                  rawX, rawY, tx, ty, onScreen ? "ok" : "rejected");
#endif
    if (onScreen && now - s_lastPetMs >= PET_REFRACTORY_MS) {
      triggerOneShot(PS_HEART);
      snap.petCount++;
      persistMarkDirty();
      wakeScreen(now, computerAway(now));
      // A short, soft chirp so petting feels rewarding without competing
      // with the louder approve/deny audio. Different pitch from anything
      // else on the device so it's instantly identifiable.
      playBeep(1568, 60); // G6
      s_lastPetMs = now;
      s_lastInteractionMs = now;
    }
  }
  s_wasTouched = touchedNow;

  // Nap-time accounting: accumulate elapsed time while face-down. We
  // commit a whole second at a time so persistMarkDirty() doesn't fire
  // on every tick of the loop.
  const unsigned long loopDtMs = s_lastLoopMs ? (now - s_lastLoopMs) : 0;
  s_lastLoopMs = now;
  bool faceDownNow = motionFaceDown();
  if (faceDownNow && s_wasFaceDown) {
    s_napAccumMs += loopDtMs;
    if (s_napAccumMs >= 1000) {
      snap.napSeconds += s_napAccumMs / 1000;
      s_napAccumMs %= 1000;
      persistMarkDirty();
    }
  } else if (!faceDownNow) {
    s_napAccumMs = 0;
  }
  s_wasFaceDown = faceDownNow;

  // 3. Battery.
  pollBatteryIfDue();

  // 4. Buttons.
  //   KEY1 — short press: approve (in ATTENTION) / wake / dismiss stats.
  //          long  press: toggle mute.
  //   KEY2 — short press: deny   (in ATTENTION) /        dismiss stats.
  //          long  press: toggle stats overlay (when not in ATTENTION).
  // Both short presses fire on RELEASE so they can be told apart from a
  // long-press that's still being held.
  static bool lastK1 = false, lastK2 = false;
  bool k1 = (digitalRead(KEY1) == LOW);
  bool k2 = (digitalRead(KEY2) == LOW);

  PersonaState st = currentState();

  // --- KEY1 long-press machine.
  if (k1 && !lastK1) {
    s_key1DownMs    = now;
    s_key1LongFired = false;
  } else if (k1 && !s_key1LongFired &&
             now - s_key1DownMs >= LONG_PRESS_MS) {
    // Long press fires while still held — only meaningful outside
    // ATTENTION (in ATTENTION the user might be holding to confirm an
    // approve press, and we don't want a surprise mute toggle).
    if (st != PS_ATTENTION) {
      snap.muted = !snap.muted;
      persistMarkDirty();
      // If we just turned sound back ON, play a small confirmation
      // chirp so it's obvious the buzzer is alive again. (Going INTO
      // mute is silent by definition — the icon in the HUD is the
      // only feedback.)
      if (!snap.muted) {
        playBeep(880, 80); // A5
      }
    }
    s_key1LongFired = true;
    s_lastInteractionMs = now;
  } else if (!k1 && lastK1) {
    // Release.
    s_lastInteractionMs = now;
    if (s_key1LongFired) {
      // Long press already consumed; swallow the release.
    } else if (!s_backlightOn) {
      wakeScreen(now, false);
    } else if (s_statsOverlay) {
      s_statsOverlay = false;
      uiClear();
    } else if (st == PS_ATTENTION && snap.hasPrompt) {
      bleSendLine(protocolBuildPermission(snap.promptId, "once"));
      snap.approveCount++;
      persistMarkDirty();
      snap.hasPrompt = false;
      triggerOneShot(PS_CELEBRATE);
      startMelody(CELEBRATE_MELODY);
    }
  }

  // --- KEY2: long-press machine. Deny fires on RELEASE so we can tell
  // it apart from a long press that toggles the stats overlay.
  if (k2 && !lastK2) {
    s_key2DownMs    = now;
    s_key2LongFired = false;
  } else if (k2 && !s_key2LongFired &&
             now - s_key2DownMs >= LONG_PRESS_MS) {
    // Long press while still held — only meaningful outside ATTENTION
    // (in ATTENTION we don't want a hold to surprise-toggle the overlay
    // when the user just hasn't released yet).
    if (st != PS_ATTENTION) {
      s_statsOverlay = !s_statsOverlay;
      if (s_statsOverlay) {
        uiRenderStats(snap, s_batPct, s_usbPlugged, persistLevel(snap));
        s_statsEnteredMs = now;
      } else {
        uiClear();
      }
    }
    s_key2LongFired = true;
    s_lastInteractionMs = now;
  } else if (!k2 && lastK2) {
    // Release.
    s_lastInteractionMs = now;
    if (s_key2LongFired) {
      // Long press already consumed; swallow the release.
    } else if (s_statsOverlay) {
      s_statsOverlay = false;
      uiClear();
    } else if (st == PS_ATTENTION && snap.hasPrompt) {
      bleSendLine(protocolBuildPermission(snap.promptId, "deny"));
      snap.denyCount++;
      persistMarkDirty();
      snap.hasPrompt = false;
      triggerOneShot(PS_CELEBRATE);
      playBeep(440, 120);
    } else {
      // No prompt pending and no stats panel: cycle the persona.
      snap.persona = (uint8_t)((snap.persona + 1) % 2);
      persistMarkDirty();
      uiSetBuddy(snap.persona ? &FACE_BUDDY : &CAT_BUDDY);
    }
  }
  lastK1 = k1;
  lastK2 = k2;

  // If a permission prompt arrives while the stats screen is up, drop
  // the overlay so the user sees the request immediately.
  if (s_statsOverlay && snap.hasPrompt) {
    s_statsOverlay = false;
    uiClear();
  }

  // 5. Re-derive state after this iteration's mutations.
  st = currentState();

  // 6. Auto-off: turn the backlight off after AUTO_OFF_MS of idle, unless
  // we have a pending prompt or USB power. Wake on shake / button / new
  // message (handled by s_lastInteractionMs updates above).
  if (st == PS_ATTENTION) s_lastInteractionMs = now;

  // A peek has its own short deadline: the eye is already shutting by the
  // time this fires, so the screen goes dark on a closed eye.
  if (s_peekOffAtMs && now >= s_peekOffAtMs) {
    s_peekUntilMs   = 0;
    s_peekOffAtMs   = 0;
    s_lastPeekEndMs = now;
    if (!s_usbPlugged && computerAway(now) && st != PS_ATTENTION) {
      uiBacklight(false);
      s_backlightOn = false;
    }
  }

  if (s_backlightOn && !s_usbPlugged && !s_peekOffAtMs &&
      now - s_lastInteractionMs >= AUTO_OFF_MS) {
    uiBacklight(false);
    s_backlightOn = false;
  } else if (!s_backlightOn && (st == PS_ATTENTION || s_usbPlugged)) {
    wakeScreen(now, false);
  }

  // 7. Persistence: coalesced periodic flush, plus an opportunistic
  // flush on BLE disconnect (when the session ends is a natural moment
  // to checkpoint stats with no user impact).
  bool connectedNow = bleIsConnected();
#if BLE_DEBUG
  if (connectedNow != s_wasConnected) {
    Serial.printf("[ble] %s at t=%lus\n",
                  connectedNow ? "CONNECTED" : "DISCONNECTED", now / 1000);
  }
  // Cumulative, so the first line printed after the host wakes up reports
  // everything that happened while serial was dead.
  static unsigned long s_bleLogMs = 0;
  static unsigned long s_maxGapMs = 0;
  const unsigned long gap = s_lastHeartbeatMs ? (now - s_lastHeartbeatMs) : 0UL;
  if (connectedNow && gap > s_maxGapMs) s_maxGapMs = gap;
  if (now - s_bleLogMs >= 5000) {
    s_bleLogMs = now;
    Serial.printf("[ble] t=%lus conn=%d hb_age=%lums maxgap=%lums "
                  "disc=%u adv=%u light=%d heap=%uKB\n",
                  now / 1000, (int)connectedNow, gap, s_maxGapMs,
                  (unsigned)bleDisconnects(), (unsigned)bleAdvKicks(),
                  (int)s_backlightOn, (unsigned)(ESP.getFreeHeap() / 1024));
  }
#endif
  if (s_wasConnected && !connectedNow) {
    persistFlush(snap);
  }
  s_wasConnected = connectedNow;
  persistTick(snap);

  // 8. Render.
  if (s_backlightOn) {
    if (s_statsOverlay) {
      // The stats panel is painted once at entry — no continuous repaint,
      // so it stays flicker-free. Just watch for the auto-dismiss timer.
      if (now - s_statsEnteredMs >= STATS_AUTO_DISMISS_MS) {
        s_statsOverlay = false;
        uiClear();
      }
    } else if (g_activeBuddy && g_activeBuddy->ownsFullScreen) {
      // A full-screen persona drives itself from the parameter vector, on
      // its own 30 fps clock.
      if (now - s_lastFaceFrameMs >= FACE_FRAME_MS) {
        s_lastFaceFrameMs = now;
        const FaceState fs = faceStateFor(st, now);
        const float mood = moodValue(snap);
        if (fs == FS_MOOD_IDLE) faceSetMood(mood);
        else                    faceSetState(fs);
        const FaceParams& fp = faceTick((uint32_t)now);
#if FACE_DEBUG
        static FaceState dbgLastFs = FS_COUNT;
        static int       dbgLastHue = -999;
        const int hue = (int)fp.bgHue;
        if (fs != dbgLastFs || abs(hue - dbgLastHue) >= 3) {
          Serial.printf("face=%d mood=%d%% today=%ld base=%.0f "
                        "eyeOpen=%.2f joy=%+.2f hue=%d\n",
                        (int)fs, (int)(mood * 100.0f), snap.tokensToday,
                        snap.tokenBaseline, fp.eyeOpen, fp.joy, hue);
          dbgLastFs = fs;
          dbgLastHue = hue;
        }
#endif
        uiRender(st, snap, s_usbPlugged, s_batPct, &fp);
#if FACE_DEBUG
        // Real cadence, since the 33 ms target competes with a 64 KB
        // polled-SPI push and the 20 ms loop delay.
        static unsigned long fpsWindowMs = 0, fpsFrames = 0, worstMs = 0;
        const unsigned long frameMs = millis() - now;
        if (frameMs > worstMs) worstMs = frameMs;
        fpsFrames++;
        if (now - fpsWindowMs >= 5000) {
          Serial.printf("fps=%lu worst_frame=%lums heap=%uKB\n",
                        fpsFrames / 5, worstMs,
                        (unsigned)(ESP.getFreeHeap() / 1024));
          fpsWindowMs = now; fpsFrames = 0; worstMs = 0;
        }
#endif
      }
    } else {
      uiRender(st, snap, s_usbPlugged, s_batPct, nullptr);
    }
  }

  updateMelody();

  // A full-screen persona's own render paces the loop — the 64 KB push is
  // polled, not DMA, so it blocks for ~30 ms on its own. Idling another
  // 20 ms on top only costs frame rate, and the spec wants 30 fps.
  const bool selfPaced = s_backlightOn && !s_statsOverlay &&
                         g_activeBuddy && g_activeBuddy->ownsFullScreen;
  delay(selfPaced ? 5 : 20);
}
