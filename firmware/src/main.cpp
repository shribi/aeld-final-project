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

#define PAYLOAD_MAX 512          // client accepts up to 1024
static uint16_t g_seq = 0;

static uint16_t crc16_update(uint16_t crc, const uint8_t *d, size_t n) {
    while (n--) {
        crc ^= (uint16_t)(*d++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}

void onAudioStream(int16_t* audioData, size_t bytesRead, size_t audioPacketCount) {
    if (bytesRead > 0) {
        // Direct write - no other operations in this hot path
        uint8_t hdr[8] = {
            0xA5, 0x5A, 0xC3, 0x3C,
            (uint8_t)(g_seq & 0xFF), // sequence number (little-endian)
            (uint8_t)(g_seq >> 8),  // sequence number (little-endian)
            (uint8_t)(bytesRead & 0xFF), // payload length (little-endian)
            (uint8_t)(bytesRead >> 8) // payload length (little-endian)
        };

        uint16_t crc = crc16_update(0xFFFF, hdr + 4, 4);
        crc = crc16_update(crc, (uint8_t*)audioData, bytesRead);
        uint8_t tail[2] = { crc & 0xFF, crc >> 8 };

        Serial2.write(hdr, sizeof(hdr));
        Serial2.write((uint8_t*)audioData, bytesRead);
        Serial2.write(tail, sizeof(tail));
        g_seq++;
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