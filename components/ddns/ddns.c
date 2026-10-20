/* Dynamic DNS client — ESP-IDF runtime.
 *
 * Pure logic (templating, validation, quorum, classification, scheduling) is in
 * ddns_core.c. This file owns the task, the HTTP exchange, the destination
 * policy, the NVS blob and the IP_EVENT wiring.
 *
 * Locks:
 *   s_lock     — active config, scheduler, config generation. Held by the task
 *                across the provider dispatch (open + write), so a settings save
 *                can never land between the generation check and the send.
 *   s_st_lock  — status snapshot and the readers' copy of the config. Never held
 *                across I/O, so web UI and SNMP readers never wait on a request.
 *   s_trig     — spinlock for the trigger flags touched from the event loop
 *                (uplink epoch, WAN-up deadline, rerun, force request).
 *   s_nvs_lock — serialises the stop-reason write (esp_timer task) against the
 *                settings save, so a stop queued before a save is never written
 *                back after the save erased it (s_stop_epoch).
 * Order: s_lock before s_st_lock / s_nvs_lock; s_trig is innermost.
 *
 * Classification logic ported from pfSense's dyndns.class (pfsense master
 * 9363ac5, Apache-2.0); see ddns_core.c.
 *
 * SPDX-License-Identifier: MIT */

#include "ddns.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h" /* xTaskCreateWithCaps — PSRAM stack */
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_parser.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "nvs.h"
#include "snmp_agent.h"

static const char *TAG = "ddns";

#define NVS_NS "tsr"
#define NVS_KEY_CFG "ddns_cfg"
#define NVS_KEY_STOP "ddns_stop"

#define US_PER_S 1000000LL
#define REQ_BUDGET_US (20 * US_PER_S) /* DNS + connect + TLS + body */
#define WAN_DELAY_US (10 * US_PER_S)
#define TIME_RETRY_US (60 * US_PER_S)
#define MAX_WAIT_MS 60000 /* per notify wait; keeps pdMS_TO_TICKS small */
#define SRC_READ_CAP 65   /* one byte over the 64-byte answer limit */
#define RESP_CAP 1024
#define TASK_STACK 8192
#define TASK_PRIO 3 /* same as the telemetry sender */
#define PLAUSIBLE_TIME 1700000000

static ddns_platform_t s_plat;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_st_lock;
static SemaphoreHandle_t s_nvs_lock;
static portMUX_TYPE s_trig = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static esp_timer_handle_t s_stop_timer;
static uint32_t s_stop_epoch; /* bumped by every save (s_lock + s_nvs_lock) */
static uint32_t s_stop_epoch_queued;
static volatile bool s_wan_trigger;

/* The larger state lives in one PSRAM block (internal RAM is the scarce resource on this board).
 * It exists whenever s_lock does. */
typedef struct {
    ddns_sched_t sched;      /* under s_lock */
    ddns_status_t status;    /* under s_st_lock */
    ddns_sched_t sched_view; /* under s_st_lock: copy for readers */
    char cool_reason[24];    /* under s_st_lock */
    char stop_reason[24];    /* handed to the esp_timer callback (s_trig) */
} ddns_rt_t;
static ddns_rt_t *s_rt;
#define s_sched (s_rt->sched)
#define s_status (s_rt->status)
#define s_sched_view (s_rt->sched_view)
#define s_cool_reason (s_rt->cool_reason)
#define s_stop_reason (s_rt->stop_reason)

/* under s_lock (s_cfg also written under s_st_lock, see ddns_save_config) */
static ddns_cfg_t *s_cfg;
static uint32_t s_gen;

/* under s_st_lock */
static int64_t s_next_due_us;

/* under s_trig */
static uint32_t s_epoch;
static int64_t s_wanup_at_us;  /* 0 = none */
static int64_t s_send_wait_us; /* provider-ready deadline of a blocked send, 0 = none */
static int64_t s_next_periodic_us;
static bool s_run_pending;
static bool s_force_req;
static int64_t s_force_at_us; /* last accepted Force, 0 = never */

static void *psram_alloc(size_t n) { return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM); }

static void copy_str(char *dst, size_t sz, const char *src) {
    if (sz)
        strlcpy(dst, src ? src : "", sz);
}

static void wake_task(void) {
    if (s_task)
        xTaskNotifyGive(s_task);
}

/* ---- status helpers (s_st_lock) ---- */

static void status_publish_sched(void) {
    /* caller holds s_lock */
    xSemaphoreTake(s_st_lock, portMAX_DELAY);
    s_sched_view = s_sched;
    s_status.last_ok_ip = s_sched.have_ok ? s_sched.ok_ip : 0;
    s_status.last_ok_wall = s_sched.have_ok ? s_sched.ok_wall : 0;
    xSemaphoreGive(s_st_lock);
}

static void status_set(ddns_state_t st, const char *detail) {
    xSemaphoreTake(s_st_lock, portMAX_DELAY);
    s_status.state = st;
    copy_str(s_status.detail, sizeof s_status.detail, detail);
    xSemaphoreGive(s_st_lock);
}

/* ---- destination policy ---- */

static bool url_host(const char *url, char *host, size_t host_sz) {
    struct http_parser_url u;
    http_parser_url_init(&u);
    if (http_parser_parse_url(url, strlen(url), 0, &u) != 0 || !(u.field_set & (1 << UF_HOST)) ||
        !(u.field_set & (1 << UF_SCHEMA)))
        return false;
    size_t n = u.field_data[UF_HOST].len;
    if (n == 0 || n >= host_sz)
        return false;
    memcpy(host, url + u.field_data[UF_HOST].off, n);
    host[n] = '\0';
    return true;
}

/* Every IPv4 address the host resolves to must be public. Literal IPs go through the same path. */
static bool dest_allowed(const char *host) {
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        if (res)
            freeaddrinfo(res);
        return false;
    }
    bool ok = true;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        if (a->ai_family != AF_INET || !a->ai_addr) {
            ok = false;
            break;
        }
        const struct sockaddr_in *sin = (const struct sockaddr_in *)a->ai_addr;
        if (!ddns_ipv4_is_public(ntohl(sin->sin_addr.s_addr))) {
            ok = false;
            break;
        }
    }
    freeaddrinfo(res);
    return ok;
}

/* ---- HTTP exchange ---- */

typedef struct {
    char *retry_after;
    size_t ra_sz;
} hdr_ctx_t;

/* esp_http_client_get_header() reads *request* headers, so Retry-After is captured here. */
static esp_err_t http_event(esp_http_client_event_t *e) {
    hdr_ctx_t *ctx = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && ctx && ctx->retry_after && e->header_key && e->header_value &&
        strcasecmp(e->header_key, "Retry-After") == 0)
        copy_str(ctx->retry_after, ctx->ra_sz, e->header_value);
    return ESP_OK;
}

static esp_http_client_method_t map_method(ddns_method_t m) {
    switch (m) {
    case DDNS_M_POST:
        return HTTP_METHOD_POST;
    case DDNS_M_PUT:
        return HTTP_METHOD_PUT;
    case DDNS_M_PATCH:
        return HTTP_METHOD_PATCH;
    case DDNS_M_GET:
    default:
        return HTTP_METHOD_GET;
    }
}

/* Returns the HTTP status, or <0 on a transport error. With hold_lock_until_written the caller
 * holds s_lock, and it is released here once the request is on the wire (or failed). */
static int http_exchange(const char *url, ddns_method_t m, const ddns_req_t *req, bool json_body, char *resp,
                         size_t resp_cap, size_t *resp_len, bool *timed_out, char *retry_after, size_t ra_sz,
                         bool hold_lock_until_written) {
    int64_t start = esp_timer_get_time();
    *resp_len = 0;
    *timed_out = false;
    if (retry_after && ra_sz)
        retry_after[0] = '\0';

    hdr_ctx_t ctx = {.retry_after = retry_after, .ra_sz = ra_sz};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = map_method(m),
        .timeout_ms = 10000,
        .disable_auto_redirect = true,
        .keep_alive_enable = false,
        /* internal RAM (below the PSRAM malloc threshold): an IP source (no req) answers <= 64 bytes */
        .buffer_size = req ? 1024 : 512,
        .buffer_size_tx = req ? 1024 : 512,
        .event_handler = http_event,
        .user_data = &ctx,
    };
    if (strncasecmp(url, "https://", 8) == 0)
        cfg.crt_bundle_attach = esp_crt_bundle_attach;

    int status = -1;
    bool locked = hold_lock_until_written;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c)
        goto out;

    char ua[48];
    snprintf(ua, sizeof ua, "esp32-tsr-ddns/%s", esp_app_get_description()->version);
    esp_http_client_set_header(c, "User-Agent", ua);
    size_t body_len = 0;
    if (req) {
        body_len = strlen(req->body);
        if (body_len)
            esp_http_client_set_header(c, "Content-Type",
                                       json_body ? "application/json" : "application/x-www-form-urlencoded");
        if (req->auth[0])
            esp_http_client_set_header(c, "Authorization", req->auth);
        if (req->has_header)
            esp_http_client_set_header(c, req->hdr_name, req->hdr_value);
    }

    if (esp_http_client_open(c, (int)body_len) != ESP_OK) {
        *timed_out = esp_timer_get_time() - start >= REQ_BUDGET_US;
        goto out;
    }
    if (body_len && esp_http_client_write(c, req->body, (int)body_len) != (int)body_len)
        goto out;
    if (locked) {
        xSemaphoreGive(s_lock);
        locked = false;
    }

    if (esp_http_client_fetch_headers(c) < 0) {
        *timed_out = true;
        goto out;
    }
    status = esp_http_client_get_status_code(c);
    while (*resp_len < resp_cap) {
        if (esp_timer_get_time() - start >= REQ_BUDGET_US) {
            *timed_out = true;
            break;
        }
        int n = esp_http_client_read(c, resp + *resp_len, (int)(resp_cap - *resp_len));
        if (n < 0) {
            *timed_out = true; /* connection drop mid-body */
            break;
        }
        if (n == 0)
            break;
        *resp_len += (size_t)n;
    }

out:
    if (locked)
        xSemaphoreGive(s_lock);
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    return status;
}

/* ---- stop persistence (esp_timer task: internal stack, may write flash) ---- */

static void stop_timer_cb(void *arg) {
    (void)arg;
    char reason[sizeof s_stop_reason];
    uint32_t epoch;
    portENTER_CRITICAL(&s_trig);
    memcpy(reason, s_stop_reason, sizeof reason);
    epoch = s_stop_epoch_queued;
    portEXIT_CRITICAL(&s_trig);
    xSemaphoreTake(s_nvs_lock, portMAX_DELAY);
    if (epoch == s_stop_epoch) { /* no save since the stop was decided */
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            /* an empty reason means Force lifted the stop: erase it so a reboot does not restore it */
            esp_err_t e = reason[0] ? nvs_set_str(h, NVS_KEY_STOP, reason) : nvs_erase_key(h, NVS_KEY_STOP);
            if (e == ESP_OK)
                nvs_commit(h);
            nvs_close(h);
        }
    }
    xSemaphoreGive(s_nvs_lock);
}

/* ---- one check ---- */

static void fetch_source(const ddns_cfg_t *c, int slot, char *buf, bool *ok, uint32_t *ip, char *err, size_t err_sz) {
    const char *url = ddns_src_url(c, slot);
    char host[64];
    *ok = false;
    err[0] = '\0';
    if (!url_host(url, host, sizeof host)) {
        copy_str(err, err_sz, "invalid URL");
        return;
    }
    if (!dest_allowed(host)) {
        copy_str(err, err_sz, "destination not public");
        return;
    }
    size_t len = 0;
    bool to = false;
    int st = http_exchange(url, DDNS_M_GET, NULL, false, buf, SRC_READ_CAP, &len, &to, NULL, 0, false);
    if (to)
        copy_str(err, err_sz, "timeout");
    else if (st < 0)
        copy_str(err, err_sz, "connect failed");
    else if (ddns_parse_ip_answer(st, buf, len, ip))
        *ok = true;
    else if (st >= 200 && st <= 299)
        copy_str(err, err_sz, "bad answer");
    else
        snprintf(err, err_sz, "http %d", st);
}

static ddns_state_t state_for(ddns_outcome_t o) {
    switch (o) {
    case DDNS_OUT_SUCCESS:
        return DDNS_ST_OK;
    case DDNS_OUT_STOPPED:
        return DDNS_ST_STOPPED;
    case DDNS_OUT_COOLDOWN:
        return DDNS_ST_COOLDOWN;
    default:
        return DDNS_ST_PROVIDER_ERROR;
    }
}

static void run_check(void) {
    ddns_cfg_t *cfg = NULL;
    ddns_req_t *req = NULL;
    char *resp = NULL;
    int64_t now = esp_timer_get_time();

    /* Apply a pending Force, then snapshot config, generation and epoch. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    portENTER_CRITICAL(&s_trig);
    bool force = s_force_req;
    int64_t force_at = s_force_at_us;
    s_force_req = false;
    uint32_t epoch0 = s_epoch;
    portEXIT_CRITICAL(&s_trig);
    if (force) {
        uint32_t w;
        ddns_sched_try_force(&s_sched, force_at, &w);
        if (s_sched.stop_clear_pending) {
            s_sched.stop_clear_pending = false;
            portENTER_CRITICAL(&s_trig);
            s_stop_reason[0] = '\0';
            s_stop_epoch_queued = s_stop_epoch;
            portEXIT_CRITICAL(&s_trig);
            esp_timer_start_once(s_stop_timer, 0);
        }
    }
    bool enabled = s_cfg->enabled;
    int64_t interval_us = (int64_t)(s_cfg->interval_min ? s_cfg->interval_min : 5) * 60 * US_PER_S;
    uint32_t gen0 = s_gen;
    if (enabled) {
        cfg = heap_caps_calloc(1, sizeof *cfg, MALLOC_CAP_SPIRAM);
        if (cfg)
            memcpy(cfg, s_cfg, sizeof *cfg);
    }
    status_publish_sched();
    xSemaphoreGive(s_lock);

    portENTER_CRITICAL(&s_trig);
    s_next_periodic_us = now + (enabled ? interval_us : 3600 * US_PER_S);
    if (!enabled)
        s_send_wait_us = 0;
    portEXIT_CRITICAL(&s_trig);

    if (!enabled) {
        status_set(DDNS_ST_DISABLED, "");
        goto cleanup;
    }
    if (!cfg) {
        status_set(DDNS_ST_WAITING, "out of memory");
        goto cleanup;
    }
    if (!s_plat.uplink_up || !s_plat.uplink_up()) {
        status_set(DDNS_ST_WAITING, "no uplink");
        goto cleanup;
    }
    if (s_plat.ota_busy && s_plat.ota_busy()) {
        status_set(DDNS_ST_WAITING, "OTA in progress");
        goto cleanup;
    }
    if (time(NULL) < PLAUSIBLE_TIME) {
        status_set(DDNS_ST_WAITING, "waiting for time sync");
        portENTER_CRITICAL(&s_trig);
        s_next_periodic_us = now + TIME_RETRY_US;
        portEXIT_CRITICAL(&s_trig);
        goto cleanup;
    }

    resp = heap_caps_calloc(1, RESP_CAP + 1, MALLOC_CAP_SPIRAM);
    req = heap_caps_calloc(1, sizeof *req, MALLOC_CAP_SPIRAM);
    if (!resp || !req) {
        status_set(DDNS_ST_WAITING, "out of memory");
        goto cleanup;
    }

    /* Sources: all three every time, so the status always shows each one. */
    bool ok[3] = {false, false, false};
    uint32_t ip[3] = {0, 0, 0};
    char err[3][24] = {"", "", ""};
    bool asked[3] = {true, true, true};
    uint32_t qip = 0;
    for (int i = 0; i < 3; i++)
        fetch_source(cfg, i, resp, &ok[i], &ip[i], err[i], sizeof err[i]);
    ddns_quorum_t q = ddns_quorum(ok, ip, &qip);
    xSemaphoreTake(s_st_lock, portMAX_DELAY);
    for (int i = 0; i < 3; i++) {
        s_status.src[i].asked = asked[i];
        s_status.src[i].ok = ok[i];
        s_status.src[i].ip = ok[i] ? ip[i] : 0;
        copy_str(s_status.src[i].err, sizeof s_status.src[i].err, err[i]);
    }
    s_status.quorum_ip = q == DDNS_Q_AGREE ? qip : 0;
    xSemaphoreGive(s_st_lock);
    if (q != DDNS_Q_AGREE) {
        status_set(DDNS_ST_QUORUM_FAILED, "");
        goto cleanup;
    }

    /* Send decision. */
    uint64_t fp = ddns_cfg_fingerprint(cfg);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool want = ddns_sched_want_send(&s_sched, qip, fp, (int64_t)time(NULL), cfg->refresh_days);
    bool stopped = s_sched.stopped;
    char stop_reason[sizeof s_sched.stop_reason];
    memcpy(stop_reason, s_sched.stop_reason, sizeof stop_reason);
    xSemaphoreGive(s_lock);
    if (!want) {
        if (stopped)
            status_set(DDNS_ST_STOPPED, stop_reason);
        else
            status_set(DDNS_ST_OK, "");
        goto cleanup;
    }

    /* Dispatch boundary. */
    ddns_err_t fe = ddns_fill(cfg, qip, req);
    if (fe != DDNS_OK) {
        status_set(DDNS_ST_PROVIDER_ERROR, ddns_err_str(fe));
        goto cleanup;
    }
    char host[64];
    if (!url_host(req->url, host, sizeof host)) {
        status_set(DDNS_ST_PROVIDER_ERROR, "invalid URL");
        goto cleanup;
    }
    if (!dest_allowed(host)) {
        status_set(DDNS_ST_PROVIDER_ERROR, "destination not public");
        goto cleanup;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t t_disp = esp_timer_get_time();
    portENTER_CRITICAL(&s_trig);
    uint32_t epoch_now = s_epoch;
    portEXIT_CRITICAL(&s_trig);
    if (!ddns_sched_may_dispatch(&s_sched, t_disp, gen0, s_gen, epoch0, epoch_now)) {
        bool gen_changed = gen0 != s_gen;
        bool time_only = !gen_changed && epoch0 == epoch_now;
        int64_t ready = ddns_sched_provider_ready_us(&s_sched);
        bool cooling = s_sched.cooldown_until_us > t_disp;
        xSemaphoreGive(s_lock);
        portENTER_CRITICAL(&s_trig);
        if (time_only) {
            s_send_wait_us = ready;
        } else if (gen_changed) {
            s_run_pending = true;
        } else if (!s_wanup_at_us || s_wanup_at_us > t_disp + WAN_DELAY_US) {
            s_wanup_at_us = t_disp + WAN_DELAY_US; /* uplink churn: let it settle, never spin */
        }
        portEXIT_CRITICAL(&s_trig);
        if (time_only && cooling) {
            xSemaphoreTake(s_st_lock, portMAX_DELAY);
            s_status.state = DDNS_ST_COOLDOWN;
            copy_str(s_status.detail, sizeof s_status.detail, s_cool_reason);
            xSemaphoreGive(s_st_lock);
        }
        goto cleanup;
    }
    ddns_sched_on_attempt(&s_sched, t_disp);
    status_publish_sched();
    size_t len = 0;
    bool to = false;
    char ra[16];
    /* releases s_lock once the request is written */
    int st = http_exchange(req->url, (ddns_method_t)cfg->method, req, cfg->body_type == DDNS_BODY_JSON, resp, RESP_CAP,
                           &len, &to, ra, sizeof ra, true);

    ddns_class_t r;
    if (st < 0 && !to) {
        memset(&r, 0, sizeof r);
        r.outcome = DDNS_OUT_TEMP;
        copy_str(r.reason, sizeof r.reason, "connect failed");
    } else {
        r = ddns_classify(st, to, resp, len, cfg->match, ra);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool same_gen = s_gen == gen0;
    ddns_sched_on_result(&s_sched, &r, same_gen, esp_timer_get_time(), (int64_t)time(NULL), qip, fp, cfg->interval_min);
    /* A failover while connecting can put the old uplink's IP on the new one: check again. */
    portENTER_CRITICAL(&s_trig);
    bool moved = s_epoch != epoch0;
    if (moved)
        s_run_pending = true;
    portEXIT_CRITICAL(&s_trig);
    if (moved)
        ddns_sched_on_uplink_moved(&s_sched);
    if (r.outcome == DDNS_OUT_STOPPED && same_gen) {
        portENTER_CRITICAL(&s_trig);
        memcpy(s_stop_reason, r.reason, sizeof s_stop_reason);
        s_stop_epoch_queued = s_stop_epoch;
        portEXIT_CRITICAL(&s_trig);
        esp_timer_start_once(s_stop_timer, 0);
    }
    status_publish_sched();
    xSemaphoreGive(s_lock);

    xSemaphoreTake(s_st_lock, portMAX_DELAY);
    if (r.outcome == DDNS_OUT_COOLDOWN)
        copy_str(s_cool_reason, sizeof s_cool_reason, r.reason);
    if (same_gen) {
        s_status.state = state_for(r.outcome);
        copy_str(s_status.detail, sizeof s_status.detail, r.outcome == DDNS_OUT_SUCCESS ? "" : r.reason);
        s_status.last_http = st;
        copy_str(s_status.last_error, sizeof s_status.last_error, r.outcome == DDNS_OUT_SUCCESS ? "" : r.reason);
        ddns_redact_snippet(resp, len, cfg->user, cfg->pass, cfg->token, s_status.snippet, sizeof s_status.snippet);
    }
    xSemaphoreGive(s_st_lock);
    if (r.outcome == DDNS_OUT_SUCCESS)
        ESP_LOGI(TAG, "updated via %s (http %d)", host, st);
    else
        ESP_LOGW(TAG, "update via %s failed: %s (http %d)", host, r.reason, st);

cleanup:
    if (cfg) {
        ddns_wipe(cfg, sizeof *cfg);
        free(cfg);
    }
    if (req) {
        ddns_wipe(req, sizeof *req);
        free(req);
    }
    if (resp) {
        ddns_wipe(resp, RESP_CAP + 1);
        free(resp);
    }
    uint32_t hw = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    xSemaphoreTake(s_st_lock, portMAX_DELAY);
    if (s_status.stack_free_min == 0 || hw < s_status.stack_free_min)
        s_status.stack_free_min = hw;
    xSemaphoreGive(s_st_lock);
}

/* ---- task ---- */

static void ddns_task(void *arg) {
    (void)arg;
    for (;;) {
        int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_trig);
        bool wan_due = s_wanup_at_us && now >= s_wanup_at_us;
        bool send_due = s_send_wait_us && now >= s_send_wait_us;
        bool due = s_run_pending || now >= s_next_periodic_us || wan_due || send_due;
        if (due) {
            s_run_pending = false;
            if (wan_due)
                s_wanup_at_us = 0;
            if (send_due)
                s_send_wait_us = 0;
        }
        portEXIT_CRITICAL(&s_trig);
        if (due)
            run_check();

        now = esp_timer_get_time();
        portENTER_CRITICAL(&s_trig);
        int64_t next = s_next_periodic_us;
        if (s_wanup_at_us && s_wanup_at_us < next)
            next = s_wanup_at_us;
        if (s_send_wait_us && s_send_wait_us < next)
            next = s_send_wait_us;
        bool again = s_run_pending;
        portEXIT_CRITICAL(&s_trig);

        xSemaphoreTake(s_st_lock, portMAX_DELAY);
        s_next_due_us = s_status.state == DDNS_ST_DISABLED ? 0 : next;
        xSemaphoreGive(s_st_lock);
        if (again)
            continue;
        int64_t ms = (next - now) / 1000;
        if (ms <= 0)
            continue;
        if (ms > MAX_WAIT_MS)
            ms = MAX_WAIT_MS;
        xTaskNotifyWait(0, ULONG_MAX, NULL, pdMS_TO_TICKS((uint32_t)ms));
    }
}

static void start_task(void) {
    if (s_task)
        return;
    if (xTaskCreateWithCaps(ddns_task, "ddns", TASK_STACK, NULL, TASK_PRIO, &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "task create failed");
    }
}

/* ---- triggers ---- */

static void uplink_trigger(bool got_ip) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_trig);
    s_epoch++;
    if (got_ip && s_wan_trigger)
        s_wanup_at_us = now + WAN_DELAY_US;
    portEXIT_CRITICAL(&s_trig);
    wake_task();
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)data;
    uplink_trigger(id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_ETH_GOT_IP);
}

void ddns_notify_uplink_changed(void) {
    if (!s_lock)
        return;
    uplink_trigger(s_plat.uplink_up && s_plat.uplink_up());
}

/* ---- SNMP (NET-SNMP-EXTEND-MIB via snmp_agent callback) ---- */

static void fmt_ip(char *out, size_t sz, uint32_t ip) {
    snprintf(out, sz, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16) & 255, (unsigned)(ip >> 8) & 255,
             (unsigned)ip & 255);
}

/* nsExtendResult: 0 ok, 1 disabled, 2 waiting, 3 quorum-failed, 4 provider-error, 5 cooldown, 6 stopped. */
static void ddns_snmp_fill(snmp_extend_snapshot_t *out) {
    static const struct {
        const char *word;
        int32_t result;
    } map[] = {
        [DDNS_ST_DISABLED] = {"disabled", 1},
        [DDNS_ST_WAITING] = {"waiting", 2},
        [DDNS_ST_OK] = {"ok", 0},
        [DDNS_ST_QUORUM_FAILED] = {"quorum-failed", 3},
        [DDNS_ST_PROVIDER_ERROR] = {"provider-error", 4},
        [DDNS_ST_COOLDOWN] = {"cooldown", 5},
        [DDNS_ST_STOPPED] = {"stopped", 6},
    };
    ddns_status_t st;
    ddns_get_status(&st);
    unsigned k = (unsigned)st.state < sizeof map / sizeof map[0] ? (unsigned)st.state : DDNS_ST_DISABLED;
    char ip[16];
    const char *arg = st.detail;
    if (st.state == DDNS_ST_OK) {
        fmt_ip(ip, sizeof ip, st.quorum_ip);
        arg = ip;
    } else if (st.state == DDNS_ST_DISABLED || st.state == DDNS_ST_QUORUM_FAILED) {
        arg = "";
    }
    snprintf(out->line1, sizeof out->line1, "%s%s%s", map[k].word, arg[0] ? " " : "", arg);
    out->result = map[k].result;

    size_t n = 0;
    for (int i = 0; i < 3; i++) {
        char v[24];
        if (!st.src[i].asked)
            copy_str(v, sizeof v, "-");
        else if (st.src[i].ok)
            fmt_ip(v, sizeof v, st.src[i].ip);
        else
            copy_str(v, sizeof v, st.src[i].err[0] ? st.src[i].err : "error");
        n += (size_t)snprintf(out->full + n, sizeof out->full - n, "src%d %s\n", i + 1, v);
        if (n >= sizeof out->full)
            n = sizeof out->full - 1;
    }
    char okip[16] = "-", when[24] = "-";
    if (st.last_ok_ip)
        fmt_ip(okip, sizeof okip, st.last_ok_ip);
    if (st.last_ok_wall) {
        time_t t = (time_t)st.last_ok_wall;
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tm);
    }
    n += (size_t)snprintf(out->full + n, sizeof out->full - n, "last_ok %s %s\n", okip, when);
    if (n >= sizeof out->full)
        n = sizeof out->full - 1;
    snprintf(out->full + n, sizeof out->full - n, "last_http %d %s", st.last_http,
             st.last_error[0] ? st.last_error : "-");
    out->num_lines = 5;
}

/* ---- public API ---- */

void ddns_init(const ddns_platform_t *p) {
    if (s_lock)
        return;
    if (p)
        s_plat = *p;
    ddns_core_set_alloc(psram_alloc, free);
    s_cfg = heap_caps_calloc(1, sizeof *s_cfg, MALLOC_CAP_SPIRAM);
    s_rt = heap_caps_calloc(1, sizeof *s_rt, MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    s_st_lock = xSemaphoreCreateMutex();
    s_nvs_lock = xSemaphoreCreateMutex();
    if (!s_cfg || !s_rt || !s_lock || !s_st_lock || !s_nvs_lock) {
        ESP_LOGE(TAG, "init: out of memory");
        s_lock = NULL;
        return;
    }
    ddns_cfg_defaults(s_cfg);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = 0;
        bool loaded = nvs_get_blob(h, NVS_KEY_CFG, NULL, &sz) == ESP_OK && sz == sizeof *s_cfg &&
                      nvs_get_blob(h, NVS_KEY_CFG, s_cfg, &sz) == ESP_OK && s_cfg->version == DDNS_CFG_VERSION;
        if (loaded) {
            char msg[64];
            if (ddns_cfg_validate(s_cfg, msg, sizeof msg) != DDNS_OK) {
                ESP_LOGW(TAG, "stored settings invalid (%s), DDNS disabled", msg);
                loaded = false;
            }
        }
        if (!loaded) {
            ddns_wipe(s_cfg, sizeof *s_cfg);
            ddns_cfg_defaults(s_cfg);
        }
        char stop[sizeof s_sched.stop_reason + 8];
        size_t n = sizeof stop;
        if (nvs_get_str(h, NVS_KEY_STOP, stop, &n) == ESP_OK)
            ddns_sched_apply_stored_stop(&s_sched, stop);
        nvs_close(h);
    }
    s_wan_trigger = s_cfg->wan_trigger;
    s_sched_view = s_sched;
    s_status.state = s_cfg->enabled ? DDNS_ST_WAITING : DDNS_ST_DISABLED;
    copy_str(s_status.detail, sizeof s_status.detail, s_cfg->enabled ? "starting" : "");
    if (!ddns_url_host(s_cfg->url, s_status.provider_host, sizeof s_status.provider_host))
        s_status.provider_host[0] = '\0';

    const esp_timer_create_args_t ta = {.callback = stop_timer_cb, .name = "ddns_stop"};
    if (esp_timer_create(&ta, &s_stop_timer) != ESP_OK)
        ESP_LOGE(TAG, "stop timer create failed");

    static const int32_t ids[] = {IP_EVENT_STA_GOT_IP, IP_EVENT_STA_LOST_IP, IP_EVENT_ETH_GOT_IP, IP_EVENT_ETH_LOST_IP};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++)
        esp_event_handler_instance_register(IP_EVENT, ids[i], ip_event, NULL, NULL);

    snmp_agent_set_ddns_source(ddns_snmp_fill);
    if (s_cfg->enabled)
        start_task();
}

void ddns_get_status(ddns_status_t *out) {
    memset(out, 0, sizeof *out);
    if (!s_lock) {
        out->state = DDNS_ST_DISABLED;
        return;
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_st_lock, portMAX_DELAY);
    *out = s_status;
    int64_t next = s_next_due_us;
    int64_t cool = s_sched_view.cooldown_until_us;
    xSemaphoreGive(s_st_lock);
    portENTER_CRITICAL(&s_trig);
    int64_t force_at = s_force_at_us;
    portEXIT_CRITICAL(&s_trig);
    out->next_check_s = next > now ? (uint32_t)((next - now + US_PER_S - 1) / US_PER_S) : 0;
    out->cooldown_s = cool > now ? (uint32_t)((cool - now + US_PER_S - 1) / US_PER_S) : 0;
    int64_t fw = force_at ? force_at + 60 * US_PER_S - now : 0;
    out->force_wait_s = fw > 0 ? (uint32_t)((fw + US_PER_S - 1) / US_PER_S) : 0;
}

void ddns_get_config(ddns_cfg_t *out, size_t *pass_len, size_t *token_len) {
    if (!s_lock) {
        ddns_cfg_defaults(out);
        if (pass_len)
            *pass_len = 0;
        if (token_len)
            *token_len = 0;
        return;
    }
    xSemaphoreTake(s_st_lock, portMAX_DELAY);
    memcpy(out, s_cfg, sizeof *out);
    xSemaphoreGive(s_st_lock);
    if (pass_len)
        *pass_len = strlen(out->pass);
    if (token_len)
        *token_len = strlen(out->token);
    ddns_wipe(out->pass, sizeof out->pass);
    ddns_wipe(out->token, sizeof out->token);
}

esp_err_t ddns_save_config(ddns_cfg_t *in, unsigned keep_flags, char *msg, size_t msg_sz) {
    esp_err_t ret = ESP_ERR_INVALID_ARG;
    if (msg && msg_sz)
        msg[0] = '\0';
    if (!in)
        return ret;
    if (!s_lock) {
        copy_str(msg, msg_sz, "DDNS not initialised");
        ret = ESP_ERR_INVALID_STATE;
        goto out;
    }
    in->version = DDNS_CFG_VERSION;
    if (keep_flags & (DDNS_KEEP_PASS | DDNS_KEEP_TOKEN)) {
        xSemaphoreTake(s_st_lock, portMAX_DELAY);
        if (keep_flags & DDNS_KEEP_PASS)
            memcpy(in->pass, s_cfg->pass, sizeof in->pass);
        if (keep_flags & DDNS_KEEP_TOKEN)
            memcpy(in->token, s_cfg->token, sizeof in->token);
        xSemaphoreGive(s_st_lock);
    }
    if (ddns_cfg_validate(in, msg, msg_sz) != DDNS_OK)
        goto out;

    xSemaphoreTake(s_lock, portMAX_DELAY); /* may wait for a dispatch, at most the request budget */
    xSemaphoreTake(s_nvs_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY_CFG, in, sizeof *in);
        if (err == ESP_OK) {
            esp_err_t e2 = nvs_erase_key(h, NVS_KEY_STOP);
            if (e2 != ESP_OK && e2 != ESP_ERR_NVS_NOT_FOUND)
                err = e2;
        }
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        xSemaphoreGive(s_nvs_lock);
        xSemaphoreGive(s_lock);
        copy_str(msg, msg_sz, "could not save (NVS)");
        ret = err;
        goto out;
    }
    s_stop_epoch++;
    xSemaphoreGive(s_nvs_lock);

    xSemaphoreTake(s_st_lock, portMAX_DELAY);
    memcpy(s_cfg, in, sizeof *s_cfg);
    if (!ddns_url_host(s_cfg->url, s_status.provider_host, sizeof s_status.provider_host))
        s_status.provider_host[0] = '\0';
    if (!s_cfg->enabled) {
        s_status.state = DDNS_ST_DISABLED;
        s_status.detail[0] = '\0';
    }
    xSemaphoreGive(s_st_lock);
    s_gen++;
    ddns_sched_on_config_saved(&s_sched);
    status_publish_sched();
    s_wan_trigger = in->wan_trigger;
    bool enabled = in->enabled;
    xSemaphoreGive(s_lock);

    portENTER_CRITICAL(&s_trig);
    s_run_pending = true;
    s_send_wait_us = 0;
    portEXIT_CRITICAL(&s_trig);
    if (enabled)
        start_task();
    wake_task();
    ret = ESP_OK;

out:
    ddns_wipe(in->pass, sizeof in->pass);
    ddns_wipe(in->token, sizeof in->token);
    return ret;
}

bool ddns_force(uint32_t *wait_s) {
    if (wait_s)
        *wait_s = 0;
    if (!s_lock)
        return false;
    int64_t now = esp_timer_get_time();
    bool ok = true;
    portENTER_CRITICAL(&s_trig);
    if (s_force_at_us && now - s_force_at_us < 60 * US_PER_S) {
        ok = false;
        if (wait_s)
            *wait_s = (uint32_t)((s_force_at_us + 60 * US_PER_S - now + US_PER_S - 1) / US_PER_S);
    } else {
        s_force_at_us = now;
        s_force_req = true;
        s_run_pending = true;
    }
    portEXIT_CRITICAL(&s_trig);
    if (ok)
        wake_task();
    return ok;
}
