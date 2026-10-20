/* Host fakes for the ESP-IDF/lwIP boundary; the production HTTP parser is used unchanged. */
#pragma once
#include <arpa/inet.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef struct {
    bool given;
} *SemaphoreHandle_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
#define MALLOC_CAP_SPIRAM 1
#define ESP_TLS_ERR_SSL_WANT_READ (-100)
#define ESP_TLS_ERR_SSL_WANT_WRITE (-101)
#define LWIP_DNS_ADDRTYPE_IPV4 0
#define ERR_INPROGRESS (-5)
#define ERR_OK 0
#define IP_IS_V4(ip) (true)
#define ip_2_ip4(ip) (ip)
#define ip4_addr_get_u32(ip) ((ip)->addr)
typedef int err_t;
typedef struct {
    uint32_t addr;
} ip_addr_t;
typedef void (*dns_found_callback)(const char *, const ip_addr_t *, void *);
typedef struct {
    int unused;
} esp_tls_t;
typedef struct {
    bool non_block, is_plain_tcp;
    int timeout_ms;
    const char *common_name;
    int (*crt_bundle_attach)(void *);
} esp_tls_cfg_t;
typedef struct {
    char version[32];
} esp_app_desc_t;
size_t strlcpy(char *, const char *, size_t);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreGive(SemaphoreHandle_t);
int xSemaphoreTake(SemaphoreHandle_t, uint32_t);
void vSemaphoreDelete(SemaphoreHandle_t);
void vTaskDelay(uint32_t);
int64_t esp_timer_get_time(void);
void *heap_caps_calloc(size_t, size_t, int);
int esp_crt_bundle_attach(void *);
const esp_app_desc_t *esp_app_get_description(void);
esp_tls_t *esp_tls_init(void);
int esp_tls_conn_destroy(esp_tls_t *);
int esp_tls_conn_new_async(const char *, int, int, const esp_tls_cfg_t *, esp_tls_t *);
ssize_t esp_tls_conn_write(esp_tls_t *, const void *, size_t);
ssize_t esp_tls_conn_read(esp_tls_t *, void *, size_t);
err_t tcpip_try_callback(void (*)(void *), void *);
err_t dns_gethostbyname_addrtype(const char *, ip_addr_t *, dns_found_callback, void *, uint8_t);
