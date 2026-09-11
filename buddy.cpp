#include "buddy.h"

// NessoFace is the default persona (spec section 3); persist.cpp overrides
// this at boot once the setting exists. Assign only via uiSetBuddy().
const Buddy* g_activeBuddy = &FACE_BUDDY;
