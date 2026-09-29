#pragma once
#include <Arduino.h>

struct LogMessage {
    char message[128];
    unsigned long timestamp;
};