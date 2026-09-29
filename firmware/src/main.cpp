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
        // log the bytes read and audio packet count
        String logMessage = "Audio Stream: Bytes Read = " + String(bytesRead) +
                            ", Audio Packet Count = " + String(audioPacketCount) +
                            "\n Audio Data (Base64): \n" + base64::encode((const uint8_t*)audioData, bytesRead);
        log(logMessage);
    }
}

void setup(){
    Serial.begin(115200);
    createLogTask();

    registerAudioStreamCallback(onAudioStream);
    createI2STask(); // Start the I2S task to handle audio streaming
    startRecordingI2S(); // Start the I2S recording task
}

void loop(){
    delay(5000);
}