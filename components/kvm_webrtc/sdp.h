/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reading a browser's SDP offer and writing the answer, for exactly one case:
 * the browser receives one H.264 video stream from the device. Anything else
 * in the offer is declined.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDP_UFRAG_MAX 64
#define SDP_PWD_MAX 128
#define SDP_MID_MAX 32
#define SDP_PLID_MAX 8

typedef struct {
    char ufrag[SDP_UFRAG_MAX];
    char pwd[SDP_PWD_MAX];
    /* The browser's certificate, SHA-256. */
    uint8_t fingerprint[32];
    bool has_fingerprint;
    char mid[SDP_MID_MAX];
    /* The chosen H.264 payload type and the profile-level-id it offered. */
    int pt;
    char plid[SDP_PLID_MAX];
    bool fb_nack, fb_pli, fb_fir, fb_remb;
    bool rtcp_rsize;
    /* The id the offer gave the playout-delay header extension, 1..14, or 0
       when it did not offer it. */
    int playout_ext;
    /* m-lines in the offer; only the first video one is accepted. */
    int m_count;
} sdp_offer_t;

typedef struct {
    uint32_t ip_be;
    uint16_t port;
} sdp_candidate_t;

/** 0 when the offer has an H.264 video stream this device can send. */
int sdp_parse_offer(const char *sdp, sdp_offer_t *out);

typedef struct {
    const char *ufrag, *pwd;
    const uint8_t *fingerprint; /* ours, SHA-256 */
    uint32_t ssrc;
    const sdp_candidate_t *cands;
    int cand_count;
} sdp_answer_params_t;

/**
 * Write the answer to @p offer. Returns its length, or -1 when @p cap is too
 * small.
 */
int sdp_write_answer(const sdp_offer_t *offer, const sdp_answer_params_t *p, char *out,
                     size_t cap);

#ifdef __cplusplus
}
#endif
