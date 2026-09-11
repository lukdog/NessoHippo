# NessoFace — implementation brief

Feature spec for a new persona in `lukdog/NessoBuddy`. Read the existing
README and source before writing code. Ask before changing anything marked
**do not touch**.

## 1. Goal

Add a second buddy persona, `NessoFace`: a **full-screen animated face**
that fills the whole 240x135 landscape display. Two large white squircle
eyes on a flat colored background, plus two nostrils. No mouth, no ears,
no outline of a head.

The design hints at a hippo without ever drawing one: only the wide-set
eyes and the two nostrils low on the screen. Keep it that way — the
restraint is the design.

Its mood is driven by how many Claude tokens the owner has used today,
compared to the owner's own recent daily average.

This is **not** a port of the upstream ASCII pets and not a GIF renderer.
The face is drawn every frame from a small set of numeric parameters.

## 2. Hard visual rules

These are decided. Do not "improve" them.

- **Flat only.** No gradients anywhere — not in the background, not in the
  eyes, not in the shadows. Every fill is one solid color.
- **No blur.** The drop shadow under each eye is a hard-edged copy of the
  eye shape offset downward. The eye highlight is a small square of the
  *background* color cut into the eye, not a light sheen.
- **Three colors on screen**, all derived from one hue: background, dark
  accent, white. Nothing else.
- **No text, no HUD, no battery icon** on the face screen. Battery and
  stats stay in the existing stats panel (KEY2 long-press), unchanged.

## 3. Scope and constraints

**Do not touch:**
- `ble_bridge.h/.cpp` — the BLE transport works, leave it alone.
- The existing `cat.cpp` persona and its behavior.
- The existing permission/approval path (KEY1/KEY2 in ATTENTION, the
  `{"cmd":"permission",...}` emit). It stays functional. NessoFace simply
  does **not** draw an approval overlay — see section 8.
- Folder-push (`char_*`) handling: still acked with `ok:false`.

**Keep:** `arduino-cli` build, board `esp32:esp32:arduino_nesso_n1`,
ArduinoJson 7, `Arduino_Nesso_N1` / M5GFX (LovyanGFX), NVS via
`Preferences` namespace `NessoBuddy`.

**Persona selection:** add a persisted setting `persona` (0 = cat,
1 = face), default 1. Cycle it with KEY2 short-press when no prompt is
pending and the stats panel is closed. Persist to NVS like the other
settings.

## 4. Extending the `Buddy` interface

Today `Buddy` is a struct of two function pointers (`frameIndex`,
`drawFrame`) built for frame-based ASCII art. That model does not fit a
parametric face, which needs continuous values and owns the background.

Extend the struct, do not replace it:

```cpp
struct Buddy {
  BuddyFrameIndexFn frameIndex;   // may be nullptr for full-screen buddies
  BuddyDrawFrameFn  drawFrame;    // may be nullptr for full-screen buddies
  BuddyDrawFullFn   drawFull;     // new, optional
  bool ownsFullScreen;            // new, default false
};
```

`drawFull(LGFX_Sprite& spr, const FaceParams& p, uint32_t nowMs)` gets the
whole sprite and is responsible for clearing it.

In `ui.cpp`: when the active buddy has `ownsFullScreen == true`, skip the
background fill, the ASCII draw and the HUD, and call `drawFull` instead.
Everything else in `ui.cpp` keeps its current behavior for the cat.

## 5. Screen orientation

The cat runs portrait (135x240). NessoFace runs **landscape 240x135**.

- Set the display rotation when the persona is activated, restore it when
  switching back. Do not assume the rotation is constant anywhere in `ui.cpp`.
- **Gotcha:** `NessoTouch` (FT6x36) reports raw panel coordinates. After
  rotating you must transform touch points yourself, or petting will
  register in the wrong place. Add a small helper and verify by logging tap
  coordinates to serial.
- The sprite is 240 x 135 x 16 bpp = ~63 KB. That fits (RAM use today is
  ~7%), but allocate it once at persona activation and free it on switch —
  never per frame.

## 6. The face parameter vector

The face is fully described by seven floats. Everything drawn derives from
these.

| Param | Range | Meaning |
|---|---|---|
| `eyeOpen` | 0.0–1.0 | vertical scale of the eye; 0 = closed |
| `lidTilt` | −1.0–1.0 | −1 angry (inner corners down), +1 sad (inner corners up) |
| `pupilX` | −1.0–1.0 | horizontal gaze offset (moves the whole eye) |
| `pupilY` | −1.0–1.0 | vertical gaze offset |
| `joy` | −1.0–1.0 | −1 miserable, +1 delighted; drives eye squash and nostril flare |
| `bgHue` | 0–360 | background hue |
| `bounce` | 0.0–4.0 | vertical breathing amplitude in px |

There is no mouth parameter. `joy` replaces it: with no mouth to bend, the
happiness reads through the eye proportions (wide and squat = happy, tall
and narrow = sad) and the nostrils.

### State table (target values)

| State | eyeOpen | lidTilt | joy | bgHue | bounce |
|---|---|---|---|---|---|
| `ASLEEP` | 0.05 | +0.3 | 0.0 | 228 | 1.0 |
| `SAD` | 0.70 | +0.8 | −0.8 | 210 | 0.0 |
| `HAPPY` | 0.85 | −0.2 | +0.9 | 88 | 3.0 |
| `BUSY` | 0.60 | −0.5 | +0.2 | 34 | 2.0 |
| `CURIOUS` | 1.00 | 0.0 | +0.4 | 168 | 2.0 |
| `DIZZY` | 1.00 | 0.0 | 0.0 | 295 | 0.0 |
| `HEART` | 0.60 | −0.3 | +1.0 | 330 | 3.0 |

### Interpolation

Keep a `current` and a `target` parameter set. Every frame:
`current += (target - current) * 0.05` at 30 fps (~1 second transition).
Interpolate `bgHue` the short way around the hue wheel.

Never snap parameters, with two exceptions: blinking writes `eyeOpen`
directly, and gaze uses a fast lerp (section 9).

## 7. Layout geometry (240x135 landscape)

### Palette

Three flat colors from `bgHue`, in HSV:

```
bg    = HSV(bgHue,      0.78, 0.95)
dark  = HSV(bgHue - 8,  0.88, 0.40)
white = #FFFFFF
```

Write an HSV→RGB565 helper; there is no gradient code anywhere.

### Draw order

1. Fill the whole sprite with `bg`.

2. **Eyes.** Compute once per frame:

   ```
   squash = lerp(1.14, 0.86, (joy + 1) / 2)
   h = max(5, 56 * eyeOpen * blink * squash)
   w = (50 / squash) * (1 + 0.12 * (1 - blink))
   ```

   `squash` is what makes joy readable without a mouth: a happy eye is
   wide and short, a sad eye is narrow and tall.

   For each eye — left at `x = 74` with `dir = +1`, right at `x = 166`
   with `dir = -1`:

   - center at `(x + pupilX * 7, 62 + bounceOffset + pupilY * 6)`
   - rotate by `dir * lidTilt * 0.20` radians
   - draw a rounded rect `w x h`, corner radius 17, filled `dark`,
     offset **+4 px in y** — this is the flat drop shadow
   - draw the same rounded rect at offset 0, filled `white`
   - if `blink > 0.75` and `h > 20`, draw an 11x11 rounded square, corner
     radius 5, filled `bg`, centered at `(w * 0.23, -h * 0.25)` in the
     rotated frame — this is the highlight, a cut-out of the background

   Clamp the corner radius to `min(17, h/2, w/2)` so a nearly closed eye
   stays a capsule and does not glitch.

3. **Nostrils.** Two rounded rects, corner radius 5, filled `dark`,
   height 15, centered at `(120 ± 17, 116 + bounceOffset)`.

   Width is `9 + flare`, where:

   ```
   flare = lerp(0, 3, (joy + 1) / 2)
   ```

   In `BUSY` only, add `1.2 * sin(now * 2π / 300ms)` to the width so the
   nostrils quiver while Claude is working. Nothing else animates them.

Then push the sprite once.

## 8. What NessoFace does not draw

No mouth. No ears. No head outline. No approval overlay — when a prompt is
pending the face just goes to `CURIOUS` and the existing chirp still
plays; the owner approves in the desktop app or with KEY1 as today.

## 9. Idle life

Without these the face looks dead even when "happy":

- **Blink:** every 2.6–6 s (random), drive `eyeOpen` to 0 and back over
  ~150 ms. 12% of the time do a double blink over ~300 ms. Note the blink
  also widens the eye by 12% (the `(1 + 0.12 * (1 - blink))` term above) —
  this squash-and-stretch is what makes it read as modern rather than as a
  fade.
- **Breathing:** `bounceOffset = bounce * sin(t * 2π / 3000ms)`.
- **Gaze:** every 1.9–4.3 s pick a new target for `pupilX` in ±0.45 and
  `pupilY` in ±0.35, then approach it with a **fast** lerp of 0.3 per
  frame. This is deliberate: a snappy saccade looks alive, a slow glide
  looks sleepy. In `BUSY`, retarget every 0.8–3.2 s instead.

Target 30 fps. If the full-sprite push over SPI cannot sustain it, drop to
20 fps rather than doing partial redraws — a partial redraw of a
full-screen face is not worth the complexity.

## 10. Mood from tokens — new module `mood.h/.cpp`

This is the primary feature. It converts the heartbeat into a mood value.

```
mood = clamp(tokens_today / baseline, 0.0, 1.5) / 1.5   // 0.0 .. 1.0
```

`baseline` is an exponential moving average of the owner's previous daily
totals, persisted in NVS:

- On local-midnight rollover: `baseline = 0.8 * baseline + 0.2 * tokens_yesterday`.
- Seed `baseline` to 50000 on first run, so day one is not meaningless.
- Store `baseline`, `lastDayIndex` and `tokensYesterday` in the existing
  `persist` module, written with the same coalesced 30 s flush policy.

**Local midnight** requires implementing `{"time":[unix, tzOffsetSec]}`,
which is currently a no-op. Store both values, compute
`dayIndex = (unix + tzOffsetSec) / 86400`, and trigger the rollover when
`dayIndex` changes. Keep acking the message as today.

Then map `mood` onto the face by interpolating between the `SAD` row and
the `HAPPY` row of the state table:
`target = lerp(SAD, HAPPY, mood)`.

Rationale to preserve if you refactor: the mood is relative to the owner's
own average, not to a fixed threshold. Do not replace it with a hardcoded
token target.

## 11. State machine

Priority order, highest first:

1. `ASLEEP` — no heartbeat for >30 s, or face-down, or backlight off.
2. `DIZZY` — shake one-shot (existing `motion.cpp` detector), ~2 s.
3. `HEART` — screen petted, or `{"cmd":"owner"}` received, ~2 s.
4. `CURIOUS` — `waiting > 0` (a prompt is pending).
5. `BUSY` — `running > 0`.
6. **mood-driven idle** — interpolated between `SAD` and `HAPPY` per
   section 10. This is the default resting state.

One-shots (`DIZZY`, `HEART`) return to whatever the priority resolves to
afterwards. Reuse the existing `ONE_SHOT_MS` constant.

"Owner is at the desk" is approximated by the BLE heartbeat: no snapshot
for 30 s means the desktop is asleep or out of range, so the face falls
asleep. When heartbeats resume after >2 minutes of silence, play a short
`CURIOUS` one-shot ("oh, you're back") before settling.

## 12. Tuning knobs

Follow the repo convention: constants at the top of their file.

- `face.cpp` — `LERP_RATE` (0.05), `GAZE_LERP` (0.3), `BLINK_MIN_MS`,
  `BLINK_MAX_MS`, `BREATH_MS`, the state table, and the geometry constants
  of section 7 (eye centers, 56/50 base size, radius 17, shadow offset 4,
  nostril x ±17 / y 116).
- `mood.cpp` — `BASELINE_ALPHA` (0.2), `BASELINE_SEED` (50000),
  `MOOD_CAP` (1.5).

## 13. Suggested commits

Do these as separate commits, and stop after each so the owner can flash
and look at it:

1. Extend the `Buddy` struct with `drawFull` / `ownsFullScreen` and route
   `ui.cpp` accordingly. Cat still works, no visual change.
2. `face.cpp` with the static geometry of section 7 and a hardcoded
   `HAPPY` parameter set, landscape rotation, touch coordinate fix. No
   animation yet.
3. Parameter interpolation + the state table + idle life (blink, breath,
   gaze). Wire KEY2 short-press to cycle states for visual tuning during
   this commit only, then remove it.
4. `mood.h/.cpp`, the `time` message, NVS baseline persistence.
5. The full state machine of section 11, persona switching, NVS persona
   setting.

## 14. Acceptance test

Without the Claude desktop app, using nRF Connect (the recipe already in
the README):

1. Connect to `Claude-XXXXXX`, subscribe to TX.
2. Write `{"tokens_today":10000}` with a fresh baseline of 50000 → the eyes
   should become tall and narrow, the background blue, the nostrils
   narrow.
3. Write `{"tokens_today":80000}` → over about a second the eyes widen and
   flatten, the background turns green, the nostrils widen. Smooth, no
   flicker, no snap.
4. Write `{"running":1}` → BUSY: amber, nostrils quivering, faster gaze.
5. Write `{"running":0}` → back to the mood-driven state.
6. Stop writing for 30 s → ASLEEP.
7. Write anything → wakes, brief CURIOUS, then settles.
8. Tap the screen → HEART, and confirm the tap registers in the correct
   place in landscape.
9. Shake → DIZZY.
10. Send `{"cmd":"status"}` → the status ack is unchanged from today.

Visual check at arm's length: no gradient anywhere, no soft edges, and the
only shapes on screen are two eyes and two nostrils.

Also confirm: `arduino-cli compile` clean with no new warnings, and RAM
usage still leaves headroom after the 63 KB sprite.
