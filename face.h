#pragma once

#include "buddy.h"

// NessoFace's entire appearance is these seven numbers (spec section 6).
// Everything drawn derives from them; there is no mouth parameter, `joy`
// reads through the eye proportions and the nostrils instead.
struct FaceParams {
  float eyeOpen;   // 0..1, vertical scale of the eye; 0 = closed
  float lidTilt;   // -1 angry (inner corners down) .. +1 sad (inner up)
  float pupilX;    // -1..1 horizontal gaze; moves the whole eye
  float pupilY;    // -1..1 vertical gaze
  float joy;       // -1 miserable .. +1 delighted
  float bgHue;     // 0..360
  float bounce;    // 0..4 px of breathing amplitude
};

// The moods NessoFace can hold. Distinct from PersonaState: these are
// what the face looks like, not what the session is doing.
enum FaceState {
  FS_ASLEEP,
  FS_SAD,
  FS_HAPPY,
  FS_BUSY,
  FS_CURIOUS,
  FS_DIZZY,
  FS_HEART,
  FS_COUNT,
};

extern const Buddy FACE_BUDDY;

// Set the parameter target. Cheap and idempotent — call it every loop.
void faceSetState(FaceState st);

// The resting state: target a blend of the SAD and HAPPY rows, by mood
// (0 = miserable, 1 = delighted). Also idempotent.
void faceSetMood(float mood);

// Advance one animation frame (blink, breath, gaze, interpolation) and
// return the parameters for it. Expects to be called at ~30 fps.
const FaceParams& faceTick(uint32_t nowMs);
