/* Dynamic DNS client — runtime API.
 *
 * A single PSRAM-stack task checks the public IPv4 address against a quorum of
 * three HTTPS sources and calls the configured provider when it changed (or
 * when forced / a refresh is due). Settings live in NVS as one versioned blob
 * ("ddns_cfg", namespace "tsr"); a permanent rejection is kept in "ddns_stop".
 *
 * SPDX-License-Identifier: MIT */

#ifndef DDNS_H_
#define DDNS_H_

#include "ddns_core.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Supplied by the application so this component never names a specific uplink. */
typedef struct {
    bool (*uplink_up)(void);
    bool (*ota_busy)(void);
} ddns_platform_t;

/* Load the blob + stop reason, register the IP_EVENT handlers once, start the task if enabled. */
void ddns_init(const ddns_platform_t *p);

/* Uplink failover / state change outside the IP_EVENTs: bumps the uplink epoch and, with the WAN
 * trigger on and the uplink up, schedules a check about 10 s later. */
void ddns_notify_uplink_changed(void);

/* Numeric order is internal; the SNMP nsExtendResult mapping is 1,2,0,3,4,5,6. */
typedef enum {
    DDNS_ST_DISABLED,
    DDNS_ST_WAITING,
    DDNS_ST_OK,
    DDNS_ST_QUORUM_FAILED,
    DDNS_ST_PROVIDER_ERROR,
    DDNS_ST_COOLDOWN,
    DDNS_ST_STOPPED
} ddns_state_t;

typedef struct {
    ddns_state_t state;
    char detail[32]; /* missing prerequisite / stop reason / "http 429" */
    char provider_host[64];
    struct {
        bool asked, ok;
        uint32_t ip;
        char err[24];
    } src[3];
    uint32_t quorum_ip, last_ok_ip; /* host byte order, 0 = none */
    int64_t last_ok_wall;           /* time_t of the last success, 0 = none */
    int last_http;                  /* last provider HTTP status, 0 = none, <0 = transport error */
    char last_error[24];
    char snippet[65];                                /* redacted response start, web UI only */
    uint32_t next_check_s, cooldown_s, force_wait_s; /* seconds from now, 0 = none */
    uint32_t stack_free_min;                         /* task stack high-water mark, bytes */
} ddns_status_t;

/* Snapshot copy; never blocks behind a provider request. */
void ddns_get_status(ddns_status_t *out);

#define DDNS_KEEP_PASS 0x1
#define DDNS_KEEP_TOKEN 0x2

/* Active settings with pass/token zeroed; their stored lengths go to *pass_len / *token_len. */
void ddns_get_config(ddns_cfg_t *out, size_t *pass_len, size_t *token_len);

/* Validate + persist + apply. keep_flags keep the stored password/token instead of in's. On error
 * msg says why and the old settings stay. in's secrets are wiped before returning. May wait up to
 * the 20 s provider-request budget. */
esp_err_t ddns_save_config(ddns_cfg_t *in, unsigned keep_flags, char *msg, size_t msg_sz);

/* "Force update now". false (and *wait_s) if used less than 60 s ago. Returns at once. */
bool ddns_force(uint32_t *wait_s);

#ifdef __cplusplus
}
#endif

#endif /* DDNS_H_ */
