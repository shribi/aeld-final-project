#pragma once
#include <Arduino.h>
#include <freertos/queue.h>
#include "base.h"
#include "constants.h"


void log(const String& message);
void createLogTask();
void getLogs(LogMessage* outLogs, size_t& outCount);
void logTask();