/* DDNS HTTP transport. All network I/O is nonblocking; DNS uses the lwIP callback API.
 * A single absolute deadline covers resolution through response parsing. The resolved IPv4 is
 * reused for the connection, with the original hostname preserved for TLS SNI and HTTP Host.
 * SPDX-License-Identifier: MIT */
#include "ddns_http.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_parser.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/tcpip.h"

#define REQUEST_US 20000000LL
#define TX_CAP 2048
#define RX_CAP 512

/* A late DNS callback owns its reference even if the caller has timed out. In particular, neither
 * its hostname nor its semaphore can live on the caller's stack. */
typedef struct {
    atomic_uint refs;
    SemaphoreHandle_t done;
    char host[64];
    ip_addr_t ip;
    bool ok;
} lookup_t;

static void lookup_release(lookup_t *q) {
    if (atomic_fetch_sub(&q->refs, 1) == 1) {
        vSemaphoreDelete(q->done);
        free(q);
    }
}

static void lookup_done(const char *name, const ip_addr_t *ip, void *arg) {
    (void)name;
    lookup_t *q = arg;
    q->ok = ip && IP_IS_V4(ip);
    if (q->ok)
        q->ip = *ip;
    xSemaphoreGive(q->done);
    lookup_release(q);
}

/* Runs on tcpip_thread, as required when lwIP core locking is disabled. */
static void lookup_start(void *arg) {
    lookup_t *q = arg;
    ip_addr_t ip;
    err_t e = dns_gethostbyname_addrtype(q->host, &ip, lookup_done, q, LWIP_DNS_ADDRTYPE_IPV4);
    if (e != ERR_INPROGRESS)
        lookup_done(q->host, e == ERR_OK ? &ip : NULL, q);
}

static bool resolve(const char *host, int64_t deadline, uint32_t *ip) {
    if (ddns_ipv4_parse(host, strlen(host), ip))
        return true;
    lookup_t *q = calloc(1, sizeof *q);
    if (!q)
        return false;
    q->done = xSemaphoreCreateBinary();
    if (!q->done) {
        free(q);
        return false;
    }
    atomic_init(&q->refs, 2);
    strlcpy(q->host, host, sizeof q->host);
    if (tcpip_try_callback(lookup_start, q) != ERR_OK) {
        lookup_release(q);
        lookup_release(q);
        return false;
    }
    int64_t left_ms = (deadline - esp_timer_get_time()) / 1000;
    bool ok = left_ms > 0 && xSemaphoreTake(q->done, pdMS_TO_TICKS((uint32_t)left_ms)) == pdTRUE && q->ok;
    if (ok)
        *ip = ntohl(ip4_addr_get_u32(ip_2_ip4(&q->ip)));
    lookup_release(q);
    return ok;
}

typedef struct {
    char *body, *retry_after;
    size_t cap, len, ra_cap, ra_len;
    char key[32];
    size_t key_len;
    bool value, complete, capped;
} response_t;

static int on_message_begin(http_parser *p) {
    response_t *r = p->data;
    r->key_len = 0;
    r->value = false;
    r->ra_len = 0;
    if (r->retry_after && r->ra_cap)
        r->retry_after[0] = 0;
    return 0;
}

static int on_header_field(http_parser *p, const char *s, size_t n) {
    response_t *r = p->data;
    if (r->value) {
        r->key_len = 0;
        r->value = false;
    }
    for (size_t i = 0; i < n; i++) {
        if (r->key_len + 1 < sizeof r->key)
            r->key[r->key_len++] = s[i];
    }
    r->key[r->key_len] = 0;
    return 0;
}

static int on_header_value(http_parser *p, const char *s, size_t n) {
    response_t *r = p->data;
    bool retry = strcasecmp(r->key, "Retry-After") == 0;
    if (!r->value && retry) {
        r->ra_len = 0;
        if (r->retry_after && r->ra_cap)
            r->retry_after[0] = 0;
    }
    r->value = true;
    if (retry && r->retry_after && r->ra_cap) {
        size_t copy = n < r->ra_cap - 1 - r->ra_len ? n : r->ra_cap - 1 - r->ra_len;
        memcpy(r->retry_after + r->ra_len, s, copy);
        r->ra_len += copy;
        r->retry_after[r->ra_len] = 0;
    }
    return 0;
}

static int on_body(http_parser *p, const char *s, size_t n) {
    response_t *r = p->data;
    size_t copy = n < r->cap - r->len ? n : r->cap - r->len;
    memcpy(r->body + r->len, s, copy);
    r->len += copy;
    if (r->len == r->cap) {
        r->capped = true;
        http_parser_pause(p, 1);
    }
    return 0;
}

static int on_complete(http_parser *p) {
    response_t *r = p->data;
    /* Skip informational responses; only a final response can finish this exchange. */
    if (p->status_code >= 200) {
        r->complete = true;
        http_parser_pause(p, 1);
    }
    return 0;
}

static bool would_block(ssize_t n) {
    return n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE ||
           (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
}

static bool write_all(esp_tls_t *tls, const char *s, size_t len, int64_t deadline) {
    size_t sent = 0;
    while (sent < len && esp_timer_get_time() < deadline) {
        errno = 0;
        ssize_t n = esp_tls_conn_write(tls, s + sent, len - sent);
        if (n > 0)
            sent += (size_t)n;
        /* ESP-TLS returns plain-TCP success before a nonblocking connect finishes. lwIP's
         * send() reports EINPROGRESS until that connection completes; retry within the deadline. */
        else if (!would_block(n) && !(n < 0 && errno == EINPROGRESS))
            return false;
        else
            vTaskDelay(1);
    }
    return sent == len;
}

static bool append_header(char *tx, size_t *used, const char *name, const char *prefix, const char *value) {
    int n = snprintf(tx + *used, TX_CAP - *used, "%s: %s%s\r\n", name, prefix, value);
    if (n < 0 || (size_t)n >= TX_CAP - *used)
        return false;
    *used += (size_t)n;
    return true;
}

int ddns_http_exchange(const char *url, ddns_method_t method, const ddns_req_t *req, bool json_body, char *resp,
                       size_t resp_cap, size_t *resp_len, bool *timed_out, char *retry_after, size_t ra_sz,
                       void (*written)(void)) {
    int64_t deadline = esp_timer_get_time() + REQUEST_US;
    *resp_len = 0;
    *timed_out = false;
    if (retry_after && ra_sz)
        retry_after[0] = 0;
    int status = -1;
    esp_tls_t *tls = NULL;
    char *tx = NULL, *rx = NULL;
    struct http_parser_url u;
    http_parser_url_init(&u);
    if (http_parser_parse_url(url, strlen(url), 0, &u) != 0 || !(u.field_set & (1 << UF_HOST)))
        goto out;
    char host[64], addr[16], host_header[80];
    size_t host_len = u.field_data[UF_HOST].len;
    if (!host_len || host_len >= sizeof host)
        goto out;
    memcpy(host, url + u.field_data[UF_HOST].off, host_len);
    host[host_len] = 0;
    uint32_t ip;
    if (!resolve(host, deadline, &ip))
        goto out;
    if (!ddns_ipv4_is_public(ip)) {
        status = DDNS_HTTP_DEST_DENIED;
        goto out;
    }
    snprintf(addr, sizeof addr, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16) & 255,
             (unsigned)(ip >> 8) & 255, (unsigned)ip & 255);
    bool https = strncmp(url, "https://", 8) == 0;
    /* Save validation permits at most one HTTP source; the provider always requires HTTPS. */
    if (!https && strncmp(url, "http://", 7) != 0) // NOSONAR: opt-in plain-HTTP IP source.
        goto out;
    int port = u.field_set & (1 << UF_PORT) ? u.port : (https ? 443 : 80);
    if (u.field_set & (1 << UF_PORT))
        snprintf(host_header, sizeof host_header, "%s:%d", host, port);
    else
        strlcpy(host_header, host, sizeof host_header);
    esp_tls_cfg_t cfg = {.non_block = true,
                         .is_plain_tcp = !https,
                         .common_name = host,
                         .crt_bundle_attach = https ? esp_crt_bundle_attach : NULL};
    tls = esp_tls_init();
    if (!tls)
        goto out;
    int connected = 0;
    while (esp_timer_get_time() < deadline) {
        /* ESP-IDF 5.5's CONNECTING state reuses select()'s fd_sets. A short timeout clears
         * those sets and strands later polls, so the first select must get the remaining
         * budget. The TLS handshake and subsequent I/O still use the nonblocking socket. */
        int64_t remaining_us = deadline - esp_timer_get_time();
        if (remaining_us <= 0)
            break;
        cfg.timeout_ms = (int)((remaining_us + 999) / 1000);
        connected = esp_tls_conn_new_async(addr, strlen(addr), port, &cfg, tls);
        if (connected != 0)
            break;
        vTaskDelay(1);
    }
    if (connected != 1 || esp_timer_get_time() >= deadline)
        goto out;

    tx = heap_caps_calloc(1, TX_CAP, MALLOC_CAP_SPIRAM);
    rx = heap_caps_calloc(1, RX_CAP, MALLOC_CAP_SPIRAM);
    if (!tx || !rx)
        goto out;
    static const char *const methods[] = {"GET", "POST", "PUT", "PATCH"};
    if ((unsigned)method >= sizeof methods / sizeof methods[0])
        goto out;
    const char *path = u.field_set & (1 << UF_PATH) ? url + u.field_data[UF_PATH].off : "/";
    int path_len = u.field_set & (1 << UF_PATH) ? u.field_data[UF_PATH].len : 1;
    const char *query = u.field_set & (1 << UF_QUERY) ? url + u.field_data[UF_QUERY].off : "";
    int query_len = u.field_set & (1 << UF_QUERY) ? u.field_data[UF_QUERY].len : 0;
    size_t body_len = req ? strlen(req->body) : 0;
    bool extra = req && req->has_header;
    bool custom_type = extra && strcasecmp(req->hdr_name, "Content-Type") == 0;
    bool custom_agent = extra && strcasecmp(req->hdr_name, "User-Agent") == 0;
    int n = snprintf(tx, TX_CAP, "%s %.*s%s%.*s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nContent-Length: %u\r\n",
                     methods[method], path_len, path, query_len ? "?" : "", query_len, query, host_header,
                     (unsigned)body_len);
    if (n < 0 || n >= TX_CAP)
        goto out;
    size_t used = (size_t)n;
    if (!custom_agent && !append_header(tx, &used, "User-Agent", "esp32-tsr-ddns/", esp_app_get_description()->version))
        goto out;
    if (body_len && !custom_type &&
        !append_header(tx, &used, "Content-Type", "",
                       json_body ? "application/json" : "application/x-www-form-urlencoded"))
        goto out;
    if (req && req->auth[0] && !append_header(tx, &used, "Authorization", "", req->auth))
        goto out;
    if (extra && !append_header(tx, &used, req->hdr_name, "", req->hdr_value))
        goto out;
    if (used + 2 >= TX_CAP)
        goto out;
    memcpy(tx + used, "\r\n", 3);
    used += 2;
    if (!write_all(tls, tx, used, deadline) || (body_len && !write_all(tls, req->body, body_len, deadline)))
        goto out;
    if (written) {
        written();
        written = NULL;
    }

    response_t r = {.body = resp, .cap = resp_cap, .retry_after = retry_after, .ra_cap = ra_sz};
    http_parser parser;
    http_parser_init(&parser, HTTP_RESPONSE);
    parser.data = &r;
    const http_parser_settings settings = {.on_message_begin = on_message_begin,
                                           .on_header_field = on_header_field,
                                           .on_header_value = on_header_value,
                                           .on_body = on_body,
                                           .on_message_complete = on_complete};
    while (!r.complete && !r.capped && esp_timer_get_time() < deadline) {
        errno = 0;
        ssize_t got = esp_tls_conn_read(tls, rx, RX_CAP);
        if (got < 0 && would_block(got)) {
            vTaskDelay(1);
            continue;
        }
        if (got < 0)
            break;
        http_parser_execute(&parser, &settings, rx, (size_t)got);
        if (got == 0 || HTTP_PARSER_ERRNO(&parser) != HPE_OK)
            break;
    }
    *resp_len = r.len;
    status = parser.status_code ? parser.status_code : -1;
    /* Preserve the bounded-prefix policy at the byte cap. A connection that ends before either
     * the complete response or the cap is ambiguous and must not count as success. */
    if (!r.complete && !r.capped)
        *timed_out = true;

out:
    if (esp_timer_get_time() >= deadline)
        *timed_out = true;
    if (written)
        written();
    if (tls)
        esp_tls_conn_destroy(tls);
    if (tx) {
        ddns_wipe(tx, TX_CAP);
        free(tx);
    }
    if (rx) {
        ddns_wipe(rx, RX_CAP);
        free(rx);
    }
    return status;
}
