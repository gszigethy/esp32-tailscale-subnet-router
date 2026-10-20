/* Host unit tests for the DDNS core. Build and run: make -C tools/ddns_host_test test
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "ddns_core.h"

static int s_fail, s_checks;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        s_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            s_fail++;                                                                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
        }                                                                                                              \
    } while (0)

#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static bool parse(const char *s, uint32_t *out) { return ddns_ipv4_parse(s, strlen(s), out); }

static void test_ipv4_parse(void) {
    uint32_t ip = 0;
    CHECK(parse("84.2.17.203", &ip) && ip == 0x540211CB);
    CHECK(parse("0.0.0.0", &ip) && ip == 0);
    CHECK(!parse("84.2.17", &ip));
    CHECK(!parse("84.2.17.203.1", &ip));
    CHECK(!parse("084.2.17.203", &ip));
    CHECK(!parse("256.1.1.1", &ip));
    CHECK(!parse("1.2.3.4 ", &ip));
    CHECK(!parse("+1.2.3.4", &ip));
    CHECK(!parse("", &ip));
    CHECK(!parse("1..2.3", &ip));
    CHECK(!parse("1.2.3.4444", &ip));
}

static void test_ipv4_public(void) {
    CHECK(ddns_ipv4_is_public(IP4(8, 8, 8, 8)));
    CHECK(ddns_ipv4_is_public(IP4(84, 2, 17, 203)));
    CHECK(!ddns_ipv4_is_public(IP4(10, 0, 0, 1)));
    CHECK(!ddns_ipv4_is_public(IP4(100, 64, 0, 1)));
    CHECK(!ddns_ipv4_is_public(IP4(100, 127, 255, 255)));
    CHECK(!ddns_ipv4_is_public(IP4(127, 0, 0, 1)));
    CHECK(!ddns_ipv4_is_public(IP4(169, 254, 1, 1)));
    CHECK(!ddns_ipv4_is_public(IP4(172, 31, 0, 1)));
    CHECK(!ddns_ipv4_is_public(IP4(192, 168, 1, 1)));
    CHECK(!ddns_ipv4_is_public(IP4(198, 18, 0, 1)));
    CHECK(!ddns_ipv4_is_public(IP4(203, 0, 113, 5)));
    CHECK(!ddns_ipv4_is_public(IP4(224, 0, 0, 1)));
    CHECK(!ddns_ipv4_is_public(IP4(255, 255, 255, 255)));
    CHECK(!ddns_ipv4_is_public(IP4(0, 0, 0, 0)));
    CHECK(ddns_ipv4_is_public(IP4(100, 63, 255, 255)));
    CHECK(ddns_ipv4_is_public(IP4(172, 32, 0, 1)));
}

static bool answer(int status, const char *s, size_t n, uint32_t *ip) { return ddns_parse_ip_answer(status, s, n, ip); }

static void test_ip_answer(void) {
    uint32_t ip = 0;
    CHECK(answer(200, "84.2.17.203\n", 12, &ip) && ip == IP4(84, 2, 17, 203));
    CHECK(answer(200, " \t84.2.17.203\r\n", 15, &ip) && ip == IP4(84, 2, 17, 203));
    CHECK(!answer(500, "84.2.17.203", 11, &ip));
    CHECK(!answer(200, "84.2.17.203\x00", 12, &ip));
    char big[65];
    memset(big, ' ', sizeof big);
    memcpy(big, "84.2.17.203", 11);
    CHECK(!answer(200, big, sizeof big, &ip));
    const char *html = "<html>84.2.17.203</html>";
    CHECK(!answer(200, html, strlen(html), &ip));
    CHECK(!answer(200, "192.168.1.1", 11, &ip));
    CHECK(!answer(200, "1.2.3.4\x1b", 8, &ip));
    CHECK(!answer(200, "", 0, &ip));
}

static void test_quorum(void) {
    uint32_t out = 0;
    {
        bool ok[3] = {true, true, true};
        uint32_t ip[3] = {1, 1, 2};
        CHECK(ddns_quorum(ok, ip, &out) == DDNS_Q_AGREE && out == 1);
    }
    {
        bool ok[3] = {true, true, true};
        uint32_t ip[3] = {1, 2, 2};
        CHECK(ddns_quorum(ok, ip, &out) == DDNS_Q_AGREE && out == 2);
    }
    {
        bool ok[3] = {false, true, true};
        uint32_t ip[3] = {0, 7, 7};
        CHECK(ddns_quorum(ok, ip, &out) == DDNS_Q_AGREE && out == 7);
    }
    {
        bool ok[3] = {true, true, true};
        uint32_t ip[3] = {1, 2, 3};
        CHECK(ddns_quorum(ok, ip, &out) == DDNS_Q_FAIL);
    }
    {
        bool ok[3] = {true, false, false};
        uint32_t ip[3] = {1, 1, 1};
        CHECK(ddns_quorum(ok, ip, &out) == DDNS_Q_FAIL);
    }
}

static void test_wipe(void) {
    char b[8] = "secret";
    ddns_wipe(b, sizeof b);
    CHECK(b[0] == 0 && b[5] == 0);
}

static ddns_cfg_t cfg_with(const char *url) {
    ddns_cfg_t c;
    ddns_cfg_defaults(&c);
    strcpy(c.url, url);
    return c;
}

static ddns_req_t s_req;

static void test_cfg_defaults(void) {
    ddns_cfg_t c;
    ddns_cfg_defaults(&c);
    CHECK(c.version == DDNS_CFG_VERSION && !c.enabled && c.wan_trigger && c.interval_min == 5 && c.refresh_days == 0);
    CHECK(c.method == DDNS_M_GET && c.body_type == DDNS_BODY_FORM);
    CHECK(strcmp(ddns_src_url(&c, 0), "https://api.ipify.org") == 0);
    CHECK(strcmp(ddns_src_url(&c, 1), "https://ipv4.icanhazip.com") == 0);
    CHECK(strcmp(ddns_src_url(&c, 2), "https://checkip.amazonaws.com") == 0);
    c.src[2].kind = DDNS_SRC_IFCONFIG;
    CHECK(strcmp(ddns_src_url(&c, 2), "https://ifconfig.me/ip") == 0);
    c.src[2].kind = DDNS_SRC_CUSTOM;
    strcpy(c.src[2].custom, "https://ip.example.net/");
    CHECK(strcmp(ddns_src_url(&c, 2), "https://ip.example.net/") == 0);
    CHECK(strlen(ddns_err_str(DDNS_E_HEADER)) > 0);
}

static void test_fill_duckdns(void) {
    ddns_cfg_t c = cfg_with("https://www.duckdns.org/update?domains=x&token={token}&ip={ip}");
    strcpy(c.token, "ab+c/d");
    CHECK(ddns_fill(&c, IP4(84, 2, 17, 203), &s_req) == DDNS_OK);
    CHECK(strcmp(s_req.url, "https://www.duckdns.org/update?domains=x&token=ab%2Bc%2Fd&ip=84.2.17.203") == 0);
    CHECK(s_req.auth[0] == 0 && !s_req.has_header && s_req.body[0] == 0);
}

static void test_fill_userinfo_basic(void) {
    ddns_cfg_t c = cfg_with("https://{user}:{pass}@members.dyndns.org/nic/update?hostname=h&myip={ip}"); // NOSONAR: placeholders
    strcpy(c.user, "u");
    strcpy(c.pass, "p:w");
    CHECK(ddns_fill(&c, IP4(84, 2, 17, 203), &s_req) == DDNS_OK);
    CHECK(strcmp(s_req.url, "https://members.dyndns.org/nic/update?hostname=h&myip=84.2.17.203") == 0);
    CHECK(strcmp(s_req.auth, "Basic dTpwOnc=") == 0);
}

static void test_fill_authorization_header_wins(void) {
    ddns_cfg_t c = cfg_with("https://api.godaddy.com/v1/domains/x/records/A/@");
    c.method = DDNS_M_PUT;
    c.body_type = DDNS_BODY_JSON;
    strcpy(c.body, "[{\"data\":\"{ip}\",\"ttl\":600}]");
    strcpy(c.header, "Authorization: sso-key {token}");
    strcpy(c.user, "u");
    strcpy(c.pass, "p");
    strcpy(c.token, "KEY:SECRET");
    CHECK(ddns_fill(&c, IP4(84, 2, 17, 203), &s_req) == DDNS_OK);
    CHECK(s_req.auth[0] == 0 && s_req.has_header);
    CHECK(strcmp(s_req.hdr_name, "Authorization") == 0);
    CHECK(strcmp(s_req.hdr_value, "sso-key KEY:SECRET") == 0);
    CHECK(strcmp(s_req.body, "[{\"data\":\"84.2.17.203\",\"ttl\":600}]") == 0);
}

static void test_fill_once(void) {
    ddns_cfg_t c = cfg_with("https://h.example/u?t={token}&ip={ip}");
    strcpy(c.token, "{ip}");
    CHECK(ddns_fill(&c, IP4(84, 2, 17, 203), &s_req) == DDNS_OK);
    CHECK(strcmp(s_req.url, "https://h.example/u?t=%7Bip%7D&ip=84.2.17.203") == 0);
}

static void test_fill_special_chars_all_contexts(void) {
    const char *pw = "a&b%c+d\"e\\f g:h@\xc3\xa9";
    const char *enc = "a%26b%25c%2Bd%22e%5Cf%20g%3Ah%40%C3%A9";
    char want[600];

    ddns_cfg_t c = cfg_with("https://h.example/u?p={pass}&ip={ip}");
    strcpy(c.user, "u");
    strcpy(c.pass, pw);
    CHECK(ddns_fill(&c, IP4(84, 2, 17, 203), &s_req) == DDNS_OK);
    snprintf(want, sizeof want, "https://h.example/u?p=%s&ip=84.2.17.203", enc);
    CHECK(strcmp(s_req.url, want) == 0);
    char raw[200], b64[300];
    snprintf(raw, sizeof raw, "u:%s", pw);
    ddns_base64((const uint8_t *)raw, strlen(raw), b64, sizeof b64);
    snprintf(want, sizeof want, "Basic %s", b64);
    CHECK(strcmp(s_req.auth, want) == 0);

    c = cfg_with("https://h.example/u");
    strcpy(c.pass, pw);
    c.method = DDNS_M_POST;
    strcpy(c.body, "p={pass}&ip={ip}");
    CHECK(ddns_fill(&c, IP4(84, 2, 17, 203), &s_req) == DDNS_OK);
    snprintf(want, sizeof want, "p=%s&ip=84.2.17.203", enc);
    CHECK(strcmp(s_req.body, want) == 0);

    c.body_type = DDNS_BODY_JSON;
    strcpy(c.body, "{\"p\":\"{pass}\"}");
    CHECK(ddns_fill(&c, IP4(84, 2, 17, 203), &s_req) == DDNS_OK);
    CHECK(strcmp(s_req.body, "{\"p\":\"a&b%c+d\\\"e\\\\f g:h@\xc3\xa9\"}") == 0);
    CHECK(ddns_json_valid(s_req.body));
}

static void test_fill_header_injection(void) {
    ddns_cfg_t c = cfg_with("https://h.example/u?ip={ip}");
    strcpy(c.header, "X-Auth: {token}");
    strcpy(c.token, "x\r\nX-Evil: 1");
    CHECK(ddns_fill(&c, IP4(84, 2, 17, 203), &s_req) == DDNS_E_HEADER);
    strcpy(c.header, "Host: {token}");
    strcpy(c.token, "evil.example");
    CHECK(ddns_fill(&c, IP4(84, 2, 17, 203), &s_req) == DDNS_E_HEADER);
    strcpy(c.header, "Bad Name: x");
    CHECK(ddns_fill(&c, IP4(84, 2, 17, 203), &s_req) == DDNS_E_HEADER);
}

static void test_fill_placeholder_in_host(void) {
    ddns_cfg_t c = cfg_with("https://{user}.example.com/?ip={ip}");
    strcpy(c.user, "x");
    CHECK(ddns_fill(&c, IP4(84, 2, 17, 203), &s_req) == DDNS_E_PLACEHOLDER);
    c = cfg_with("https://h.example:{token}/?ip={ip}");
    CHECK(ddns_fill(&c, IP4(84, 2, 17, 203), &s_req) == DDNS_E_PLACEHOLDER);
}

static void test_fill_unknown_placeholder(void) {
    ddns_cfg_t c = cfg_with("https://h.example/?ip={ipv6}");
    CHECK(ddns_fill(&c, IP4(84, 2, 17, 203), &s_req) == DDNS_E_PLACEHOLDER);
}

static void test_fill_json_placeholder_outside_string(void) {
    ddns_cfg_t c = cfg_with("https://h.example/");
    c.method = DDNS_M_PUT;
    c.body_type = DDNS_BODY_JSON;
    strcpy(c.body, "{\"ttl\":{ip}}");
    CHECK(ddns_fill(&c, IP4(84, 2, 17, 203), &s_req) == DDNS_E_PLACEHOLDER);
    strcpy(c.body, "{\"ip\":\"{ip}\",}");
    CHECK(ddns_fill(&c, IP4(84, 2, 17, 203), &s_req) == DDNS_E_JSON);
}

static void test_fill_caps(void) {
    char url[257];
    strcpy(url, "https://h.example/");
    while (strlen(url) < 256 - strlen("?t={token}"))
        strcat(url, "a");
    strcat(url, "?t={token}");
    ddns_cfg_t c = cfg_with(url);
    memset(c.token, '%', 128);
    c.token[128] = 0;
    c.src_ip = true;
    memset(&s_req, 0x55, sizeof s_req);
    CHECK(ddns_fill(&c, IP4(84, 2, 17, 203), &s_req) == DDNS_E_TOO_LONG);
    const unsigned char *p = (const unsigned char *)&s_req;
    bool zero = true;
    for (size_t i = 0; i < sizeof s_req; i++)
        if (p[i])
            zero = false;
    CHECK(zero);
}

static void test_base64_vectors(void) {
    const char *in[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char *want[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    char out[16];
    for (int i = 0; i < 7; i++) {
        size_t n = ddns_base64((const uint8_t *)in[i], strlen(in[i]), out, sizeof out);
        CHECK(n == strlen(want[i]) && strcmp(out, want[i]) == 0);
    }
    CHECK(ddns_base64((const uint8_t *)"foobar", 6, out, 8) == 0);
}

static void test_json_valid(void) {
    CHECK(ddns_json_valid("{\"a\":[1,-2.5e3,true,false,null,\"x\\u00e9\"]}"));
    CHECK(ddns_json_valid(" [ ] "));
    CHECK(!ddns_json_valid("{\"a\":1,}"));
    CHECK(!ddns_json_valid("{\"a\":01}"));
    CHECK(!ddns_json_valid("\"a\nb\""));
    CHECK(!ddns_json_valid("[[[[[[[[[1]]]]]]]]]"));
    CHECK(!ddns_json_valid("{} x"));
}

static ddns_err_t validate(ddns_cfg_t *c, char *msg) { return ddns_cfg_validate(c, msg, 128); }

/* Disabled with an empty template is the factory state; it must stay saveable so a config can be cleared. */
static void test_validate_accepts_disabled_empty(void) {
    ddns_cfg_t c = cfg_with("");
    char msg[128];
    c.enabled = false;
    CHECK(validate(&c, msg) == DDNS_OK);
    c.enabled = true;
    CHECK(validate(&c, msg) == DDNS_E_SCHEME);
    c.enabled = false;
    strcpy(c.header, "X-Api-Key: abc");
    CHECK(validate(&c, msg) == DDNS_E_SCHEME);
    c = cfg_with("");
    c.interval_min = 0;
    CHECK(validate(&c, msg) == DDNS_E_RANGE);
}

static void test_validate_moves_userinfo(void) {
    ddns_cfg_t c = cfg_with("https://bob:s%40crt@members.dyndns.org/nic/update?myip={ip}"); // NOSONAR: test fixture
    char msg[128];
    CHECK(validate(&c, msg) == DDNS_OK);
    CHECK(strcmp(c.user, "bob") == 0 && strcmp(c.pass, "s@crt") == 0);
    CHECK(strcmp(c.url, "https://{user}:{pass}@members.dyndns.org/nic/update?myip={ip}") == 0);
}

static void test_validate_rejects_literal_token(void) {
    ddns_cfg_t c = cfg_with("https://h.example/u?token=abc123&ip={ip}");
    char msg[128] = "";
    CHECK(validate(&c, msg) == DDNS_E_SECRET_LITERAL);
    CHECK(msg[0] && !strstr(msg, "abc123"));
    c = cfg_with("https://h.example/u?ApiKey=abc123&ip={ip}");
    CHECK(validate(&c, msg) == DDNS_E_SECRET_LITERAL);
    c = cfg_with("https://h.example/u");
    c.method = DDNS_M_POST;
    c.body_type = DDNS_BODY_JSON;
    strcpy(c.body, "{\"secretapikey\":\"abc\",\"ip\":\"{ip}\"}");
    CHECK(validate(&c, msg) == DDNS_E_SECRET_LITERAL);
    strcpy(c.body, "{\"secretapikey\":\"{token}\",\"ip\":\"{ip}\"}");
    CHECK(validate(&c, msg) == DDNS_OK);
    c.body_type = DDNS_BODY_FORM;
    strcpy(c.body, "password=hunter2&ip={ip}");
    CHECK(validate(&c, msg) == DDNS_E_SECRET_LITERAL);
    c = cfg_with("https://h.example/u?ip={ip}");
    strcpy(c.header, "X-Api-Key: abc");
    CHECK(validate(&c, msg) == DDNS_E_SECRET_LITERAL);
    strcpy(c.header, "X-Api-Key: {token}");
    CHECK(validate(&c, msg) == DDNS_OK);
}

static void test_validate_cloudns_q_literal(void) {
    ddns_cfg_t c = cfg_with("https://ipv4.cloudns.net/api/dynamicURL/?q=SECRET");
    c.src_ip = true;
    char msg[128];
    CHECK(validate(&c, msg) == DDNS_E_SECRET_LITERAL);
    c = cfg_with("https://ipv4.cloudns.net/api/dynamicURL/?q={token}");
    c.src_ip = true;
    CHECK(validate(&c, msg) == DDNS_OK);
    c = cfg_with("https://h.example/?Q=visible&ip={ip}");
    CHECK(validate(&c, msg) == DDNS_OK);
}

static void test_validate_http_provider(void) {
    ddns_cfg_t c = cfg_with("http://h.example/?ip={ip}");
    char msg[128];
    CHECK(validate(&c, msg) == DDNS_E_SCHEME);
}

static void test_validate_two_http_sources(void) {
    ddns_cfg_t c = cfg_with("https://h.example/?ip={ip}");
    char msg[128];
    c.src[1].kind = DDNS_SRC_CUSTOM;
    strcpy(c.src[1].custom, "http://ip1.example.net/");
    CHECK(validate(&c, msg) == DDNS_OK);
    c.src[2].kind = DDNS_SRC_CUSTOM;
    strcpy(c.src[2].custom, "http://ip2.example.net/");
    CHECK(validate(&c, msg) == DDNS_E_SOURCES);
}

static void test_validate_duplicate_sources(void) {
    ddns_cfg_t c = cfg_with("https://h.example/?ip={ip}");
    char msg[128];
    c.src[2].kind = DDNS_SRC_CUSTOM;
    strcpy(c.src[2].custom, "https://api.ipify.org");
    CHECK(validate(&c, msg) == DDNS_E_SOURCES);
    c.src[2].kind = DDNS_SRC_IPIFY;
    CHECK(validate(&c, msg) == DDNS_E_SOURCES);
    c.src[2].kind = DDNS_SRC_CUSTOM;
    strcpy(c.src[2].custom, "ftp://ip.example.net/");
    CHECK(validate(&c, msg) == DDNS_E_SOURCES);
    c.src[2].custom[0] = 0;
    CHECK(validate(&c, msg) == DDNS_E_SOURCES);
}

static void test_validate_private_custom_source(void) {
    ddns_cfg_t c = cfg_with("https://h.example/?ip={ip}");
    char msg[128];
    c.src[2].kind = DDNS_SRC_CUSTOM;
    strcpy(c.src[2].custom, "http://192.168.1.1/ip"); // NOSONAR: must be rejected
    CHECK(validate(&c, msg) == DDNS_E_SOURCES);
    strcpy(c.src[2].custom, "https://100.100.100.100/ip");
    CHECK(validate(&c, msg) == DDNS_E_SOURCES);
    strcpy(c.src[2].custom, "https://1.1.1.1/cdn-cgi/trace");
    CHECK(validate(&c, msg) == DDNS_OK);
}

static void test_validate_no_ip(void) {
    ddns_cfg_t c = cfg_with("https://h.example/update?host=x");
    char msg[128];
    CHECK(validate(&c, msg) == DDNS_E_NO_IP);
    c.src_ip = true;
    CHECK(validate(&c, msg) == DDNS_OK);
}

static void test_validate_body_with_get(void) {
    ddns_cfg_t c = cfg_with("https://h.example/?ip={ip}");
    char msg[128];
    strcpy(c.body, "a=b");
    CHECK(validate(&c, msg) == DDNS_E_BODY_WITH_GET);
}

static void test_validate_ranges(void) {
    ddns_cfg_t c = cfg_with("https://h.example/?ip={ip}");
    char msg[128];
    c.interval_min = 0;
    CHECK(validate(&c, msg) == DDNS_E_RANGE);
    c.interval_min = 1441;
    CHECK(validate(&c, msg) == DDNS_E_RANGE);
    c.interval_min = 1440;
    c.refresh_days = 366;
    CHECK(validate(&c, msg) == DDNS_E_RANGE);
    c.refresh_days = 365;
    CHECK(validate(&c, msg) == DDNS_OK);
    c.method = 4;
    CHECK(validate(&c, msg) == DDNS_E_RANGE);
}

static void test_validate_desec_example_ok(void) {
    ddns_cfg_t c = cfg_with("https://update.dedyn.io/?hostname=h.dedyn.io&myipv4={ip}&myipv6=preserve");
    strcpy(c.user, "h.dedyn.io");
    strcpy(c.pass, "tok");
    char msg[128];
    CHECK(validate(&c, msg) == DDNS_OK);
}

static void test_validate_dry_run_caps(void) {
    char url[257];
    strcpy(url, "https://h.example/");
    while (strlen(url) < 256 - strlen("?t={token}&ip={ip}"))
        strcat(url, "a");
    strcat(url, "?t={token}&ip={ip}");
    ddns_cfg_t c = cfg_with(url);
    memset(c.token, 'x', 128);
    c.token[128] = 0;
    char msg[128];
    CHECK(validate(&c, msg) == DDNS_OK); /* 'x' needs no encoding: 256 - 7 + 128 + 11 < 512 */
    c.header[0] = 0;
    strcpy(c.header, "Authorization: {token}{token}{token}{token}");
    CHECK(validate(&c, msg) == DDNS_E_TOO_LONG);
}

/* Save must reject requests that only overflow after real credentials are encoded. */
static void test_validate_encoded_secrets(void) {
    ddns_cfg_t c = cfg_with("https://h.example/?ip={ip}&token={token}");
    memset(c.token, '/', 128);
    c.token[128] = 0;
    c.method = DDNS_M_POST;
    strcpy(c.body, "a={token}&b={token}&c={token}");
    char msg[128];
    CHECK(validate(&c, msg) == DDNS_E_TOO_LONG);

    c.body_type = DDNS_BODY_JSON;
    memset(c.token, '\t', 128);
    strcpy(c.body, "{\"a\":\"{token}{token}\"}");
    CHECK(validate(&c, msg) == DDNS_E_TOO_LONG);

    c = cfg_with("https://h.example/?ip={ip}");
    strcpy(c.header, "Authorization: Bearer {token}");
    strcpy(c.token, "invalid\nheader");
    CHECK(validate(&c, msg) == DDNS_E_HEADER);

    c = cfg_with("https://h.example/?ip={ip}");
    c.method = DDNS_M_POST;
    for (int i = 0; i < 70; i++)
        strcat(c.body, "{ip}");
    CHECK(validate(&c, msg) == DDNS_E_TOO_LONG); /* A 15-character IPv4 must fit too. */

    c = cfg_with("https://h.example/?ip={ip}");
    /* A legal 128-byte unreserved token still fits the URL and remains accepted. */
    memset(c.token, 'x', 128);
    c.token[128] = 0;
    CHECK(validate(&c, msg) == DDNS_OK);
}

static void test_fingerprint(void) {
    ddns_cfg_t a = cfg_with("https://h.example/?ip={ip}");
    ddns_cfg_t b = a;
    uint64_t f = ddns_cfg_fingerprint(&a);
    CHECK(f == ddns_cfg_fingerprint(&b));
    strcpy(b.pass, "x");
    CHECK(f != ddns_cfg_fingerprint(&b));
    b = a;
    b.body_type = DDNS_BODY_JSON;
    CHECK(f != ddns_cfg_fingerprint(&b));
    b = a;
    b.src_ip = true;
    CHECK(f != ddns_cfg_fingerprint(&b));
    b = a;
    strcpy(b.match, "OK");
    b.interval_min = 9;
    CHECK(f == ddns_cfg_fingerprint(&b));
}

static void test_url_host(void) {
    char h[64];
    CHECK(ddns_url_host("https://{user}:{pass}@members.dyndns.org:443/x", h, sizeof h) &&
          strcmp(h, "members.dyndns.org") == 0);
    CHECK(ddns_url_host("https://api.ipify.org", h, sizeof h) && strcmp(h, "api.ipify.org") == 0);
    CHECK(ddns_url_host("http://h.example?x", h, sizeof h) && strcmp(h, "h.example") == 0);
    CHECK(!ddns_url_host("nourl", h, sizeof h));
    CHECK(!ddns_url_host("https:///path", h, sizeof h));
}

static void test_match(void) {
    CHECK(ddns_match("^good|^nochg", "good 1.2.3.4"));
    CHECK(ddns_match("^good|^nochg", "nochg 1.2.3.4"));
    CHECK(!ddns_match("^good", "notgood"));
    CHECK(ddns_match("OK", "OK"));
    CHECK(ddns_match("<ErrCount>0</ErrCount>", "...<ErrCount>0</ErrCount>..."));
    CHECK(ddns_match("", "anything"));
    CHECK(!ddns_match("ok", "OK"));
    CHECK(!ddns_match("a||b", "xyz"));
    CHECK(ddns_match("a||b", "xbz"));
}

static ddns_class_t cls(int st, const char *body, const char *match, const char *ra) {
    return ddns_classify(st, false, body, strlen(body), match, ra);
}

static void test_classify_order(void) {
    ddns_class_t r = cls(200, "badauth", "", NULL);
    CHECK(r.outcome == DDNS_OUT_STOPPED && strcmp(r.reason, "badauth") == 0);
    r = cls(401, "", "", NULL);
    CHECK(r.outcome == DDNS_OUT_STOPPED && strcmp(r.reason, "http 401") == 0);
    r = cls(429, "", "", "120");
    CHECK(r.outcome == DDNS_OUT_COOLDOWN && r.cooldown_s == 120 && strcmp(r.reason, "http 429") == 0);
    r = cls(429, "", "", "Wed, 21 Oct 2026 07:28:00 GMT");
    CHECK(r.outcome == DDNS_OUT_COOLDOWN && r.cooldown_s == 600);
    r = cls(429, "", "", NULL);
    CHECK(r.outcome == DDNS_OUT_COOLDOWN && r.cooldown_s == 600);
    r = cls(429, "", "", "999999");
    CHECK(r.outcome == DDNS_OUT_COOLDOWN && r.cooldown_s == 86400);
    r = cls(429, "", "", "99999999999999999999");
    CHECK(r.outcome == DDNS_OUT_COOLDOWN && r.cooldown_s == 86400);
    r = cls(200, "911", "", NULL);
    CHECK(r.outcome == DDNS_OUT_COOLDOWN && r.cooldown_s == 1800);
    r = cls(200, "dnserr\n", "^good", NULL);
    CHECK(r.outcome == DDNS_OUT_COOLDOWN && r.cooldown_s == 1800);
    r = cls(302, "", "", NULL);
    CHECK(r.outcome == DDNS_OUT_REDIRECT && strcmp(r.reason, "redirect (3xx)") == 0);
    r = cls(200, "  good 1.2.3.4\r\n", "^good", NULL);
    CHECK(r.outcome == DDNS_OUT_SUCCESS && strcmp(r.reason, "good") == 0);
    r = cls(200, "nohost", "^good", NULL);
    CHECK(r.outcome == DDNS_OUT_STOPPED && strcmp(r.reason, "nohost") == 0);
    r = cls(200, "whatever", "^good", NULL);
    CHECK(r.outcome == DDNS_OUT_TEMP && strcmp(r.reason, "no match") == 0);
    r = cls(500, "", "", NULL);
    CHECK(r.outcome == DDNS_OUT_TEMP && strcmp(r.reason, "http 500") == 0);
    r = ddns_classify(200, true, "good", 4, "^good", NULL);
    CHECK(r.outcome == DDNS_OUT_TEMP && strcmp(r.reason, "timeout") == 0);
    r = cls(204, "", "", NULL);
    CHECK(r.outcome == DDNS_OUT_SUCCESS);
}

static void test_redact_encoded_forms(void) {
    char body[200];
    char b64[32];
    ddns_base64((const uint8_t *)"u:p&ss", 6, b64, sizeof b64);
    size_t n = 0;
    n += (size_t)snprintf(body + n, sizeof body - n, "a p&ss b p%%26ss c %s d TOK123 e", b64);
    body[n++] = '\0';
    body[n++] = '\x1b';
    memcpy(body + n, "end", 3);
    n += 3;
    char out[65];
    ddns_redact_snippet(body, n, "u", "p&ss", "TOK123", out, sizeof out);
    CHECK(strlen(out) <= 64);
    CHECK(!strstr(out, "p&ss") && !strstr(out, "p%26ss") && !strstr(out, b64) && !strstr(out, "TOK123"));
    CHECK(strstr(out, "a *** b *** c *** d *** e..end") != NULL);
    char longb[300];
    memset(longb, 'z', sizeof longb);
    memcpy(longb + 62, "TOK123", 6); /* straddles the 64-char cut */
    ddns_redact_snippet(longb, sizeof longb, "", "", "TOK123", out, sizeof out);
    CHECK(strlen(out) == 64 && !strstr(out, "TOK") && strstr(out, "**"));
}

static void test_redact_json_escaped(void) {
    /* {pass} in a JSON body is sent as p\"s\\s; a provider echoing the body must not leak it */
    const char *body = "{\"pw\":\"p\\\"s\\\\s\",\"t\":\"T\\\\K\"}";
    char out[65];
    ddns_redact_snippet(body, strlen(body), "u", "p\"s\\s", "T\\K", out, sizeof out);
    CHECK(!strstr(out, "p\\\"s") && !strstr(out, "T\\\\K"));
    CHECK(strcmp(out, "{\"pw\":\"***\",\"t\":\"***\"}") == 0);
}

#define SEC 1000000LL

static ddns_class_t outcome(ddns_outcome_t o, uint32_t cooldown) {
    ddns_class_t r;
    memset(&r, 0, sizeof r);
    r.outcome = o;
    r.cooldown_s = cooldown;
    strcpy(r.reason, o == DDNS_OUT_STOPPED ? "badauth" : "x");
    return r;
}

static void test_sched_first_boot_sends(void) {
    ddns_sched_t s;
    memset(&s, 0, sizeof s);
    CHECK(ddns_sched_want_send(&s, 1, 2, 1800000000, 0));
}

static void test_sched_nochange_no_send(void) {
    ddns_sched_t s;
    memset(&s, 0, sizeof s);
    ddns_class_t ok = outcome(DDNS_OUT_SUCCESS, 0);
    ddns_sched_on_result(&s, &ok, true, 100 * SEC, 1800000000, 7, 9, 5);
    CHECK(!ddns_sched_want_send(&s, 7, 9, 1800000100, 0));
    CHECK(ddns_sched_want_send(&s, 8, 9, 1800000100, 0));
    CHECK(ddns_sched_want_send(&s, 7, 10, 1800000100, 0));
}

static void test_sched_refresh_due(void) {
    ddns_sched_t s;
    memset(&s, 0, sizeof s);
    ddns_class_t ok = outcome(DDNS_OUT_SUCCESS, 0);
    ddns_sched_on_result(&s, &ok, true, 100 * SEC, 1800000000, 7, 9, 5);
    CHECK(!ddns_sched_want_send(&s, 7, 9, 1800000000 + 86399, 1));
    CHECK(ddns_sched_want_send(&s, 7, 9, 1800000000 + 86400, 1));
    CHECK(!ddns_sched_want_send(&s, 7, 9, 1800000000 + 86400 * 5, 0));
}

static void test_sched_refresh_clock_backwards(void) {
    ddns_sched_t s;
    memset(&s, 0, sizeof s);
    s.have_ok = true;
    s.ok_ip = 7;
    s.ok_fp = 9;
    s.ok_wall = 2000000000;
    CHECK(!ddns_sched_want_send(&s, 7, 9, 1800000000, 1));
    s.ok_wall = 1800000000;
    CHECK(ddns_sched_want_send(&s, 7, 9, 1800000000 + 86400, 1));
}

static void test_sched_refresh_after_future_success(void) {
    /* success stamped while the clock ran ahead; after the correction refresh is due N days later */
    ddns_sched_t s;
    memset(&s, 0, sizeof s);
    s.have_ok = true;
    s.ok_ip = 7;
    s.ok_fp = 9;
    s.ok_wall = 2000000000;
    CHECK(!ddns_sched_want_send(&s, 7, 9, 1800000000, 1));
    CHECK(ddns_sched_want_send(&s, 7, 9, 1800000000 + 86400, 1));
}

static void test_sched_uplink_moved_after_send(void) {
    /* the uplink changed while the request was in flight: the success must not count */
    ddns_sched_t s;
    memset(&s, 0, sizeof s);
    ddns_class_t ok = outcome(DDNS_OUT_SUCCESS, 0);
    ddns_sched_on_result(&s, &ok, true, 100 * SEC, 1800000000, 7, 9, 5);
    CHECK(!ddns_sched_want_send(&s, 7, 9, 1800000000, 0));
    ddns_sched_on_uplink_moved(&s);
    CHECK(ddns_sched_want_send(&s, 7, 9, 1800000000, 0));
}

static void test_sched_force_clears_stored_stop(void) {
    ddns_sched_t s;
    memset(&s, 0, sizeof s);
    uint32_t w;
    CHECK(ddns_sched_try_force(&s, 100 * SEC, &w));
    CHECK(!s.stop_clear_pending);
    memset(&s, 0, sizeof s);
    ddns_sched_apply_stored_stop(&s, "badauth");
    CHECK(ddns_sched_try_force(&s, 100 * SEC, &w));
    CHECK(s.stop_clear_pending && !s.stopped);
}

static void test_sched_60s_limit(void) {
    ddns_sched_t s;
    memset(&s, 0, sizeof s);
    CHECK(ddns_sched_provider_ready_us(&s) == 0);
    int64_t t = 1000 * SEC;
    ddns_sched_on_attempt(&s, t);
    CHECK(ddns_sched_provider_ready_us(&s) == t + 60 * SEC);
    CHECK(!ddns_sched_may_dispatch(&s, t + 59 * SEC, 1, 1, 1, 1));
    CHECK(ddns_sched_may_dispatch(&s, t + 60 * SEC, 1, 1, 1, 1));
}

static void test_sched_cooldown_blocks_force(void) {
    ddns_sched_t s;
    memset(&s, 0, sizeof s);
    int64_t t = 1000 * SEC;
    ddns_sched_on_attempt(&s, t);
    ddns_class_t c = outcome(DDNS_OUT_COOLDOWN, 120);
    ddns_sched_on_result(&s, &c, true, t, 1800000000, 7, 9, 5);
    uint32_t w = 0;
    CHECK(ddns_sched_try_force(&s, t + SEC, &w));
    CHECK(ddns_sched_want_send(&s, 7, 9, 1800000001, 0));
    CHECK(!ddns_sched_may_dispatch(&s, t + 119 * SEC, 1, 1, 1, 1));
    CHECK(ddns_sched_may_dispatch(&s, t + 120 * SEC, 1, 1, 1, 1));
    CHECK(s.force_pending);
}

static void test_sched_force_rate(void) {
    ddns_sched_t s;
    memset(&s, 0, sizeof s);
    uint32_t w = 0;
    CHECK(ddns_sched_try_force(&s, 500 * SEC, &w));
    CHECK(!ddns_sched_try_force(&s, 520 * SEC, &w) && w == 40);
    CHECK(!ddns_sched_try_force(&s, 559 * SEC + 1, &w) && w == 1);
    CHECK(ddns_sched_try_force(&s, 560 * SEC, &w));
}

static void test_sched_backoff_doubles(void) {
    ddns_sched_t s;
    memset(&s, 0, sizeof s);
    ddns_class_t t = outcome(DDNS_OUT_TEMP, 0);
    const uint32_t want[] = {300, 600, 1200, 2400, 3600, 3600};
    for (int i = 0; i < 6; i++) {
        ddns_sched_on_result(&s, &t, true, 1000 * SEC, 1800000000, 7, 9, 5);
        CHECK(s.backoff_s == want[i]);
        CHECK(s.backoff_until_us == 1000 * SEC + (int64_t)want[i] * SEC);
    }
    ddns_class_t ok = outcome(DDNS_OUT_SUCCESS, 0);
    ddns_sched_on_result(&s, &ok, true, 2000 * SEC, 1800000000, 7, 9, 5);
    CHECK(s.backoff_s == 0 && s.backoff_until_us == 0);
    memset(&s, 0, sizeof s);
    ddns_class_t r = outcome(DDNS_OUT_REDIRECT, 0);
    ddns_sched_on_result(&s, &r, true, 0, 1800000000, 7, 9, 1);
    CHECK(s.backoff_s == 60);
    memset(&s, 0, sizeof s);
    ddns_sched_on_result(&s, &r, true, 0, 1800000000, 7, 9, 1440);
    CHECK(s.backoff_s == 3600);
}

static void test_sched_stopped_needs_force_or_save(void) {
    ddns_sched_t s;
    memset(&s, 0, sizeof s);
    ddns_class_t st = outcome(DDNS_OUT_STOPPED, 0);
    ddns_sched_on_result(&s, &st, true, 100 * SEC, 1800000000, 7, 9, 5);
    CHECK(s.stopped && strcmp(s.stop_reason, "badauth") == 0);
    CHECK(!ddns_sched_want_send(&s, 8, 9, 1800000000, 0));
    ddns_sched_on_config_saved(&s);
    CHECK(!s.stopped && ddns_sched_want_send(&s, 8, 9, 1800000000, 0));
    ddns_sched_on_result(&s, &st, true, 200 * SEC, 1800000000, 7, 9, 5);
    CHECK(!ddns_sched_want_send(&s, 8, 9, 1800000000, 0));
    uint32_t w;
    CHECK(ddns_sched_try_force(&s, 300 * SEC, &w));
    CHECK(!s.stopped && ddns_sched_want_send(&s, 8, 9, 1800000000, 0));
}

static void test_sched_epoch_change_blocks_send(void) {
    ddns_sched_t s;
    memset(&s, 0, sizeof s);
    CHECK(!ddns_sched_may_dispatch(&s, 100 * SEC, 3, 3, 4, 5));
    CHECK(!ddns_sched_may_dispatch(&s, 100 * SEC, 3, 4, 5, 5));
    CHECK(ddns_sched_may_dispatch(&s, 100 * SEC, 3, 3, 5, 5));
}

static void test_sched_stale_generation_still_records_cooldown(void) {
    ddns_sched_t s;
    memset(&s, 0, sizeof s);
    ddns_class_t c = outcome(DDNS_OUT_COOLDOWN, 1800);
    ddns_sched_on_result(&s, &c, false, 100 * SEC, 1800000000, 7, 9, 5);
    CHECK(s.cooldown_until_us == 1900 * SEC);
    ddns_class_t ok = outcome(DDNS_OUT_SUCCESS, 0);
    ddns_sched_on_result(&s, &ok, false, 100 * SEC, 1800000000, 7, 9, 5);
    CHECK(!s.have_ok);
    ddns_class_t st = outcome(DDNS_OUT_STOPPED, 0);
    ddns_sched_on_result(&s, &st, false, 100 * SEC, 1800000000, 7, 9, 5);
    CHECK(!s.stopped);
}

static void test_sched_config_saved_keeps_limits(void) {
    ddns_sched_t s;
    memset(&s, 0, sizeof s);
    ddns_sched_on_attempt(&s, 100 * SEC);
    ddns_class_t c = outcome(DDNS_OUT_COOLDOWN, 600);
    ddns_sched_on_result(&s, &c, true, 100 * SEC, 1800000000, 7, 9, 5);
    ddns_class_t t = outcome(DDNS_OUT_TEMP, 0);
    ddns_sched_on_result(&s, &t, true, 100 * SEC, 1800000000, 7, 9, 5);
    s.force_pending = true;
    ddns_sched_on_config_saved(&s);
    CHECK(s.cooldown_until_us == 700 * SEC && s.last_attempt_us == 100 * SEC);
    CHECK(s.backoff_s == 0 && s.backoff_until_us == 0 && !s.force_pending);
}

static void test_stop_reason_survives_reload(void) {
    ddns_sched_t s;
    memset(&s, 0, sizeof s);
    ddns_sched_apply_stored_stop(&s, "");
    ddns_sched_apply_stored_stop(&s, NULL);
    CHECK(!s.stopped);
    ddns_sched_apply_stored_stop(&s, "badauth");
    CHECK(s.stopped && strcmp(s.stop_reason, "badauth") == 0);
    CHECK(!ddns_sched_want_send(&s, 7, 9, 1800000000, 0));
    uint32_t w;
    CHECK(ddns_sched_try_force(&s, 100 * SEC, &w));
    CHECK(ddns_sched_want_send(&s, 7, 9, 1800000000, 0));
    memset(&s, 0, sizeof s);
    ddns_sched_apply_stored_stop(&s, "abcdefghijklmnopqrstuvwxyz0123");
    CHECK(strlen(s.stop_reason) == 23 && strncmp(s.stop_reason, "abcdefghijklmnopqrstuvw", 23) == 0);
}

int main(void) {
    test_ipv4_parse();
    test_ipv4_public();
    test_ip_answer();
    test_quorum();
    test_wipe();
    test_cfg_defaults();
    test_fill_duckdns();
    test_fill_userinfo_basic();
    test_fill_authorization_header_wins();
    test_fill_once();
    test_fill_special_chars_all_contexts();
    test_fill_header_injection();
    test_fill_placeholder_in_host();
    test_fill_unknown_placeholder();
    test_fill_json_placeholder_outside_string();
    test_fill_caps();
    test_base64_vectors();
    test_json_valid();
    test_validate_accepts_disabled_empty();
    test_validate_moves_userinfo();
    test_validate_rejects_literal_token();
    test_validate_cloudns_q_literal();
    test_validate_http_provider();
    test_validate_two_http_sources();
    test_validate_duplicate_sources();
    test_validate_private_custom_source();
    test_validate_no_ip();
    test_validate_body_with_get();
    test_validate_ranges();
    test_validate_desec_example_ok();
    test_validate_dry_run_caps();
    test_validate_encoded_secrets();
    test_fingerprint();
    test_url_host();
    test_match();
    test_classify_order();
    test_redact_encoded_forms();
    test_redact_json_escaped();
    test_sched_first_boot_sends();
    test_sched_nochange_no_send();
    test_sched_refresh_due();
    test_sched_refresh_clock_backwards();
    test_sched_refresh_after_future_success();
    test_sched_uplink_moved_after_send();
    test_sched_force_clears_stored_stop();
    test_sched_60s_limit();
    test_sched_cooldown_blocks_force();
    test_sched_force_rate();
    test_sched_backoff_doubles();
    test_sched_stopped_needs_force_or_save();
    test_sched_epoch_change_blocks_send();
    test_sched_stale_generation_still_records_cooldown();
    test_sched_config_saved_keeps_limits();
    test_stop_reason_survives_reload();
    if (s_fail) {
        printf("%d of %d checks failed\n", s_fail, s_checks);
        return 1;
    }
    printf("all tests passed (%d checks)\n", s_checks);
    return 0;
}
