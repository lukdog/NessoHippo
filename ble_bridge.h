#pragma once

#include <Arduino.h>

void bleBegin(const char* namePrefix);
bool bleIsConnected();
bool blePopLine(String& out);
void bleSendLine(const String& s);

// Call once per main-loop iteration. Keeps the advertising alive whenever
// there is no real connection, and repairs the connection flag if a
// disconnect callback was never delivered.
void bleTick();
