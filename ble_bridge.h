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

// Diagnostics that survive the host sleeping, since serial does not:
// disconnects seen, and how many times bleTick() had to re-assert
// advertising. Both count from boot.
uint32_t bleDisconnects();
uint32_t bleAdvKicks();
uint32_t bleForcedDrops();

// Hang up on a peer that holds the link open but has stopped talking.
// No-op when not connected.
void bleDropLink();
