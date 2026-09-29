#include <driver/i2s.h>
#include <cstring>
#include <esp_heap_caps.h>

#include "devices.h"

#include "constants.h"
#include "logger.h"


static void(*onAudioStreamCallback)(int16_t*, size_t, size_t) = nullptr;

static TaskHandle_t i2sTaskHandle = NULL;

static bool recordingI2S = false;

static int audioPacketCount = 0;

static constexpr size_t kAudioSampleRate = 16000;
static constexpr size_t kAudioMaxCaptureSeconds = 12;
static constexpr size_t kAudioMinCaptureSeconds = 2;
static constexpr size_t kAudioReserveBytes = 64 * 1024;
static constexpr size_t kAudioFallbackCaptureSeconds = 2;
static constexpr size_t kAudioDispatchChunkSamples = 512;
static size_t audioRingBufferCapacitySamples = 0;
static int16_t* audioRingBuffer = nullptr;
static size_t audioRingWriteIndex = 0;
static size_t audioRingReadIndex = 0;
static size_t audioRingFilledSamples = 0;
static portMUX_TYPE audioRingMutex = portMUX_INITIALIZER_UNLOCKED;

static TaskHandle_t audioDispatchTaskHandle = NULL;
static int16_t audioBuffer[512];

static void resetAudioRingBuffer()
{
    taskENTER_CRITICAL(&audioRingMutex);
    audioRingWriteIndex = 0;
    audioRingReadIndex = 0;
    audioRingFilledSamples = 0;
    taskEXIT_CRITICAL(&audioRingMutex);
}

static bool initAudioRingBuffer()
{
    if (audioRingBuffer != nullptr)
    {
        return true;
    }

    size_t maxBytes = kAudioMaxCaptureSeconds * kAudioSampleRate * sizeof(int16_t);
    size_t minBytes = kAudioMinCaptureSeconds * kAudioSampleRate * sizeof(int16_t);
    size_t fallbackBytes = kAudioFallbackCaptureSeconds * kAudioSampleRate * sizeof(int16_t);
    size_t targetBytes = maxBytes;
    uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    size_t freePsramBytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t freeInternalBytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (freePsramBytes < maxBytes + kAudioReserveBytes && freeInternalBytes < maxBytes + kAudioReserveBytes)
    {
        targetBytes = fallbackBytes;
        caps = MALLOC_CAP_DEFAULT;
    }
    else if (freePsramBytes < maxBytes + kAudioReserveBytes)
    {
        targetBytes = fallbackBytes;
        caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    }

    if (targetBytes < minBytes)
    {
        targetBytes = minBytes;
    }

    audioRingBufferCapacitySamples = targetBytes / sizeof(int16_t);
    audioRingBuffer = static_cast<int16_t*>(heap_caps_malloc(targetBytes, caps));

    if (audioRingBuffer == nullptr)
    {
        audioRingBuffer = static_cast<int16_t*>(heap_caps_malloc(targetBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }

    if (audioRingBuffer == nullptr)
    {
        audioRingBuffer = static_cast<int16_t*>(heap_caps_malloc(targetBytes, MALLOC_CAP_DEFAULT));
    }

    if (audioRingBuffer == nullptr)
    {
        log("Audio ring buffer allocation failed");
        return false;
    }

    resetAudioRingBuffer();
    Serial.printf("Audio ring buffer allocated: %lu bytes (%lu samples)\n",
        static_cast<unsigned long>(targetBytes),
        static_cast<unsigned long>(audioRingBufferCapacitySamples));
    return true;
}

void initI2S()
{
    i2s_config_t i2s_config =
    {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config =
    {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD
    };

    i2s_driver_install(
        I2S_PORT,
        &i2s_config,
        0,
        NULL);

    i2s_set_pin(
        I2S_PORT,
        &pin_config);

    i2s_zero_dma_buffer(I2S_PORT);
}


static void writeAudioRingBuffer(const int16_t* data, size_t sampleCount)
{
    if (audioRingBuffer == nullptr)
    {
        log("Audio ring buffer not initialized");
        return;
    }

    taskENTER_CRITICAL(&audioRingMutex);
    for (size_t i = 0; i < sampleCount; ++i)
    {
        audioRingBuffer[audioRingWriteIndex] = data[i];
        audioRingWriteIndex = (audioRingWriteIndex + 1) % audioRingBufferCapacitySamples;

        if (audioRingFilledSamples < audioRingBufferCapacitySamples)
        {
            audioRingFilledSamples++;
        }
        else
        {
            audioRingReadIndex = (audioRingReadIndex + 1) % audioRingBufferCapacitySamples;
        }
    }
    taskEXIT_CRITICAL(&audioRingMutex);
}

static size_t readAudioRingBuffer(int16_t* out, size_t maxSamples)
{
    if (audioRingBuffer == nullptr)
    {
        return 0;
    }

    size_t availableSamples = 0;

    taskENTER_CRITICAL(&audioRingMutex);
    availableSamples = (audioRingFilledSamples < maxSamples) ? audioRingFilledSamples : maxSamples;
    for (size_t i = 0; i < availableSamples; ++i)
    {
        out[i] = audioRingBuffer[audioRingReadIndex];
        audioRingReadIndex = (audioRingReadIndex + 1) % audioRingBufferCapacitySamples;
    }
    audioRingFilledSamples -= availableSamples;
    taskEXIT_CRITICAL(&audioRingMutex);

    return availableSamples;
}

void audioDispatchTask(void *pvParameters)
{
    while (true)
    {
        int16_t chunk[kAudioDispatchChunkSamples];
        size_t sampleCount = readAudioRingBuffer(chunk, kAudioDispatchChunkSamples);

        if (sampleCount > 0)
        {
            if (onAudioStreamCallback)
            {
                onAudioStreamCallback(chunk, sampleCount * sizeof(int16_t), audioPacketCount);
            }
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

void registerAudioStreamCallback(void (*onAudioStream)(int16_t*, size_t, size_t))
{
    onAudioStreamCallback = onAudioStream;
    if (audioDispatchTaskHandle == NULL)
    {
        xTaskCreatePinnedToCore(
            audioDispatchTask,
            "AudioDispatchTask",
            6144,
            NULL,
            1,
            &audioDispatchTaskHandle,
            0);
    }
}

void recordingTask(void *pvParameters)
{
    size_t bytesRead;
    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (recordingI2S)
        {
            uint32_t t1 = micros();
            i2s_read(
                I2S_PORT,
                audioBuffer,
                sizeof(audioBuffer),
                &bytesRead,
                portMAX_DELAY);

            audioPacketCount++;
            if (bytesRead > 0)
            {
                size_t sampleCount = bytesRead / sizeof(int16_t);
                writeAudioRingBuffer(audioBuffer, sampleCount);
            }
        }
    }
}

void startRecordingI2S()
{
    recordingI2S = true;
    xTaskNotifyGive(i2sTaskHandle);
}

void stopRecordingI2S()
{
    recordingI2S = false;
    i2s_zero_dma_buffer(I2S_PORT);
    audioPacketCount = 0;
    resetAudioRingBuffer();
    audioBuffer[0] = 0; // Clear the audio buffer
}

void createI2STask()
{
    initI2S();
    initAudioRingBuffer();
    xTaskCreatePinnedToCore(
        recordingTask,
        "I2STask",
        4096,
        NULL,
        2,
        &i2sTaskHandle,
        1);
}

