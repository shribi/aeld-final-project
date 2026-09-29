#pragma once 

void initI2S();

void createI2STask();

void registerAudioStreamCallback(void (*onAudioStream)(int16_t*, size_t, size_t));

void stopRecordingI2S();
void startRecordingI2S();