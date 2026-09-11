#pragma once

#include <Arduino.h>

void motionBegin();

// Reads the BMI270, updates internal EMAs, and updates the flags below.
// Call once per loop iteration.
void motionTick();

// Edge-triggered: returns true once per detected shake gesture. Call once
// per loop iteration; consuming the event clears it.
bool motionConsumeShake();

// Level-triggered: true while the device has been face-down for >=1 s.
bool motionFaceDown();
