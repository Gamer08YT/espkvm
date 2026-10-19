/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * See kvm_audio.h. The encoder task holds one PCM reader while it has Opus
 * readers, brings the sound to 48 kHz (Opus takes nothing in between), encodes
 * 20 ms at a time and copies each packet into every reader's queue.
 */
#include <stdlib.h>
#include <string.h>

#include "encoder/impl/esp_opus_enc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio_priv.h"
#include "kvm_audio.h"

static const char *TAG = "opus";

#define MAX_READERS 4
#define BITRATE 96000
#define COMPLEXITY 1
/* 300 ms of 5 ms blocks: the encoder sits below the video and can wait
   that long behind it. */
#define PCM_DEPTH 60
/* libopus keeps its work arrays on the stack: it used 36 KB at 96 kbit/s. */
#define TASK_STACK (48 * 1024)
#define TASK_PRIO 4

struct kvm_opus_reader {
    QueueHandle_t queue;
    StaticQueue_t queue_state;
    uint8_t *storage;
};

/* Large, so in PSRAM, owned by the task. */
typedef struct {
    kvm_audio_block_t in;
    int16_t pcm48[RESAMPLE_MAX * KVM_AUDIO_CHANNELS];
    int16_t pcm[KVM_OPUS_FRAMES * KVM_AUDIO_CHANNELS];
    kvm_opus_packet_t pkt;
    kvm_opus_packet_t old;
} work_t;

static SemaphoreHandle_t s_mu;
static TaskHandle_t s_task;
static kvm_opus_reader_t *s_readers[MAX_READERS];

static bool any_reader(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool any = false;
    for (int i = 0; i < MAX_READERS; i++) {
        any |= s_readers[i] != NULL;
    }
    xSemaphoreGive(s_mu);
    return any;
}

static void deliver(work_t *w)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < MAX_READERS; i++) {
        kvm_opus_reader_t *r = s_readers[i];
        if (!r) {
            continue;
        }
        if (xQueueSend(r->queue, &w->pkt, 0) != pdTRUE) {
            (void)xQueueReceive(r->queue, &w->old, 0);
            (void)xQueueSend(r->queue, &w->pkt, 0);
        }
    }
    xSemaphoreGive(s_mu);
}

static void encode_task(void *arg)
{
    (void)arg;
    work_t *w = heap_caps_calloc(1, sizeof(*w), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!w) {
        ESP_LOGE(TAG, "no memory");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        if (!any_reader()) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        kvm_audio_reader_t *pcm = NULL;
        if (kvm_audio_open(PCM_DEPTH, &pcm) != ESP_OK) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
        cfg.sample_rate = OUT_RATE;
        cfg.channel = KVM_AUDIO_CHANNELS;
        cfg.bitrate = BITRATE;
        cfg.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
        cfg.complexity = COMPLEXITY;
        cfg.enable_vbr = true;
        void *enc = NULL;
        if (esp_opus_enc_open(&cfg, sizeof(cfg), &enc) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "encoder did not open");
            kvm_audio_close(pcm);
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
            continue;
        }
        ESP_LOGI(TAG, "encoding at %d bit/s", BITRATE);

        resampler_t rs = {0};
        unsigned have = 0;         /* frames in w->pcm */
        int64_t first_us = 0;      /* start time of w->pcm[0] */
        uint32_t ts = 0;
        bool resync = true;
        uint64_t next_index = 0;   /* the input frame expected next */
        uint32_t epoch = 0;
        while (any_reader()) {
            if (kvm_audio_read(pcm, &w->in, 100) != ESP_OK) {
                continue; /* no sound; the next block is a gap */
            }
            const kvm_audio_block_t *b = &w->in;
            if (b->rate != rs.rate || b->epoch != epoch || b->frame_index != next_index) {
                /* Start over at a gap: what was gathered is dropped, and the
                 * timestamp moves on to where the sound is now. */
                resample_reset(&rs, b->rate);
                have = 0;
                resync = true;
                epoch = b->epoch;
            }
            next_index = b->frame_index + b->frames;
            int64_t at_us;
            const size_t n = resample_block(&rs, b, w->pcm48, &at_us);
            for (size_t k = 0; k < n; k++) {
                if (have == 0) {
                    first_us = at_us + (int64_t)k * 1000000 / OUT_RATE;
                    if (resync) {
                        ts = (uint32_t)(first_us * 48 / 1000);
                    }
                }
                memcpy(&w->pcm[have * KVM_AUDIO_CHANNELS], &w->pcm48[k * KVM_AUDIO_CHANNELS],
                       KVM_AUDIO_CHANNELS * sizeof(int16_t));
                if (++have == KVM_OPUS_FRAMES) {
                    esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)w->pcm,
                                                   .len = sizeof(w->pcm)};
                    esp_audio_enc_out_frame_t out = {.buffer = w->pkt.data, .len = KVM_OPUS_MAX};
                    if (esp_opus_enc_process(enc, &in, &out) == ESP_AUDIO_ERR_OK &&
                        out.encoded_bytes > 0 && out.encoded_bytes <= KVM_OPUS_MAX) {
                        w->pkt.len = (uint16_t)out.encoded_bytes;
                        w->pkt.start_us = first_us;
                        w->pkt.ts = ts;
                        w->pkt.resync = resync;
                        deliver(w);
                        resync = false;
                    }
                    ts += KVM_OPUS_FRAMES;
                    have = 0;
                }
            }
        }
        esp_opus_enc_close(enc);
        kvm_audio_close(pcm);
        ESP_LOGI(TAG, "encoder stopped; stack never below %u bytes free",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

esp_err_t opus_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    return s_mu ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t kvm_opus_open(uint16_t depth, kvm_opus_reader_t **out)
{
    *out = NULL;
    if (depth == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mu) {
        return ESP_ERR_INVALID_STATE;
    }
    kvm_audio_status_t st;
    kvm_audio_status(&st);
    if (!st.enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    kvm_opus_reader_t *r = heap_caps_calloc(1, sizeof(*r), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!r) {
        return ESP_ERR_NO_MEM;
    }
    r->storage = heap_caps_malloc((size_t)depth * sizeof(kvm_opus_packet_t), MALLOC_CAP_SPIRAM);
    if (!r->storage) {
        free(r);
        return ESP_ERR_NO_MEM;
    }
    r->queue = xQueueCreateStatic(depth, sizeof(kvm_opus_packet_t), r->storage, &r->queue_state);

    esp_err_t err = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < MAX_READERS; i++) {
        if (!s_readers[i]) {
            s_readers[i] = r;
            err = ESP_OK;
            break;
        }
    }
    if (err == ESP_OK && !s_task &&
        xTaskCreateWithCaps(encode_task, "opus", TASK_STACK, NULL, TASK_PRIO, &s_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        s_task = NULL;
        for (int i = 0; i < MAX_READERS; i++) {
            if (s_readers[i] == r) {
                s_readers[i] = NULL;
            }
        }
        err = ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_mu);
    if (err != ESP_OK) {
        vQueueDelete(r->queue);
        free(r->storage);
        free(r);
        return err;
    }
    xTaskNotifyGive(s_task);
    *out = r;
    return ESP_OK;
}

esp_err_t kvm_opus_read(kvm_opus_reader_t *r, kvm_opus_packet_t *pkt, uint32_t timeout_ms)
{
    TickType_t t = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0 && t == 0) {
        t = 1;
    }
    return xQueueReceive(r->queue, pkt, t) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void kvm_opus_close(kvm_opus_reader_t *r)
{
    if (!r) {
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < MAX_READERS; i++) {
        if (s_readers[i] == r) {
            s_readers[i] = NULL;
        }
    }
    xSemaphoreGive(s_mu);
    vQueueDelete(r->queue);
    free(r->storage);
    free(r);
}
