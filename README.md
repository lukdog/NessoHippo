# NessoHippo

[NessoBuddy](https://github.com/lukdog/NessoBuddy) plus a second persona:
**NessoFace**, a full-screen animated face whose mood tracks how many Claude
tokens you have used today against your own recent daily average.

The sketch is a fork of NessoBuddy (at `3969af1`) rather than a branch of it,
so the cat, the BLE transport and the permission flow are unchanged and the
NessoBuddy submodule stays untouched.

## What NessoFace is

Two large white squircle eyes and two nostrils on a flat colored background,
filling the whole 240x135 landscape screen. No mouth, no ears, no head
outline — the hippo is implied by the wide-set eyes and the low nostrils and
never drawn. The restraint is the design.

Everything on screen comes from seven numbers, redrawn every frame:

| Param | Range | Meaning |
|---|---|---|
| `eyeOpen` | 0–1 | vertical scale of the eye |
| `lidTilt` | −1…1 | −1 angry (inner corners down), +1 sad (up) |
| `pupilX` / `pupilY` | −1…1 | gaze; moves the whole eye |
| `joy` | −1…1 | eye squash and nostril flare |
| `bgHue` | 0–360 | background hue |
| `bounce` | 0–4 | breathing amplitude, px |

There is no mouth parameter. With nothing to bend, happiness reads through the
eye proportions — wide and squat is happy, tall and narrow is sad — and
through the nostrils.

Three flat colors, all from one hue: `bg = HSV(h, .78, .95)`,
`dark = HSV(h-8, .88, .40)`, white. No gradients, no blur, no antialiasing
anywhere: the drop shadow is a hard copy of the eye offset 4 px straight down,
and the highlight is a square of the *background* cut into the eye.

## Mood from tokens

```
mood = clamp(tokens_today / baseline, 0, 1.5) / 1.5
```

`baseline` is an exponential moving average of your own previous daily totals
(`0.8 * baseline + 0.2 * yesterday`, rolled over at your local midnight),
seeded to 50 000 on first run and persisted in NVS. The resting face is a blend
between the SAD and HAPPY rows by that value.

The point is that it is relative to *your* average, not to a fixed target — a
heavy day is whatever is heavy for you.

Local midnight needs `{"time":[unix, tzOffsetSec]}`, which NessoBuddy acked and
ignored; here both values are kept and a change in
`(unix + tzOffset) / 86400` triggers the rollover.

## States

Priority order, highest first:

| State | When |
|---|---|
| `ASLEEP` | no heartbeat for 30 s, face-down, or backlight off |
| `DIZZY` | shake, ~2 s |
| `HEART` | screen petted, or `{"cmd":"owner"}`, ~2 s |
| `CURIOUS` | a prompt is pending, or a heartbeat just resumed after a nap |
| `BUSY` | `running > 0` — the nostrils quiver |
| mood-driven | the resting default |
| `PEEK` | touched or nudged while the computer is away — see below |

NessoFace draws no approval overlay: a pending prompt just makes it curious,
the chirp still plays, and KEY1/KEY2 approve and deny exactly as before.

## Sleeping and waking

The backlight still goes dark after 30 s of idle on battery, but it is no
longer a one-way trip. NessoBuddy could only switch it back on for a pending
prompt or for USB power, and with the desk computer asleep neither can happen
— so the device looked dead until you pressed a button. Now:

- **a resumed heartbeat** wakes it properly (the computer is being used again),
- **a touch or a nudge** wakes it, and
- if the computer is *away*, that wake is deliberately half-hearted: the face
  cracks **one eye** open, looks around for a couple of seconds, then shuts it
  and goes back to sleep. Someone knocked the desk; that does not deserve 30 s
  of backlight.

The nudge threshold (`NUDGE_THRESHOLD_G`, 0.18 g) sits well below the shake
threshold so picking the device up registers but typing on the same desk does
not, and `PEEK_COOLDOWN_MS` stops a busy desk from making it peek on a loop.

Note that with USB connected the backlight never turns off at all, so the peek
can only be observed on battery.

## Idle life

A blink every 2.6–6 s (12% of them doubles) that also widens the eye by 12%, a
3 s breathing bob, and a gaze that retargets every 1.9–4.3 s (0.8–3.2 s while
busy) and gets there with a deliberately snappy lerp — a fast saccade reads as
alive, a slow glide reads as sleepy.

Measured ~27 fps on the real panel. The 64 KB sprite push is polled SPI, not
DMA, and blocks for ~26 ms on its own, so the interpolation is scaled by
measured frame time rather than assuming 30 fps.

## Controls

As NessoBuddy, with one addition:

| Gesture | Effect |
|---|---|
| KEY2 short | Cycle persona (cat / face), when nothing is pending and stats are closed |

The persona is persisted (`persona`, default 1 = face). The stats panel is
portrait-only and borrows the rotation while it is up.

## Build & flash

```sh
arduino-cli compile -b esp32:esp32:arduino_nesso_n1 NessoHippo
arduino-cli upload  -b esp32:esp32:arduino_nesso_n1 -p /dev/cu.usbmodem1101 NessoHippo
```

Footprint: 28% flash, 7% static RAM, plus a 63 KB sprite on the heap
(allocated on first activation of a persona and then kept — freeing and
reclaiming it on every switch would fragment a heap shared with NimBLE).

## Tuning knobs

- `face.cpp` — `LERP_RATE`, `GAZE_LERP`, `BLINK_*`, `BREATH_MS`, `GAZE_*`,
  `NOSE_QUIVER_*`, the state table, and the section 7 geometry constants.
- `mood.cpp` — `BASELINE_ALPHA`, `BASELINE_SEED`, `MOOD_CAP`.
- `NessoHippo.ino` — `FACE_FRAME_MS`, `HEARTBEAT_STALE_MS`, `WELCOME_BACK_MS`,
  plus everything NessoBuddy already had.
- `TOUCH_DEBUG` / `FACE_DEBUG` in `NessoHippo.ino` log tap coordinates and the
  resolved state to serial.

## Implementation notes

- **Rotation and touch.** The face runs `setRotation(1)` (240x135); the cat
  stays portrait. `ui.cpp` owns the rotation and both sprites, so nothing else
  may assume either. `NessoTouch` reports raw portrait panel coordinates and
  knows nothing about rotation, so every tap goes through `uiMapTouch()`.
- **No antialiasing.** `fillSmoothRoundRect()` blends its edges, so the eyes
  use a small rotated-rounded-rect rasterizer instead: integer Q16 with
  incremental per-column stepping, because the ESP32-C6 has no FPU, filling one
  span per row since the shape is convex.
