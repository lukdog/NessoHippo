#pragma once

#include <Arduino.h>

// Latest heartbeat snapshot from the desktop, plus a few derived flags.
struct Snapshot {
  // From heartbeat:
  int      total       = 0;
  int      running     = 0;
  int      waiting     = 0;
  String   msg;                       // short status line, e.g. "approve: Bash"
  long     tokens      = 0;
  long     tokensToday = 0;
  // Pending permission prompt, if any:
  bool     hasPrompt   = false;
  String   promptId;
  String   promptTool;
  String   promptHint;
  // Identity / config:
  String   buddyName   = "Clawd";
  String   ownerName;
  bool     muted       = false;       // KEY1 long-press toggles; persisted
  // Mood baseline — the owner's own recent daily token average, and the
  // day tracking that advances it. Persisted; 0 baseline means unseeded.
  float    tokenBaseline   = 0.0f;
  uint32_t lastDayIndex    = 0;
  long     tokensYesterday = 0;
  // Local stats — loaded from NVS at boot, written back coalesced.
  uint32_t approveCount = 0;
  uint32_t denyCount    = 0;
  uint32_t shakeCount   = 0;   // total detected shake gestures
  uint32_t napSeconds   = 0;   // total seconds spent face-down (SLEEP)
  uint32_t petCount     = 0;   // total touchscreen "pets"
};

// Side effects requested by handleIncoming() so the main loop can run them
// without the protocol module having to touch the display or BLE directly.
struct ProtocolFx {
  bool ownerChanged    = false;       // → HEART one-shot
  bool nameChanged     = false;
  bool unpaired        = false;       // → SLEEP + drop connection
  bool statusRequested = false;       // → main loop sends a status reply
};

void protocolHandleLine(const String& line, Snapshot& s, ProtocolFx& fx);
String protocolBuildStatus(const Snapshot& s, bool usbPlugged, uint16_t batPct);
String protocolBuildPermission(const String& id, const char* decision);
