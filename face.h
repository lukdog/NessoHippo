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

extern const Buddy FACE_BUDDY;

// Parameters for this frame.
const FaceParams& faceTick(uint32_t nowMs);
