#include <Arduino.h>
#include "constants.h"
#include "logger.h"
#include "base.h"
#include "devices.h"
#include <base64.h>

//==================================================
// Forward Declarations
//==================================================
void log(const String& message);

void onAudioStream(int16_t* audioData, size_t bytesRead, size_t audioPacketCount) {
    if (bytesRead > 0) {
        // Direct write - no other operations in this hot path
        Serial2.write((uint8_t*)audioData, bytesRead);
    }
}

void setup(){
    Serial.begin(115200);
    Serial2.begin(921600); // Initialize Serial2 for RX2 pin (GPIO 17)
    createLogTask();

    registerAudioStreamCallback(onAudioStream);
    createI2STask(); // Start the I2S task to handle audio streaming
    startRecordingI2S(); // Start the I2S recording task
}

void loop(){
    delay(5000);
}