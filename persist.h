#pragma once

#include "protocol.h"

// Load persisted fields into `s` and prime the dirty-tracker. Call once
// from setup() AFTER snap is constructed but before BLE is up.
void persistBegin(Snapshot& s);

// Mark the in-memory snapshot as differing from what's in flash. The
// next persistTick() may flush; persistFlush() always does.
void persistMarkDirty();

// Coalesced flush: writes to NVS at most every FLUSH_INTERVAL_MS, and
// only when dirty. Call once per main-loop iteration.
void persistTick(const Snapshot& s);

// Force-write any dirty state immediately (e.g. on disconnect).
void persistFlush(const Snapshot& s);

// Derived "level" reported in the status response. Grows sublinearly so
// the early gains feel rewarding but late progress requires real usage.
uint16_t persistLevel(const Snapshot& s);
