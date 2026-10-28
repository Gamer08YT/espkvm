/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The other end of espkvm-agent, the program on the target. It talks over the
 * USB serial port when usb_serial is set to "agent": one JSON object per line,
 * the device asks, the agent answers. The protocol is in the agent's repo,
 * PROTOCOL.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest text the device takes from or gives to the clipboard. */
#define KVM_AGENT_MAX_TEXT (64 * 1024)

typedef struct {
    bool port;          /**< the USB serial port is there, in agent mode */
    bool connected;     /**< an agent has the port open and said hello */
    char version[24];   /**< the agent's version, from its hello */
    char os[16];        /**< linux, windows, darwin */
    char ops[160];      /**< the ops it answers, comma separated */
} kvm_agent_status_t;

/** Start listening when usb_serial is "agent" and the port is there. */
esp_err_t kvm_agent_init(void);

void kvm_agent_status(kvm_agent_status_t *out);

/**
 * Ask the agent. @p params_json is a JSON object or NULL. On ESP_OK @p out
 * gets the whole response line (malloc'd, the caller frees it), which may
 * still say "ok":false. ESP_ERR_INVALID_STATE: no agent. ESP_ERR_TIMEOUT: it
 * did not answer in @p timeout_ms. One request at a time: a second caller waits
 * its turn within the same timeout.
 */
esp_err_t kvm_agent_call(const char *op, const char *params_json, int timeout_ms, char **out);

#ifdef __cplusplus
}
#endif
