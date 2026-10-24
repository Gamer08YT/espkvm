/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Frame rate limit: true when the frame that came at @p now_us should be
 * encoded. @p next_us is when the next one is due, 0 at the start.
 *
 * Frames come with jitter, so a frame up to a quarter interval early is taken.
 * The due time moves by a whole interval, so the average stays at the limit.
 * After a stall longer than an interval the missed slots are skipped and the
 * count starts again from now.
 */
static inline bool fps_gate_take(int64_t *next_us, int64_t now_us, int64_t interval_us)
{
    if (now_us < *next_us - interval_us / 4) {
        return false;
    }
    *next_us = now_us - *next_us > interval_us ? now_us + interval_us : *next_us + interval_us;
    return true;
}
