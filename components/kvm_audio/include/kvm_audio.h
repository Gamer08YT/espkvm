/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * HDMI audio. The capture bridge pulls the sound out of the HDMI stream and
 * sends it as I2S - bit clock, word clock, data - on three wires to three GPIOs
 * the operator picks. This device is the I2S slave: the bridge drives both
 * clocks, so the rate is whatever the source plays, read from the bridge.
 *
 * The receiver runs only while someone reads. Each reader gets its own queue of
 * 5 ms blocks, so a slow one (the SD card) never holds up a fast one (the
 * network); when a queue is full the oldest block goes. No source, no clocks or
 * a rate change are not errors: reads time out until sound comes again.
 *
 * Settings: aud_enable, aud_bclk, aud_ws, aud_din. Based on Zachary Leahan's
 * kvm_audio in the espkvm-xbox fork.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_AUDIO_CHANNELS 2
/** One block is 5 ms (rate / 200 frames); this is 5 ms at 96 kHz, the top rate. */
#define KVM_AUDIO_MAX_FRAMES 480

typedef struct {
    /** esp_timer time of the first sample, from the DMA interrupt. */
    int64_t start_us;
    /** Frames since the receiver started. A dropped block leaves a gap; a
     * restart (overflow, new rate) begins a new epoch at 0. */
    uint64_t frame_index;
    uint32_t epoch;
    uint32_t rate;
    uint16_t frames;
    int16_t pcm[KVM_AUDIO_MAX_FRAMES * KVM_AUDIO_CHANNELS]; /**< interleaved L R */
} kvm_audio_block_t;

typedef struct kvm_audio_reader kvm_audio_reader_t;

typedef struct {
    bool enabled;        /**< switched on, with three distinct pins */
    bool running;        /**< the I2S receiver is open */
    uint32_t rate;       /**< what the receiver runs at, 0 when closed */
    int bclk, ws, din;   /**< -1 when not set */
    uint8_t readers;
    uint64_t frames;     /**< received since boot */
    uint32_t overruns;   /**< DMA overflows, each one restarts the receiver */
    uint32_t restarts;
    float peak;          /**< last block, 0..1 */
    float rms;
    int64_t last_us;     /**< esp_timer time of the last block, 0 for none */
    uint32_t isr_gap_max_us;  /**< longest wait between two DMA blocks */
    uint32_t read_gap_max_us; /**< longest wait between two reads */
} kvm_audio_status_t;

/** One-time setup: locks and the receiver task. Opens no I2S yet. */
esp_err_t kvm_audio_init(void);

/** (Re)read the aud_* settings and report KVM_CAP_AUDIO. */
void kvm_audio_apply(void);

/**
 * Start reading. @p depth is how many 5 ms blocks the queue holds before the
 * oldest goes. ESP_ERR_INVALID_STATE when audio is off or all readers are taken.
 */
esp_err_t kvm_audio_open(uint16_t depth, kvm_audio_reader_t **out);

/** Next block. ESP_ERR_TIMEOUT when none came within @p timeout_ms. */
esp_err_t kvm_audio_read(kvm_audio_reader_t *r, kvm_audio_block_t *block, uint32_t timeout_ms);

/** Blocks this reader lost to a full queue. */
uint32_t kvm_audio_dropped(const kvm_audio_reader_t *r);

void kvm_audio_close(kvm_audio_reader_t *r);

void kvm_audio_status(kvm_audio_status_t *out);

/*
 * The same sound as Opus, for the browser: 20 ms packets, 48 kHz stereo,
 * whatever rate the source plays. One encoder serves every reader and runs
 * only while there is one.
 */

/** Room for one packet; at the bitrate used one is about 240 bytes. */
#define KVM_OPUS_MAX 600
#define KVM_OPUS_FRAMES 960 /* 20 ms at 48 kHz */

typedef struct {
    int64_t start_us;  /**< esp_timer time of the first sample */
    /** 48 kHz sample count: advances by KVM_OPUS_FRAMES a packet, and jumps
     * after a gap so it keeps following start_us. */
    uint32_t ts;
    bool resync;       /**< first packet after a gap */
    uint16_t len;
    uint8_t data[KVM_OPUS_MAX];
} kvm_opus_packet_t;

typedef struct kvm_opus_reader kvm_opus_reader_t;

/** Like kvm_audio_open, with @p depth 20 ms packets. */
esp_err_t kvm_opus_open(uint16_t depth, kvm_opus_reader_t **out);
esp_err_t kvm_opus_read(kvm_opus_reader_t *r, kvm_opus_packet_t *pkt, uint32_t timeout_ms);
void kvm_opus_close(kvm_opus_reader_t *r);

/*
 * The same sound as AAC-LC, for recordings: 48 kHz mono, 1024 frames
 * (21.3 ms) a frame, each with its ADTS header. Unlike Opus it never stops:
 * no sound, a gap, or a source clock a little off is made up with silence or
 * trimmed, so frame n starts n * 1024 / 48000 s after the first and stays
 * within 20 ms of the time its sound was captured. The frames wait for the
 * reader in PSRAM; when that is full the oldest go. One reader at a time; the
 * encoder runs while it is open.
 */
#define KVM_AAC_FRAMES 1024
#define KVM_AAC_RATE 48000
/** Room for one ADTS frame; at the bitrate used one is about 170 bytes. */
#define KVM_AAC_MAX 1600

typedef struct {
    int64_t start_us; /**< esp_timer time of the first sample */
    uint16_t len;
    uint8_t data[KVM_AAC_MAX];
} kvm_aac_frame_t;

typedef struct kvm_aac kvm_aac_t;

/**
 * @p keep_s: the frames waiting are kept for at least this many seconds, and
 * never less than about 8. ESP_ERR_INVALID_STATE when audio is off or the
 * encoder is taken.
 */
esp_err_t kvm_aac_open(uint32_t keep_s, kvm_aac_t **out);
esp_err_t kvm_aac_read(kvm_aac_t *a, kvm_aac_frame_t *f, uint32_t timeout_ms);
/** Frames that went unread: the oldest, pushed out when the reader fell behind. */
uint32_t kvm_aac_dropped(const kvm_aac_t *a);
void kvm_aac_close(kvm_aac_t *a);

#ifdef __cplusplus
}
#endif
