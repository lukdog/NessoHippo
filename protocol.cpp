#include "protocol.h"
#include "ble_bridge.h"
#include "persist.h"
#include "mood.h"

#include <ArduinoJson.h>

// Cap incoming/outgoing JSON at 4 KB to match upstream's documented
// "Events that serialize larger than 4 KB are dropped" rule. The Nesso N1
// is an ESP32-C6 with ~300 KB free heap, so a 4 KB stack-allocated
// JsonDocument is comfortable.
static const size_t JSON_CAP = 4096;

static String ack(const char* cmd, bool ok) {
  JsonDocument doc;
  doc["ack"] = cmd;
  doc["ok"]  = ok;
  String out;
  serializeJson(doc, out);
  return out;
}

void protocolHandleLine(const String& line, Snapshot& s, ProtocolFx& fx) {
  if (line.length() == 0) return;
  if (line.length() > JSON_CAP) return; // drop oversized

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) return;

  // Heartbeat snapshot: detected by the presence of "total" or "running".
  // The desktop sends these every few seconds without any "cmd" field.
  if (doc["total"].is<int>() || doc["running"].is<int>() ||
      doc["waiting"].is<int>()) {
    s.total       = doc["total"]   | 0;
    s.running     = doc["running"] | 0;
    s.waiting     = doc["waiting"] | 0;
    s.msg         = String((const char*)(doc["msg"] | ""));
    s.tokens      = doc["tokens"]       | 0L;
    s.tokensToday = doc["tokens_today"] | 0L;

    if (doc["prompt"].is<JsonObject>()) {
      JsonObject p = doc["prompt"].as<JsonObject>();
      s.hasPrompt  = true;
      s.promptId   = String((const char*)(p["id"]   | ""));
      s.promptTool = String((const char*)(p["tool"] | ""));
      s.promptHint = String((const char*)(p["hint"] | ""));
    } else {
      s.hasPrompt  = false;
      s.promptId   = "";
      s.promptTool = "";
      s.promptHint = "";
    }
    return; // no ack for heartbeats (they're unsolicited)
  }

  // time sync — no clock UI, but the mood baseline needs to know when the
  // owner's local day rolls over.
  if (doc["time"].is<JsonArray>()) {
    JsonArray t = doc["time"].as<JsonArray>();
    if (t.size() >= 1) {
      const uint32_t unixSec = t[0] | 0UL;
      const int32_t  tzOff   = (t.size() >= 2) ? (int32_t)(t[1] | 0L) : 0;
      moodOnTime(unixSec, tzOff, s);
    }
    bleSendLine(ack("time", true));
    return;
  }

  const char* cmd = doc["cmd"];
  if (!cmd) return;

  if (strcmp(cmd, "status") == 0) {
    // Status reply needs live USB/battery state, which lives in the main
    // loop. Flag it so the caller can build and send the reply.
    fx.statusRequested = true;
    return;
  }

  if (strcmp(cmd, "name") == 0) {
    const char* nm = doc["name"];
    if (nm && *nm) {
      s.buddyName = nm;
      fx.nameChanged = true;
      persistMarkDirty();
    }
    bleSendLine(ack("name", true));
    return;
  }

  if (strcmp(cmd, "owner") == 0) {
    const char* nm = doc["name"];
    if (nm) {
      s.ownerName = nm;
      fx.ownerChanged = true;
      persistMarkDirty();
    }
    bleSendLine(ack("owner", true));
    return;
  }

  if (strcmp(cmd, "unpair") == 0) {
    fx.unpaired = true;
    bleSendLine(ack("unpair", true));
    return;
  }

  // Folder-push transport: explicitly out of scope for this port. Ack
  // with ok:false so the desktop times out the transfer quickly instead
  // of waiting for chunk acks that will never come.
  if (strcmp(cmd, "char_begin") == 0 || strcmp(cmd, "file")     == 0 ||
      strcmp(cmd, "chunk")      == 0 || strcmp(cmd, "file_end") == 0 ||
      strcmp(cmd, "char_end")   == 0) {
    bleSendLine(ack(cmd, false));
    return;
  }
}

String protocolBuildStatus(const Snapshot& s, bool usbPlugged, uint16_t batPct) {
  JsonDocument doc;
  doc["ack"] = "status";
  doc["ok"]  = true;
  JsonObject data = doc["data"].to<JsonObject>();
  data["name"] = s.buddyName;
  data["sec"]  = true; // just-works pairing; report secure for picker UX
  JsonObject bat = data["bat"].to<JsonObject>();
  bat["pct"] = batPct;
  bat["mV"]  = 0;       // not exposed by NessoBattery API in 1.0.0
  bat["mA"]  = 0;
  bat["usb"] = usbPlugged;
  JsonObject sys = data["sys"].to<JsonObject>();
  sys["up"]   = (uint32_t)(millis() / 1000);
  sys["heap"] = (uint32_t)ESP.getFreeHeap();
  JsonObject stats = data["stats"].to<JsonObject>();
  stats["appr"] = s.approveCount;
  stats["deny"] = s.denyCount;
  stats["vel"]  = s.shakeCount;    // upstream's "vel" = shake/velocity count
  stats["nap"]  = s.napSeconds;
  stats["lvl"]  = persistLevel(s);
  String out;
  serializeJson(doc, out);
  return out;
}

String protocolBuildPermission(const String& id, const char* decision) {
  JsonDocument doc;
  doc["cmd"]      = "permission";
  doc["id"]       = id;
  doc["decision"] = decision;
  String out;
  serializeJson(doc, out);
  return out;
}
