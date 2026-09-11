#pragma once

#include <Arduino.h>

void bleBegin(const char* namePrefix);
bool bleIsConnected();
bool blePopLine(String& out);
void bleSendLine(const String& s);
