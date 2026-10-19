/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * See kvm_audio.h. The task keeps a count of the 48 kHz frames it has put out
 * since it opened, which is also a clock: frame n is due n / 48000 s after the
 * start. Each block of sound goes where its capture time says. A little early
 * or late is taken as it comes; further off, the stream is filled with silence
 * up to it or the overlap is cut. With no sound at all, silence keeps the
 * count up with the time; a whole frame of it is a silent frame made once at
 * the start, not encoded again, so catching up after a stall costs next to
 * nothing. The frames go out through a ring buffer in PSRAM, so a recorder
 * stuck on a slow card loses nothing for a few seconds, and the dashcam keeps
 * its minutes of past there.
 *
 * Mono: the two channels are mixed. A recording runs next to the Opus encoder
 * for the console, and in stereo the two together nearly filled both cores.
 */
#include <stdlib.h>
#include <string.h>

#include "encoder/impl/esp_aac_enc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio_priv.h"
#include "kvm_audio.h"

static const char *TAG = "aac";

#define BITRATE 64000
#define CHANNELS 1
/* 500 ms of 5 ms blocks: the task sits below the video, like Opus. */
#define PCM_DEPTH 100
#define TASK_STACK (40 * 1024)
#define TASK_PRIO 4
/* At least this much is kept: about 16 s at 64 kbit/s. */
#define OUT_BYTES (128 * 1024)
/* Room a second of frames takes in the ring: the bitrate with a margin for
 * the louder parts, and each item's header. */
#define BYTES_PER_S (BITRATE / 8 * 5 / 4 + 1024)
/* Silence is made up only this far behind now: sound may still be on its way. */
#define LAG_US 400000
/* 20 ms: closer than this a block is taken where it falls. */
#define SLACK (KVM_AAC_RATE / 50)

typedef struct {
    int64_t start_us;
    uint8_t data[];
} item_t;

struct kvm_aac {
    RingbufHandle_t out;
    SemaphoreHandle_t done;
    kvm_audio_reader_t *pcm;
    void *enc;
    volatile bool stop;
    volatile uint32_t dropped;
    /* The task's own. */
    int64_t origin_us;
    uint64_t written; /* 48 kHz frames since origin_us */
    uint64_t frames;  /* AAC frames encoded */
    size_t have;      /* frames in pcm */
    int16_t *frame;   /* KVM_AAC_FRAMES, aligned for the encoder */
    uint8_t *adts;
    size_t adts_cap;
    uint8_t *silence; /* one silent frame, ADTS header included */
    uint16_t silence_len;
    item_t *item;
    kvm_audio_block_t in;
    int16_t pcm48[RESAMPLE_MAX * KVM_AUDIO_CHANNELS];
};

static portMUX_TYPE s_mu = portMUX_INITIALIZER_UNLOCKED;
static bool s_taken;

static void emit(kvm_aac_t *a, int64_t start_us, const uint8_t *data, size_t len)
{
    a->item->start_us = start_us;
    memcpy(a->item->data, data, len);
    /* Full: the oldest frames make room. */
    while (xRingbufferSend(a->out, a->item, sizeof(item_t) + len, 0) != pdTRUE) {
        size_t size = 0;
        void *old = xRingbufferReceive(a->out, &size, 0);
        if (!old) {
            a->dropped++;
            return;
        }
        vRingbufferReturnItem(a->out, old);
        a->dropped++;
    }
}

static int64_t frame_start_us(const kvm_aac_t *a)
{
    return a->origin_us + (int64_t)(a->frames * KVM_AAC_FRAMES * 1000000 / KVM_AAC_RATE);
}

static void encode(kvm_aac_t *a)
{
    esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)a->frame,
                                   .len = KVM_AAC_FRAMES * CHANNELS * sizeof(int16_t)};
    esp_audio_enc_out_frame_t out = {.buffer = a->adts, .len = a->adts_cap};
    const int64_t start_us = frame_start_us(a);
    a->frames++;
    if (esp_aac_enc_process(a->enc, &in, &out) != ESP_AUDIO_ERR_OK || out.encoded_bytes == 0 ||
        out.encoded_bytes > KVM_AAC_MAX) {
        a->dropped++;
        return;
    }
    emit(a, start_us, a->adts, out.encoded_bytes);
}

/* @p n stereo frames of @p pcm, or of silence when it is NULL. */
static void put(kvm_aac_t *a, const int16_t *pcm, size_t n)
{
    while (n) {
        if (!pcm && a->silence && a->have == 0 && n >= KVM_AAC_FRAMES) {
            emit(a, frame_start_us(a), a->silence, a->silence_len);
            a->frames++;
            a->written += KVM_AAC_FRAMES;
            n -= KVM_AAC_FRAMES;
            continue;
        }
        size_t k = KVM_AAC_FRAMES - a->have;
        if (k > n) {
            k = n;
        }
        int16_t *dst = a->frame + a->have;
        if (pcm) {
            for (size_t i = 0; i < k; i++) {
                dst[i] = (int16_t)(((int32_t)pcm[2 * i] + pcm[2 * i + 1]) / 2);
            }
            pcm += k * KVM_AUDIO_CHANNELS;
        } else {
            memset(dst, 0, k * sizeof(int16_t));
        }
        a->have += k;
        a->written += k;
        n -= k;
        if (a->have == KVM_AAC_FRAMES) {
            encode(a);
            a->have = 0;
        }
    }
}

static int64_t frames_at(const kvm_aac_t *a, int64_t us)
{
    return (us - a->origin_us) * KVM_AAC_RATE / 1000000;
}

static void enc_config(esp_aac_enc_config_t *cfg)
{
    *cfg = (esp_aac_enc_config_t)ESP_AAC_ENC_CONFIG_DEFAULT();
    cfg->sample_rate = KVM_AAC_RATE;
    cfg->channel = CHANNELS;
    cfg->bits_per_sample = 16;
    cfg->bitrate = BITRATE;
    cfg->adts_used = true;
}

/*
 * One silent frame, from an encoder of its own so the stream's is left as it
 * is. The first frames out of an AAC encoder carry its start-up; the third is
 * a steady one. Made on the encoder's task: the encoder needs its big stack.
 */
static esp_err_t make_silence(kvm_aac_t *a)
{
    esp_aac_enc_config_t cfg;
    enc_config(&cfg);
    void *enc = NULL;
    if (esp_aac_enc_open(&cfg, sizeof(cfg), &enc) != ESP_AUDIO_ERR_OK) {
        return ESP_FAIL;
    }
    memset(a->frame, 0, KVM_AAC_FRAMES * CHANNELS * sizeof(int16_t));
    esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)a->frame,
                                   .len = KVM_AAC_FRAMES * CHANNELS * sizeof(int16_t)};
    esp_audio_enc_out_frame_t out = {.buffer = a->adts, .len = a->adts_cap};
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3; i++) {
        out.encoded_bytes = 0;
        err = esp_aac_enc_process(enc, &in, &out) == ESP_AUDIO_ERR_OK && out.encoded_bytes > 0 &&
                      out.encoded_bytes <= KVM_AAC_MAX
                  ? ESP_OK
                  : ESP_FAIL;
    }
    esp_aac_enc_close(enc);
    if (err == ESP_OK) {
        a->silence = heap_caps_malloc(out.encoded_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!a->silence) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(a->silence, a->adts, out.encoded_bytes);
        a->silence_len = (uint16_t)out.encoded_bytes;
    }
    return err;
}

static void encode_task(void *arg)
{
    kvm_aac_t *a = arg;
    resampler_t rs = {0};
    uint64_t next_index = 0;
    uint32_t epoch = 0;
    if (make_silence(a) != ESP_OK) {
        ESP_LOGW(TAG, "no ready silent frame, silence is encoded");
    }
    a->origin_us = esp_timer_get_time();
    while (!a->stop) {
        if (kvm_audio_read(a->pcm, &a->in, 50) != ESP_OK) {
            const int64_t due = frames_at(a, esp_timer_get_time() - LAG_US);
            if (due > (int64_t)a->written) {
                put(a, NULL, (size_t)(due - (int64_t)a->written));
            }
            continue;
        }
        const kvm_audio_block_t *b = &a->in;
        if (b->rate != rs.rate || b->epoch != epoch || b->frame_index != next_index) {
            resample_reset(&rs, b->rate);
            epoch = b->epoch;
        }
        next_index = b->frame_index + b->frames;
        int64_t at_us;
        size_t n = resample_block(&rs, b, a->pcm48, &at_us);
        const int64_t due = frames_at(a, at_us);
        const int64_t pos = (int64_t)a->written;
        size_t skip = 0;
        if (due > pos + SLACK) {
            put(a, NULL, (size_t)(due - pos)); /* a gap */
        } else if (due < pos - SLACK) {
            skip = (size_t)(pos - due) < n ? (size_t)(pos - due) : n; /* already filled */
        }
        put(a, a->pcm48 + skip * KVM_AUDIO_CHANNELS, n - skip);
    }
    ESP_LOGI(TAG, "encoder stopped after %llu frames, %u lost; stack never below %u bytes free",
             (unsigned long long)a->frames, (unsigned)a->dropped,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(a->done);
    vTaskDeleteWithCaps(NULL);
}

static void release(kvm_aac_t *a)
{
    if (a->enc) {
        esp_aac_enc_close(a->enc);
    }
    kvm_audio_close(a->pcm);
    if (a->out) {
        vRingbufferDeleteWithCaps(a->out);
    }
    if (a->done) {
        vSemaphoreDelete(a->done);
    }
    heap_caps_free(a->frame);
    heap_caps_free(a->adts);
    heap_caps_free(a->item);
    heap_caps_free(a->silence);
    heap_caps_free(a);
    portENTER_CRITICAL(&s_mu);
    s_taken = false;
    portEXIT_CRITICAL(&s_mu);
}

esp_err_t kvm_aac_open(uint32_t keep_s, kvm_aac_t **out)
{
    *out = NULL;
    portENTER_CRITICAL(&s_mu);
    const bool taken = s_taken;
    s_taken = true;
    portEXIT_CRITICAL(&s_mu);
    if (taken) {
        return ESP_ERR_INVALID_STATE;
    }
    kvm_aac_t *a = heap_caps_calloc(1, sizeof(*a), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!a) {
        portENTER_CRITICAL(&s_mu);
        s_taken = false;
        portEXIT_CRITICAL(&s_mu);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = kvm_audio_open(PCM_DEPTH, &a->pcm);
    if (err != ESP_OK) {
        release(a);
        return err;
    }
    esp_aac_enc_config_t cfg;
    enc_config(&cfg);
    esp_audio_enc_frame_info_t info = {0};
    if (esp_aac_enc_get_frame_info_by_cfg(&cfg, &info) != ESP_AUDIO_ERR_OK ||
        info.in_frame_size != KVM_AAC_FRAMES * CHANNELS * (int)sizeof(int16_t) ||
        esp_aac_enc_open(&cfg, sizeof(cfg), &a->enc) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "encoder did not open");
        a->enc = NULL;
        release(a);
        return ESP_FAIL;
    }
    /* 16-byte aligned: the encoder's P4 code wants it. */
    a->adts_cap = (size_t)info.out_frame_size;
    a->frame = heap_caps_aligned_alloc(16, (size_t)info.in_frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    a->adts = heap_caps_aligned_alloc(16, a->adts_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    a->item = heap_caps_malloc(sizeof(item_t) + KVM_AAC_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!a->frame || !a->adts || !a->item) {
        release(a);
        return ESP_ERR_NO_MEM;
    }
    const size_t out_bytes = (size_t)keep_s * BYTES_PER_S > OUT_BYTES ? (size_t)keep_s * BYTES_PER_S : OUT_BYTES;
    a->out = xRingbufferCreateWithCaps(out_bytes, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
    a->done = xSemaphoreCreateBinary();
    if (!a->out || !a->done ||
        xTaskCreateWithCaps(encode_task, "aac", TASK_STACK, a, TASK_PRIO, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        release(a);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "encoding at %d bit/s", BITRATE);
    *out = a;
    return ESP_OK;
}

esp_err_t kvm_aac_read(kvm_aac_t *a, kvm_aac_frame_t *f, uint32_t timeout_ms)
{
    TickType_t t = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0 && t == 0) {
        t = 1;
    }
    size_t size = 0;
    item_t *it = xRingbufferReceive(a->out, &size, t);
    if (!it) {
        return ESP_ERR_TIMEOUT;
    }
    f->start_us = it->start_us;
    f->len = (uint16_t)(size - sizeof(item_t));
    memcpy(f->data, it->data, f->len);
    vRingbufferReturnItem(a->out, it);
    return ESP_OK;
}

uint32_t kvm_aac_dropped(const kvm_aac_t *a)
{
    return a->dropped;
}

void kvm_aac_close(kvm_aac_t *a)
{
    if (!a) {
        return;
    }
    a->stop = true;
    xSemaphoreTake(a->done, portMAX_DELAY);
    release(a);
}
