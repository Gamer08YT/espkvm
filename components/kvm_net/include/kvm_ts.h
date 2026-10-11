/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Optional native Tailscale membership via the bundled microlink client, so the
 * device joins a tailnet directly - no separate gateway, NAT traversal handled -
 * and is reachable at its 100.x address from anywhere on the mesh. Everything is
 * gated by the ts_* settings and off by default. Unlike WireGuard this needs the
 * network to be up before it starts, so bring-up waits for the Ethernet IP.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Prepare internal state and start watching for the network to come up. */
esp_err_t kvm_ts_init(void);

/**
 * Reconcile the tailnet session with the ts_* settings: join (or rejoin with new
 * parameters) when enabled and configured and the network is up, otherwise tear
 * it down. Safe to call repeatedly; the slow work runs on a worker task.
 */
esp_err_t kvm_ts_apply(void);

typedef struct {
    bool enabled;       /**< the client is configured and started */
    bool up;            /**< registered with the control plane and ready */
    char address[24];   /**< our tailnet IP (100.x.y.z), empty until assigned */
    int  peers;         /**< number of known tailnet peers */
    /** When the node key runs out, in Unix epoch seconds; 0 when the control
     *  plane reported none (key expiry switched off) or has not answered yet.
     *  Tailscale allows six months at most, so this is a date worth watching. */
    int64_t key_expiry;
    bool key_expired;   /**< the control plane says the key is already out */
    char name[128];     /**< our MagicDNS name, "" when MagicDNS is off */
    /** The name the control plane will help certify (DNSConfig.CertDomains);
     *  "" when HTTPS certificates are off for the tailnet. */
    char cert_domain[128];
} kvm_ts_status_t;

/** Fill @p out with the tailnet client's current state. */
void kvm_ts_status(kvm_ts_status_t *out);

/**
 * Publish a TXT record through the control plane, for an ACME DNS-01 challenge
 * on our own tailnet name. Blocks until the control plane answers (up to a
 * minute). @p err gets a short reason on failure.
 */
esp_err_t kvm_ts_set_txt(const char *name, const char *value, char *err, size_t err_len);

/**
 * Callback invoked once the tailnet is up with the device's 100.x address and
 * MagicDNS name (either may be ""), so another component (the TLS layer) can name
 * them in the console certificate. Returning true means the values were new and
 * the device should restart to re-issue the certificate. Registered by the
 * composition root to avoid a dependency cycle with the web/TLS component.
 */
typedef bool (*kvm_ts_identity_cb_t)(const char *ip, const char *fqdn);
void kvm_ts_set_identity_cb(kvm_ts_identity_cb_t cb);

#ifdef __cplusplus
}
#endif
