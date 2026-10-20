/* DDNS core: pure C helpers with no ESP-IDF dependency, so they can be unit-tested on the host
 * (tools/ddns_host_test). The runtime in ddns.c owns all I/O, clocks and locking.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- IPv4 rules ---- */

/* Exactly four decimal octets 0-255, no leading zeros (except a lone 0), no sign, nothing trailing.
 * *out is host byte order. */
bool ddns_ipv4_parse(const char *s, size_t n, uint32_t *out);

/* False for private, loopback, link-local, CGNAT/tailnet, documentation, benchmark, multicast and
 * reserved ranges. */
bool ddns_ipv4_is_public(uint32_t ip);

/* A valid IP-source answer: 2xx, <= 64 bytes, surrounding ASCII whitespace only, one public IPv4. */
bool ddns_parse_ip_answer(int http_status, const char *body, size_t len, uint32_t *ip);

typedef enum { DDNS_Q_AGREE, DDNS_Q_FAIL } ddns_quorum_t;

/* All three slots are asked. AGREE when any two successful slots hold the same IP, otherwise FAIL. */
ddns_quorum_t ddns_quorum(const bool ok[3], const uint32_t ip[3], uint32_t *out);

/* memset that the compiler cannot drop; used for every buffer that held a secret. */
void ddns_wipe(void *p, size_t n);

/* ---- Configuration ---- */

#define DDNS_CFG_VERSION 1

typedef enum { DDNS_M_GET, DDNS_M_POST, DDNS_M_PUT, DDNS_M_PATCH } ddns_method_t;
typedef enum { DDNS_BODY_FORM, DDNS_BODY_JSON } ddns_body_type_t;
typedef enum { DDNS_SRC_IPIFY, DDNS_SRC_ICANHAZIP, DDNS_SRC_AWS, DDNS_SRC_IFCONFIG, DDNS_SRC_CUSTOM } ddns_src_kind_t;

/* Stored as one NVS blob; field sizes include the NUL. */
typedef struct {
    uint8_t version;
    bool enabled, src_ip, wan_trigger;
    uint8_t method, body_type; /* ddns_method_t / ddns_body_type_t */
    uint16_t interval_min, refresh_days;
    char url[257], header[257], body[513];
    char user[65], pass[129], token[129], match[65];
    struct {
        uint8_t kind; /* ddns_src_kind_t */
        char custom[129];
    } src[3];
} ddns_cfg_t;

/* Disabled, GET, form, every 5 min, WAN trigger on, no refresh, sources ipify/icanhazip/AWS. */
void ddns_cfg_defaults(ddns_cfg_t *c);

/* The preset URL for a slot, or its custom URL. */
const char *ddns_src_url(const ddns_cfg_t *c, int slot);

typedef enum {
    DDNS_OK = 0,
    DDNS_E_TOO_LONG,
    DDNS_E_PLACEHOLDER,
    DDNS_E_HEADER,
    DDNS_E_JSON,
    DDNS_E_URL,
    DDNS_E_NO_IP,
    DDNS_E_SECRET_LITERAL,
    DDNS_E_SCHEME,
    DDNS_E_SOURCES,
    DDNS_E_RANGE,
    DDNS_E_BODY_WITH_GET,
} ddns_err_t;

/* Short English text; never contains configuration values, so it is safe to log. */
const char *ddns_err_str(ddns_err_t e);

/* ---- Request templating ---- */

/* One filled-in provider request. Lives in PSRAM and is wiped after use. */
typedef struct {
    char url[513];
    char hdr_name[64], hdr_value[449]; /* hdr_name + ": " + hdr_value <= 512 */
    char body[1025];
    char auth[300]; /* full "Basic ..." value; empty = none */
    bool has_header;
} ddns_req_t;

/* Fill the URL, header and body templates of c with ip and the stored secrets, then check the
 * result. On any error out is wiped. */
ddns_err_t ddns_fill(const ddns_cfg_t *c, uint32_t ip, ddns_req_t *out);

/* Standard base64 with padding. Returns the length written (excluding NUL) or 0 when out is too
 * small. */
size_t ddns_base64(const uint8_t *in, size_t n, char *out, size_t out_sz);

/* Strict JSON syntax check, nesting depth <= 8. */
bool ddns_json_valid(const char *s);

/* ---- Save-time validation ---- */

/* Normalise c in place (literal user:pass@ moves into the user/pass fields), then validate it.
 * On failure msg holds one user-facing sentence that never contains a configured value. */
ddns_err_t ddns_cfg_validate(ddns_cfg_t *c, char *msg, size_t msg_sz);

/* FNV-1a 64 over every request setting (url, method, header, body, body type, source-IP flag,
 * user, pass, token). A change means the provider must be called again. */
uint64_t ddns_cfg_fingerprint(const ddns_cfg_t *c);

/* Host of an http(s) URL, without userinfo or port. The only URL-derived text allowed in logs,
 * SNMP and status. */
bool ddns_url_host(const char *url, char *host, size_t host_sz);

/* Scratch allocator for ddns_cfg_validate (defaults to malloc/free). The runtime points it at PSRAM
 * so the httpd task stack never carries a settings copy. */
void ddns_core_set_alloc(void *(*alloc)(size_t), void (*release)(void *));

/* ---- Provider response ---- */

/* '|'-separated alternatives, case-sensitive; a leading '^' anchors to the start of body; empty
 * pattern matches anything. body is trimmed and NUL-terminated. */
bool ddns_match(const char *pattern, const char *body);

typedef enum { DDNS_OUT_SUCCESS, DDNS_OUT_STOPPED, DDNS_OUT_COOLDOWN, DDNS_OUT_REDIRECT, DDNS_OUT_TEMP } ddns_outcome_t;

typedef struct {
    ddns_outcome_t outcome;
    uint32_t cooldown_s;
    char reason[24]; /* safe to log and show */
} ddns_class_t;

/* timed_out: the request budget expired or the connection dropped mid-body. body is <= 1024 bytes
 * and not NUL-terminated. retry_after is the raw Retry-After header or NULL. */
ddns_class_t ddns_classify(int http_status, bool timed_out, const char *body, size_t len, const char *match,
                           const char *retry_after);

/* At most 64 printable-ASCII characters of body for the web UI, with the password and token
 * (raw, percent-encoded) and base64("user:pass") replaced by "***". */
void ddns_redact_snippet(const char *body, size_t len, const char *user, const char *pass, const char *token, char *out,
                         size_t out_sz);

/* ---- Scheduler ---- */

/* All times are passed in; the core never reads a clock. *_us are monotonic microseconds
 * (esp_timer), *_wall are time_t seconds. */
typedef struct {
    /* last success (RAM only) */
    bool have_ok;
    uint32_t ok_ip;
    uint64_t ok_fp;
    int64_t ok_wall;
    /* limits */
    int64_t last_attempt_us;   /* 0 = never */
    int64_t cooldown_until_us; /* 0 = none */
    int64_t backoff_until_us;  /* 0 = none */
    uint32_t backoff_s;        /* current back-off step, 0 = none */
    int64_t last_force_us;     /* 0 = never */
    bool force_pending;
    bool stopped;
    bool stop_clear_pending; /* Force lifted a stop: the stored stop reason must be erased too */
    char stop_reason[24];
} ddns_sched_t;

/* false (and *wait_s) if Force was used less than 60 s ago; true sets force_pending and clears
 * stopped. */
bool ddns_sched_try_force(ddns_sched_t *s, int64_t now_us, uint32_t *wait_s);

/* Forced, first success not yet seen, (ip, fingerprint) changed, or refresh due. Never while
 * stopped without a pending force. */
bool ddns_sched_want_send(ddns_sched_t *s, uint32_t ip, uint64_t fp, int64_t wall_now, uint16_t refresh_days);

/* Earliest time a provider call is allowed: 60 s after the last attempt, after any cooldown and
 * back-off. 0 if none applies. */
int64_t ddns_sched_provider_ready_us(const ddns_sched_t *s);

/* The ready time has passed and neither the settings generation nor the uplink epoch changed
 * since the check started. */
bool ddns_sched_may_dispatch(const ddns_sched_t *s, int64_t now_us, uint32_t gen_at_start, uint32_t gen_now,
                             uint32_t epoch_at_start, uint32_t epoch_now);

/* Records the attempt time (for the 60 s limit) and consumes a pending force. */
void ddns_sched_on_attempt(ddns_sched_t *s, int64_t now_us);

/* Apply a provider outcome. same_gen = settings generation unchanged since dispatch; cooldown and
 * back-off apply regardless, success and stop only for the current settings. */
void ddns_sched_on_result(ddns_sched_t *s, const ddns_class_t *r, bool same_gen, int64_t now_us, int64_t wall_now,
                          uint32_t ip, uint64_t fp, uint16_t interval_min);

/* Settings saved: clears stopped, back-off and force_pending; keeps cooldown and last attempt. */
void ddns_sched_on_config_saved(ddns_sched_t *s);

/* The uplink changed while a request was in flight: its success no longer proves the published IP. */
void ddns_sched_on_uplink_moved(ddns_sched_t *s);

/* Applies a stop reason persisted in NVS ("ddns_stop"). NULL or empty does nothing; the reason is
 * truncated to fit stop_reason. */
void ddns_sched_apply_stored_stop(ddns_sched_t *s, const char *stored);
