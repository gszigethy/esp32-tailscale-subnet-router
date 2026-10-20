/* Exercise the real DDNS transport and ESP-IDF HTTP parser with deterministic network fakes. */
#include "ddns_http.h"
#include "http_shim.h"
#include <assert.h>
#include <stdio.h>

static int64_t now_us, connect_at, read_at, byte_interval;
static const char *answer;
static size_t answer_pos, answer_step;
static char request[4096];
static size_t request_len;
static int writes_before_block, released, dns_mode, live_semaphores;
static bool stall_body;
static dns_found_callback pending_dns;
static void *pending_arg;
static int checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        assert(x);                                                                                                     \
    } while (0)

size_t strlcpy(char *out, const char *s, size_t cap) {
    size_t n = strlen(s);
    if (cap) {
        size_t copy = n < cap - 1 ? n : cap - 1;
        memcpy(out, s, copy);
        out[copy] = 0;
    }
    return n;
}
SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    live_semaphores++;
    return calloc(1, sizeof(struct { bool given; }));
}
int xSemaphoreGive(SemaphoreHandle_t s) {
    s->given = true;
    return 1;
}
int xSemaphoreTake(SemaphoreHandle_t s, uint32_t ticks) {
    if (!s->given)
        now_us += (int64_t)ticks * 1000;
    return s->given;
}
void vSemaphoreDelete(SemaphoreHandle_t s) {
    live_semaphores--;
    free(s);
}
void vTaskDelay(uint32_t ticks) { now_us += (int64_t)ticks * 1000; }
int64_t esp_timer_get_time(void) { return now_us; }
void *heap_caps_calloc(size_t n, size_t sz, int caps) {
    (void)caps;
    return calloc(n, sz);
}
int esp_crt_bundle_attach(void *p) {
    (void)p;
    return 0;
}
const esp_app_desc_t *esp_app_get_description(void) {
    static const esp_app_desc_t d = {"test"};
    return &d;
}
esp_tls_t *esp_tls_init(void) { return calloc(1, sizeof(esp_tls_t)); }
int esp_tls_conn_destroy(esp_tls_t *tls) {
    free(tls);
    return 0;
}
int esp_tls_conn_new_async(const char *host, int len, int port, const esp_tls_cfg_t *cfg, esp_tls_t *tls) {
    (void)len;
    (void)tls;
    assert(strcmp(host, "8.8.8.8") == 0); /* The validated IP is used, never a second DNS lookup. */
    assert(strcmp(cfg->common_name, "h.example") == 0 || strcmp(cfg->common_name, "8.8.8.8") == 0);
    assert(cfg->non_block && (port == 443 || port == 8443 || port == 80));
    return now_us >= connect_at;
}
ssize_t esp_tls_conn_write(esp_tls_t *tls, const void *buf, size_t len) {
    (void)tls;
    if (writes_before_block == 0)
        return ESP_TLS_ERR_SSL_WANT_WRITE;
    if (writes_before_block > 0)
        writes_before_block--;
    size_t copy = len > 7 ? 7 : len; /* Force partial writes, including headers and body. */
    assert(request_len + copy < sizeof request);
    memcpy(request + request_len, buf, copy);
    request_len += copy;
    request[request_len] = 0;
    return (ssize_t)copy;
}
ssize_t esp_tls_conn_read(esp_tls_t *tls, void *buf, size_t len) {
    (void)tls;
    if (now_us < read_at || (stall_body && answer_pos >= strlen(answer)))
        return ESP_TLS_ERR_SSL_WANT_READ;
    size_t left = strlen(answer) - answer_pos;
    size_t copy = left < len ? left : len;
    if (copy > answer_step)
        copy = answer_step;
    memcpy(buf, answer + answer_pos, copy);
    answer_pos += copy;
    read_at = now_us + byte_interval;
    return (ssize_t)copy;
}
err_t tcpip_try_callback(void (*cb)(void *), void *arg) {
    cb(arg);
    return ERR_OK;
}
err_t dns_gethostbyname_addrtype(const char *host, ip_addr_t *out, dns_found_callback cb, void *arg, uint8_t type) {
    (void)host;
    (void)type;
    if (dns_mode == 1) {
        pending_dns = cb;
        pending_arg = arg;
        return ERR_INPROGRESS;
    }
    out->addr = htonl(dns_mode == 2 ? 0xC0A80101u : 0x08080808u);
    return ERR_OK;
}
static void on_written(void) { released++; }
static void reset(const char *text) {
    now_us = 1;
    connect_at = 0;
    read_at = 0;
    byte_interval = 0;
    answer = text;
    answer_pos = 0;
    answer_step = 512;
    request_len = 0;
    request[0] = 0;
    writes_before_block = -1;
    released = 0;
    dns_mode = 0;
    stall_body = false;
    assert(!pending_dns && live_semaphores == 0);
}
static int exchange(const char *url, const ddns_req_t *req, char *resp, size_t cap, size_t *len, bool *timeout,
                    char *ra) {
    return ddns_http_exchange(url, req ? DDNS_M_POST : DDNS_M_GET, req, true, resp, cap, len, timeout, ra, 16,
                              on_written);
}
int main(void) {
    char resp[1024], ra[16];
    size_t len;
    bool timeout;
    reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK");
    CHECK(exchange("https://h.example/update?ip=8.8.8.8", NULL, resp, sizeof resp, &len, &timeout, ra) == 200);
    CHECK(!timeout && len == 2 && memcmp(resp, "OK", 2) == 0 && released == 1);
    CHECK(strstr(request, "GET /update?ip=8.8.8.8 HTTP/1.1\r\nHost: h.example\r\n") != NULL);
    CHECK(strstr(request, "Content-Length: 0\r\n") != NULL && strstr(request, "\r\n\r\n") != NULL);

    reset("HTTP/1.1 429 Slow\r\nRetry-After: 123\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nNO\r\n0\r\n\r\n");
    answer_step = 1; /* Header names and values split across reads. */
    ddns_req_t req = {0};
    strcpy(req.body, "{\"ip\":\"8.8.8.8\"}");
    strcpy(req.auth, "Basic dTpw");
    req.has_header = true;
    strcpy(req.hdr_name, "X-Token");
    strcpy(req.hdr_value, "secret");
    CHECK(exchange("https://h.example:8443/u", &req, resp, sizeof resp, &len, &timeout, ra) == 429);
    CHECK(!timeout && len == 2 && memcmp(resp, "NO", 2) == 0 && strcmp(ra, "123") == 0);
    CHECK(strstr(request, "Host: h.example:8443\r\n") != NULL);
    CHECK(strstr(request, "Content-Type: application/json\r\nAuthorization: Basic dTpw\r\nX-Token: "
                          "secret\r\n\r\n{\"ip\":\"8.8.8.8\"}") != NULL);
    CHECK(released == 1);

    reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK");
    strcpy(req.hdr_name, "Content-Type");
    strcpy(req.hdr_value, "application/custom");
    CHECK(exchange("https://h.example/u", &req, resp, sizeof resp, &len, &timeout, ra) == 200);
    CHECK(strstr(request, "Content-Type: application/custom\r\n") &&
          !strstr(request, "Content-Type: application/json"));

    reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK");
    strcpy(req.hdr_name, "User-Agent");
    strcpy(req.hdr_value, "custom-agent");
    CHECK(exchange("https://h.example/u", &req, resp, sizeof resp, &len, &timeout, ra) == 200);
    CHECK(strstr(request, "User-Agent: custom-agent\r\n") && !strstr(request, "User-Agent: esp32"));

    reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK");
    connect_at = 30000000;
    CHECK(exchange("https://h.example/", NULL, resp, sizeof resp, &len, &timeout, ra) < 0);
    CHECK(timeout && now_us <= 20001001 && request_len == 0 && released == 1);

    reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK");
    answer_step = 1;
    byte_interval = 9000000;
    CHECK(exchange("https://h.example/", NULL, resp, sizeof resp, &len, &timeout, ra) < 0);
    CHECK(timeout && now_us <= 20001001 && released == 1);

    reset("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nOK");
    stall_body = true;
    CHECK(exchange("https://h.example/", NULL, resp, sizeof resp, &len, &timeout, ra) == 200);
    CHECK(timeout && len == 2 && now_us <= 20001001 && released == 1);

    reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK");
    writes_before_block = 1;
    CHECK(exchange("https://h.example/", NULL, resp, sizeof resp, &len, &timeout, ra) < 0);
    CHECK(timeout && now_us <= 20001001 && released == 1);

    reset("");
    dns_mode = 1;
    CHECK(exchange("https://h.example/", NULL, resp, sizeof resp, &len, &timeout, ra) < 0);
    CHECK(timeout && released == 1 && live_semaphores == 1 && pending_dns);
    ip_addr_t late = {.addr = htonl(0x08080808u)};
    pending_dns("h.example", &late, pending_arg);
    pending_dns = NULL;
    pending_arg = NULL;
    CHECK(live_semaphores == 0); /* Late completion must be safe and release its allocation. */

    reset("");
    dns_mode = 2;
    CHECK(exchange("https://h.example/", NULL, resp, sizeof resp, &len, &timeout, ra) == DDNS_HTTP_DEST_DENIED);
    CHECK(!timeout && request_len == 0 && released == 1);

    reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK");
    CHECK(exchange("http://h.example/", NULL, resp, sizeof resp, &len, &timeout, ra) ==
          200); // NOSONAR: plain HTTP source fixture
    CHECK(!timeout && released == 1);

    reset("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nBAD");
    CHECK(exchange("https://h.example/", NULL, resp, 2, &len, &timeout, ra) == 200);
    CHECK(!timeout && len == 2 && released == 1); /* Classify the captured prefix at the byte cap. */

    reset("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nOK");
    CHECK(exchange("https://h.example/", NULL, resp, sizeof resp, &len, &timeout, ra) == 200);
    CHECK(timeout && len == 2 && released == 1);

    reset("HTTP/1.1 200 OK\r\n\r\nOK");
    CHECK(exchange("https://h.example/", NULL, resp, sizeof resp, &len, &timeout, ra) == 200);
    CHECK(!timeout && len == 2 && released == 1); /* A close-delimited body completes on EOF. */

    reset("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK");
    CHECK(exchange("https://h.example/", NULL, resp, sizeof resp, &len, &timeout, ra) == 200);
    CHECK(!timeout && len == 2 && released == 1);
    reset("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nOK");
    connect_at = 10000000;
    read_at = 19000000;
    stall_body = true;
    CHECK(exchange("https://h.example/", NULL, resp, sizeof resp, &len, &timeout, ra) == 200);
    CHECK(timeout && now_us <= 20001001 && released == 1); /* One budget shared across phases. */

    reset("HTTP/1.1 200 OK\r\nX-Header-Name-Longer-Than-The-Retry-After-Scratch-Buffer: ignored\r\nContent-Length: "
          "2\r\n\r\nOK");
    answer_step = 1;
    CHECK(exchange("https://h.example/", NULL, resp, sizeof resp, &len, &timeout, ra) == 200);
    CHECK(!timeout && len == 2 && released == 1);

    reset("HTTP/1.1 100 Continue\r\nRetry-After: 999\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK");
    CHECK(exchange("https://h.example/", NULL, resp, sizeof resp, &len, &timeout, ra) == 200);
    CHECK(!timeout && len == 2 && !ra[0] && released == 1);

    CHECK(live_semaphores == 0);
    printf("all HTTP transport tests passed (%d checks)\n", checks);
    return 0;
}
