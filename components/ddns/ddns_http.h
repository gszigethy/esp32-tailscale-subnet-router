/* Bounded, single-request HTTP transport for DDNS. SPDX-License-Identifier: MIT */
#pragma once
#include "ddns_core.h"

#define DDNS_HTTP_DEST_DENIED (-2)

/* Releases the caller's dispatch lock once the request has been written, or on failure.
 * DNS, connection, TLS, headers and body share one 20-second monotonic deadline. */
int ddns_http_exchange(const char *url, ddns_method_t method, const ddns_req_t *req, bool json_body, char *resp,
                       size_t resp_cap, size_t *resp_len, bool *timed_out, char *retry_after, size_t ra_sz,
                       void (*written)(void));
