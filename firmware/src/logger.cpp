#include "logger.h"

#include "base.h"
#include "constants.h"

static QueueHandle_t logQueue;

static LogMessage logs[LOG_SIZE];
static size_t headIndex = 0;
static size_t counter = 0;

void createLogTask() {
    logQueue = xQueueCreate(10, sizeof(LogMessage));
    xTaskCreatePinnedToCore(
        [](void*){ logTask(); },
        "LogTask",
        4096,
        nullptr,
        1,
        nullptr,
        0);
}

void getLogs(LogMessage* outLogs, size_t& outCount) {
    size_t count = counter;
    for (size_t i = 0; i < count; ++i) {
        size_t index = (headIndex + LOG_SIZE - count + i) % LOG_SIZE;
        outLogs[i] = logs[index];
    }
    outCount = count;
}

void log(const String& message) {
    Serial.println(message);
    if (logQueue == NULL) {
        return;
    }

    LogMessage log;
    strncpy(log.message, message.c_str(), sizeof(log.message) - 1);
    log.timestamp = millis();
    xQueueSend(logQueue, &log, pdMS_TO_TICKS(10));
}

void logTask() {
    while (true) {
        LogMessage log;
        if (xQueueReceive(logQueue, &log, portMAX_DELAY)) {
            logs[headIndex].timestamp = log.timestamp;
            strncpy(logs[headIndex].message, log.message, sizeof(logs[headIndex].message));
            headIndex = (headIndex + 1) % LOG_SIZE;

            if (counter < LOG_SIZE)
                counter++;
        }
        // Delay to prevent task from consuming too much CPU
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}