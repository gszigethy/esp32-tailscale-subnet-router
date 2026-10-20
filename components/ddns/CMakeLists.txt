# Dynamic DNS client. ddns_core.c is pure C (host-tested in tools/ddns_host_test);
# ddns.c is the ESP-IDF runtime: task, HTTP exchange, NVS blob, events.
idf_component_register(SRCS "ddns.c" "ddns_core.c"
                       INCLUDE_DIRS "include" "."
                       PRIV_REQUIRES esp_http_client esp-tls mbedtls http_parser nvs_flash esp_event
                                     esp_netif esp_timer lwip freertos log esp_app_format snmp_agent)
