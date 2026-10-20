/* DDNS core — see ddns_core.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ddns_core.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* IPv4 rules                                                          */
/* ------------------------------------------------------------------ */

bool ddns_ipv4_parse(const char *s, size_t n, uint32_t *out) {
    uint32_t ip = 0;
    size_t i = 0;
    for (int octet = 0; octet < 4; octet++) {
        if (octet > 0) {
            if (i >= n || s[i] != '.')
                return false;
            i++;
        }
        size_t start = i;
        uint32_t v = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9' && i - start < 3) {
            v = v * 10 + (uint32_t)(s[i] - '0');
            i++;
        }
        size_t digits = i - start;
        if (digits == 0 || v > 255)
            return false;
        if (digits > 1 && s[start] == '0')
            return false;
        ip = (ip << 8) | v;
    }
    if (i != n)
        return false;
    *out = ip;
    return true;
}

static bool in_net(uint32_t ip, uint32_t net, int prefix) {
    uint32_t mask = prefix == 0 ? 0 : 0xFFFFFFFFu << (32 - prefix);
    return (ip & mask) == net;
}

bool ddns_ipv4_is_public(uint32_t ip) {
    static const struct {
        uint32_t net;
        uint8_t prefix;
    } blocked[] = {
        {0x00000000u, 8},  /* 0/8 this network */
        {0x0A000000u, 8},  /* 10/8 */
        {0x64400000u, 10}, /* 100.64/10 CGNAT + tailnet */
        {0x7F000000u, 8},  /* 127/8 */
        {0xA9FE0000u, 16}, /* 169.254/16 */
        {0xAC100000u, 12}, /* 172.16/12 */
        {0xC0000000u, 24}, /* 192.0.0/24 */
        {0xC0000200u, 24}, /* 192.0.2/24 TEST-NET-1 */
        {0xC0586300u, 24}, /* 192.88.99/24 6to4 relay */
        {0xC0A80000u, 16}, /* 192.168/16 */
        {0xC6120000u, 15}, /* 198.18/15 benchmark */
        {0xC6336400u, 24}, /* 198.51.100/24 TEST-NET-2 */
        {0xCB007100u, 24}, /* 203.0.113/24 TEST-NET-3 */
        {0xE0000000u, 4},  /* 224/4 multicast */
        {0xF0000000u, 4},  /* 240/4 reserved + broadcast */
    };
    for (size_t i = 0; i < sizeof blocked / sizeof blocked[0]; i++)
        if (in_net(ip, blocked[i].net, blocked[i].prefix))
            return false;
    return true;
}

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool ddns_parse_ip_answer(int http_status, const char *body, size_t len, uint32_t *ip) {
    if (http_status < 200 || http_status > 299 || !body || len > 64)
        return false;
    size_t a = 0, b = len;
    while (a < b && is_ws(body[a]))
        a++;
    while (b > a && is_ws(body[b - 1]))
        b--;
    for (size_t i = a; i < b; i++) {
        unsigned char c = (unsigned char)body[i];
        if (c < 0x20 || c == 0x7f)
            return false;
    }
    uint32_t v;
    if (!ddns_ipv4_parse(body + a, b - a, &v) || !ddns_ipv4_is_public(v))
        return false;
    *ip = v;
    return true;
}

ddns_quorum_t ddns_quorum(const bool ok[3], const uint32_t ip[3], uint32_t *out) {
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++)
            if (ok[i] && ok[j] && ip[i] == ip[j]) {
                *out = ip[i];
                return DDNS_Q_AGREE;
            }
    return DDNS_Q_FAIL;
}

void ddns_wipe(void *p, size_t n) {
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--)
        *v++ = 0;
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

static const char *const k_src_presets[] = {
    "https://api.ipify.org",
    "https://ipv4.icanhazip.com",
    "https://checkip.amazonaws.com",
    "https://ifconfig.me/ip",
};

void ddns_cfg_defaults(ddns_cfg_t *c) {
    memset(c, 0, sizeof *c);
    c->version = DDNS_CFG_VERSION;
    c->method = DDNS_M_GET;
    c->body_type = DDNS_BODY_FORM;
    c->interval_min = 5;
    c->wan_trigger = true;
    c->src[0].kind = DDNS_SRC_IPIFY;
    c->src[1].kind = DDNS_SRC_ICANHAZIP;
    c->src[2].kind = DDNS_SRC_AWS;
}

const char *ddns_src_url(const ddns_cfg_t *c, int slot) {
    if (slot < 0 || slot > 2)
        return "";
    uint8_t k = c->src[slot].kind;
    if (k < DDNS_SRC_CUSTOM)
        return k_src_presets[k];
    return c->src[slot].custom;
}

const char *ddns_err_str(ddns_err_t e) {
    switch (e) {
    case DDNS_OK:
        return "ok";
    case DDNS_E_TOO_LONG:
        return "too long after filling in values";
    case DDNS_E_PLACEHOLDER:
        return "invalid placeholder or placeholder position";
    case DDNS_E_HEADER:
        return "invalid extra header";
    case DDNS_E_JSON:
        return "body is not valid JSON";
    case DDNS_E_URL:
        return "invalid URL";
    case DDNS_E_NO_IP:
        return "{ip} is missing (or enable 'provider uses the request's source IP')";
    case DDNS_E_SECRET_LITERAL:
        return "a secret is written literally; use {pass} or {token}";
    case DDNS_E_SCHEME:
        return "the update URL must use https://";
    case DDNS_E_SOURCES:
        return "invalid IP sources";
    case DDNS_E_RANGE:
        return "value out of range";
    case DDNS_E_BODY_WITH_GET:
        return "a body needs POST, PUT or PATCH";
    }
    return "error";
}

/* ------------------------------------------------------------------ */
/* Bounded string builder                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char *p;
    size_t cap; /* buffer size including the NUL */
    size_t len;
    bool overflow;
} sbuf_t;

static void sb_init(sbuf_t *b, char *p, size_t cap) {
    b->p = p;
    b->cap = cap;
    b->len = 0;
    b->overflow = false;
    p[0] = '\0';
}

static void sb_putc(sbuf_t *b, char c) {
    if (b->overflow || b->len + 1 >= b->cap) {
        b->overflow = true;
        return;
    }
    b->p[b->len++] = c;
    b->p[b->len] = '\0';
}

static void sb_putn(sbuf_t *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        sb_putc(b, s[i]);
}

static void sb_puts(sbuf_t *b, const char *s) { sb_putn(b, s, strlen(s)); }

static const char k_hex[] = "0123456789ABCDEF";

static bool is_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~';
}

static void sb_put_pct(sbuf_t *b, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (is_unreserved(c)) {
            sb_putc(b, (char)c);
        } else {
            sb_putc(b, '%');
            sb_putc(b, k_hex[c >> 4]);
            sb_putc(b, k_hex[c & 15]);
        }
    }
}

static void sb_put_json(sbuf_t *b, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            sb_putc(b, '\\');
            sb_putc(b, (char)c);
        } else if (c < 0x20) {
            sb_puts(b, "\\u00");
            sb_putc(b, k_hex[c >> 4]);
            sb_putc(b, k_hex[c & 15]);
        } else {
            sb_putc(b, (char)c);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Placeholders                                                        */
/* ------------------------------------------------------------------ */

typedef enum { PH_NONE, PH_IP, PH_USER, PH_PASS, PH_TOKEN, PH_UNKNOWN } ph_t;

/* s points at '{'. A placeholder-shaped token is '{' + 1..16 of [a-z0-9_] + '}'; anything else
 * (a JSON brace, say) is literal text. */
static ph_t ph_at(const char *s, size_t *tok_len) {
    size_t j = 1;
    while (j <= 16 && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= '0' && s[j] <= '9') || s[j] == '_'))
        j++;
    if (j == 1 || s[j] != '}')
        return PH_NONE;
    *tok_len = j + 1;
    size_t n = j - 1;
    if (n == 2 && strncmp(s + 1, "ip", 2) == 0)
        return PH_IP;
    if (n == 4 && strncmp(s + 1, "user", 4) == 0)
        return PH_USER;
    if (n == 4 && strncmp(s + 1, "pass", 4) == 0)
        return PH_PASS;
    if (n == 5 && strncmp(s + 1, "token", 5) == 0)
        return PH_TOKEN;
    return PH_UNKNOWN;
}

typedef enum { ENC_RAW, ENC_PCT, ENC_JSON } enc_t;

typedef struct {
    const ddns_cfg_t *c;
    char ip[16];
} fill_ctx_t;

/* Fill one template region in a single left-to-right pass; inserted values are never rescanned.
 * json_strings: placeholders must sit inside JSON string literals. */
static ddns_err_t fill_region(const fill_ctx_t *x, const char *t, size_t n, enc_t enc, bool json_strings, sbuf_t *out) {
    bool in_str = false;
    for (size_t i = 0; i < n;) {
        char ch = t[i];
        if (ch == '{') {
            size_t tl = 0;
            ph_t ph = ph_at(t + i, &tl);
            if (ph != PH_NONE && i + tl <= n) {
                if (ph == PH_UNKNOWN)
                    return DDNS_E_PLACEHOLDER;
                if (json_strings && !in_str)
                    return DDNS_E_PLACEHOLDER;
                const char *v = ph == PH_IP     ? x->ip
                                : ph == PH_USER ? x->c->user
                                : ph == PH_PASS ? x->c->pass
                                                : x->c->token;
                if (ph == PH_IP || enc == ENC_RAW)
                    sb_puts(out, v);
                else if (enc == ENC_PCT)
                    sb_put_pct(out, v);
                else
                    sb_put_json(out, v);
                i += tl;
                continue;
            }
        }
        if (json_strings) {
            if (in_str && ch == '\\' && i + 1 < n) {
                sb_putc(out, ch);
                sb_putc(out, t[i + 1]);
                i += 2;
                continue;
            }
            if (ch == '"')
                in_str = !in_str;
        }
        sb_putc(out, ch);
        i++;
    }
    return out->overflow ? DDNS_E_TOO_LONG : DDNS_OK;
}

/* ------------------------------------------------------------------ */
/* Header checks                                                       */
/* ------------------------------------------------------------------ */

static bool is_tchar(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("!#$%&'*+-.^_`|~", c);
}

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z')
            x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = (char)(y - 'A' + 'a');
        if (x != y)
            return false;
    }
    return *a == *b;
}

static ddns_err_t split_header(const char *line, ddns_req_t *out) {
    const char *colon = strchr(line, ':');
    if (!colon)
        return DDNS_E_HEADER;
    size_t nl = (size_t)(colon - line);
    if (nl == 0 || nl >= sizeof out->hdr_name)
        return DDNS_E_HEADER;
    for (size_t i = 0; i < nl; i++)
        if (!is_tchar((unsigned char)line[i]))
            return DDNS_E_HEADER;
    memcpy(out->hdr_name, line, nl);
    out->hdr_name[nl] = '\0';
    static const char *const reserved[] = {"Host", "Content-Length", "Transfer-Encoding", "Connection", "Cookie"};
    for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++)
        if (ieq(out->hdr_name, reserved[i]))
            return DDNS_E_HEADER;
    const char *v = colon + 1;
    while (*v == ' ' || *v == '\t')
        v++;
    for (const char *p = v; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c < 0x20 && c != '\t') || c == 0x7f)
            return DDNS_E_HEADER;
    }
    size_t vl = strlen(v);
    if (vl >= sizeof out->hdr_value)
        return DDNS_E_TOO_LONG;
    memcpy(out->hdr_value, v, vl + 1);
    out->has_header = true;
    return DDNS_OK;
}

/* ------------------------------------------------------------------ */
/* Fill                                                                */
/* ------------------------------------------------------------------ */

static ddns_err_t fill_url(const fill_ctx_t *x, ddns_req_t *out, bool *basic_from_url) {
    const char *u = x->c->url;
    const char *sep = strstr(u, "://");
    if (!sep || sep == u)
        return DDNS_E_URL;
    const char *auth = sep + 3;
    size_t alen = strcspn(auth, "/?#");
    const char *host = auth;
    const char *at = NULL;
    for (size_t i = 0; i < alen; i++)
        if (auth[i] == '@')
            at = auth + i;
    if (at) {
        static const char k_ui[] = "{user}:{pass}";
        if ((size_t)(at - auth) != sizeof k_ui - 1 || strncmp(auth, k_ui, sizeof k_ui - 1) != 0) {
            for (const char *p = auth; p < at; p++)
                if (*p == '{')
                    return DDNS_E_PLACEHOLDER;
            return DDNS_E_URL; /* literal userinfo is moved into the fields when the settings are saved */
        }
        *basic_from_url = true;
        host = at + 1;
    }
    const char *auth_end = auth + alen;
    for (const char *p = u; p < auth_end; p++) {
        if (p >= auth && p < host)
            continue;
        if (*p == '{')
            return DDNS_E_PLACEHOLDER;
    }
    sbuf_t b;
    sb_init(&b, out->url, sizeof out->url);
    sb_putn(&b, u, (size_t)(auth - u));
    sb_putn(&b, host, (size_t)(auth_end - host));
    return fill_region(x, auth_end, strlen(auth_end), ENC_PCT, false, &b);
}

static ddns_err_t fill_all(const ddns_cfg_t *c, uint32_t ip, ddns_req_t *out) {
    fill_ctx_t x;
    x.c = c;

    {
        sbuf_t ib;
        sb_init(&ib, x.ip, sizeof x.ip);
        for (int s = 24; s >= 0; s -= 8) {
            unsigned v = (ip >> s) & 0xFF;
            char d[4];
            int n = 0;
            do {
                d[n++] = (char)('0' + v % 10);
                v /= 10;
            } while (v);
            while (n)
                sb_putc(&ib, d[--n]);
            if (s)
                sb_putc(&ib, '.');
        }
    }

    bool basic_from_url = false;
    ddns_err_t e = fill_url(&x, out, &basic_from_url);
    if (e != DDNS_OK)
        return e;

    if (c->header[0]) {
        char line[513];
        sbuf_t hb;
        sb_init(&hb, line, sizeof line);
        e = fill_region(&x, c->header, strlen(c->header), ENC_RAW, false, &hb);
        if (e == DDNS_OK)
            e = split_header(line, out);
        ddns_wipe(line, sizeof line);
        if (e != DDNS_OK)
            return e;
    }

    if (c->method != DDNS_M_GET && c->body[0]) {
        bool json = c->body_type == DDNS_BODY_JSON;
        sbuf_t bb;
        sb_init(&bb, out->body, sizeof out->body);
        e = fill_region(&x, c->body, strlen(c->body), json ? ENC_JSON : ENC_PCT, json, &bb);
        if (e != DDNS_OK)
            return e;
        if (json && !ddns_json_valid(out->body))
            return DDNS_E_JSON;
    }

    bool hdr_auth = out->has_header && ieq(out->hdr_name, "Authorization");
    if (!hdr_auth && (basic_from_url || (c->user[0] && c->pass[0]))) {
        char raw[sizeof c->user + sizeof c->pass];
        size_t ul = strlen(c->user), pl = strlen(c->pass);
        memcpy(raw, c->user, ul);
        raw[ul] = ':';
        memcpy(raw + ul + 1, c->pass, pl);
        memcpy(out->auth, "Basic ", 6);
        size_t n = ddns_base64((const uint8_t *)raw, ul + 1 + pl, out->auth + 6, sizeof out->auth - 6);
        ddns_wipe(raw, sizeof raw);
        if (n == 0 && ul + 1 + pl > 0)
            return DDNS_E_TOO_LONG;
    }
    return DDNS_OK;
}

ddns_err_t ddns_fill(const ddns_cfg_t *c, uint32_t ip, ddns_req_t *out) {
    memset(out, 0, sizeof *out);
    ddns_err_t e = fill_all(c, ip, out);
    if (e != DDNS_OK)
        ddns_wipe(out, sizeof *out);
    return e;
}

size_t ddns_base64(const uint8_t *in, size_t n, char *out, size_t out_sz) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = (n + 2) / 3 * 4;
    if (need + 1 > out_sz)
        return 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n)
            v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n)
            v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

/* ------------------------------------------------------------------ */
/* JSON syntax check                                                   */
/* ------------------------------------------------------------------ */

static void j_ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')
        (*p)++;
}

static bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static bool j_string(const char **p) {
    const char *s = *p;
    if (*s != '"')
        return false;
    s++;
    for (;;) {
        unsigned char c = (unsigned char)*s;
        if (c == '\0' || c < 0x20)
            return false;
        if (c == '"')
            break;
        if (c == '\\') {
            s++;
            if (*s == 'u') {
                for (int i = 1; i <= 4; i++)
                    if (!is_hex(s[i]))
                        return false;
                s += 4;
            } else if (!*s || !strchr("\"\\/bfnrt", *s)) {
                return false;
            }
        }
        s++;
    }
    *p = s + 1;
    return true;
}

static bool j_digits(const char **p) {
    if (**p < '0' || **p > '9')
        return false;
    while (**p >= '0' && **p <= '9')
        (*p)++;
    return true;
}

static bool j_number(const char **p) {
    if (**p == '-')
        (*p)++;
    if (**p == '0')
        (*p)++;
    else if (!j_digits(p))
        return false;
    if (**p == '.') {
        (*p)++;
        if (!j_digits(p))
            return false;
    }
    if (**p == 'e' || **p == 'E') {
        (*p)++;
        if (**p == '+' || **p == '-')
            (*p)++;
        if (!j_digits(p))
            return false;
    }
    return true;
}

static bool j_value(const char **p, int depth);

static bool j_container(const char **p, int depth, char close, bool object) {
    if (depth > 8)
        return false;
    (*p)++;
    j_ws(p);
    if (**p == close) {
        (*p)++;
        return true;
    }
    for (;;) {
        j_ws(p);
        if (object) {
            if (!j_string(p))
                return false;
            j_ws(p);
            if (**p != ':')
                return false;
            (*p)++;
        }
        if (!j_value(p, depth))
            return false;
        j_ws(p);
        if (**p == ',') {
            (*p)++;
            continue;
        }
        if (**p == close) {
            (*p)++;
            return true;
        }
        return false;
    }
}

static bool j_value(const char **p, int depth) {
    j_ws(p);
    switch (**p) {
    case '{':
        return j_container(p, depth + 1, '}', true);
    case '[':
        return j_container(p, depth + 1, ']', false);
    case '"':
        return j_string(p);
    case 't':
        if (strncmp(*p, "true", 4))
            return false;
        *p += 4;
        return true;
    case 'f':
        if (strncmp(*p, "false", 5))
            return false;
        *p += 5;
        return true;
    case 'n':
        if (strncmp(*p, "null", 4))
            return false;
        *p += 4;
        return true;
    default:
        return j_number(p);
    }
}

bool ddns_json_valid(const char *s) {
    if (!s)
        return false;
    const char *p = s;
    if (!j_value(&p, 0))
        return false;
    j_ws(&p);
    return *p == '\0';
}

/* ------------------------------------------------------------------ */
/* Save-time validation                                                */
/* ------------------------------------------------------------------ */

static void *(*s_alloc)(size_t) = malloc;
static void (*s_release)(void *) = free;

void ddns_core_set_alloc(void *(*alloc)(size_t), void (*release)(void *)) {
    s_alloc = alloc ? alloc : malloc;
    s_release = release ? release : free;
}

bool ddns_url_host(const char *url, char *host, size_t host_sz) {
    const char *sep = url ? strstr(url, "://") : NULL;
    if (!sep || host_sz == 0)
        return false;
    const char *a = sep + 3;
    size_t alen = strcspn(a, "/?#");
    const char *h = a;
    for (size_t i = 0; i < alen; i++)
        if (a[i] == '@')
            h = a + i + 1;
    size_t hl = strcspn(h, ":/?#");
    if (h + hl > a + alen)
        hl = (size_t)(a + alen - h);
    if (hl == 0 || hl >= host_sz)
        return false;
    memcpy(host, h, hl);
    host[hl] = '\0';
    return true;
}

static void set_msg(char *msg, size_t sz, const char *prefix, ddns_err_t e) {
    if (!msg || sz == 0)
        return;
    sbuf_t b;
    sb_init(&b, msg, sz);
    if (prefix)
        sb_puts(&b, prefix);
    sb_puts(&b, ddns_err_str(e));
}

static int hexval(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Percent-decode s[0..n) into out (size out_sz). False when it does not fit. */
static bool pct_decode(const char *s, size_t n, char *out, size_t out_sz) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '%' && i + 2 < n && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            c = (char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        }
        if (o + 1 >= out_sz)
            return false;
        out[o++] = c;
    }
    out[o] = '\0';
    return true;
}

/* Rule 2: a literal user:pass@ moves into the fields and becomes {user}:{pass}@. */
static ddns_err_t move_userinfo(ddns_cfg_t *c) {
    const char *sep = strstr(c->url, "://");
    if (!sep)
        return DDNS_OK; /* the scheme rule reports it */
    const char *a = sep + 3;
    size_t alen = strcspn(a, "/?#");
    const char *at = NULL;
    for (size_t i = 0; i < alen; i++)
        if (a[i] == '@')
            at = a + i;
    if (!at)
        return DDNS_OK;
    size_t ul = (size_t)(at - a);
    static const char k_ui[] = "{user}:{pass}";
    if (ul == sizeof k_ui - 1 && strncmp(a, k_ui, ul) == 0)
        return DDNS_OK;
    if (memchr(a, '{', ul))
        return DDNS_E_PLACEHOLDER;
    const char *colon = memchr(a, ':', ul);
    size_t nl = colon ? (size_t)(colon - a) : ul;
    char user[sizeof c->user], pass[sizeof c->pass];
    pass[0] = '\0';
    bool ok = pct_decode(a, nl, user, sizeof user) && (!colon || pct_decode(colon + 1, ul - nl - 1, pass, sizeof pass));
    char url[sizeof c->url];
    sbuf_t b;
    sb_init(&b, url, sizeof url);
    sb_putn(&b, c->url, (size_t)(a - c->url));
    sb_puts(&b, k_ui);
    sb_puts(&b, at);
    ddns_err_t e = DDNS_OK;
    if (!ok || b.overflow) {
        e = DDNS_E_TOO_LONG;
    } else {
        memcpy(c->user, user, sizeof user);
        memcpy(c->pass, pass, sizeof pass);
        memcpy(c->url, url, sizeof url);
    }
    ddns_wipe(user, sizeof user);
    ddns_wipe(pass, sizeof pass);
    ddns_wipe(url, sizeof url);
    return e;
}

static bool secret_param_name(const char *n, size_t len) {
    static const char *const names[] = {"token", "key", "password", "pass", "pwd", "secret", "apikey", "secretapikey"};
    char low[16];
    if (len == 1 && n[0] == 'q')
        return true; /* ClouDNS dynamic URL; case-sensitive */
    if (len == 0 || len >= sizeof low)
        return false;
    for (size_t i = 0; i < len; i++)
        low[i] = (n[i] >= 'A' && n[i] <= 'Z') ? (char)(n[i] - 'A' + 'a') : n[i];
    low[len] = '\0';
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(low, names[i]) == 0)
            return true;
    return false;
}

static bool literal_value(const char *v, size_t n) { return n > 0 && !memchr(v, '{', n); }

/* name=value&... (query string or form body) */
static bool params_have_literal_secret(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        size_t end = i;
        while (end < n && s[end] != '&' && s[end] != '#')
            end++;
        const char *eq = memchr(s + i, '=', end - i);
        if (eq) {
            size_t nl = (size_t)(eq - (s + i));
            if (secret_param_name(s + i, nl) && literal_value(eq + 1, (size_t)(s + end - eq - 1)))
                return true;
        }
        if (end < n && s[end] == '#')
            break;
        i = end + 1;
    }
    return false;
}

/* Reads a JSON string at *p (no unescaping needed for the names we look for). */
static bool scan_json_string(const char **p, const char **start, size_t *len) {
    const char *s = *p + 1;
    *start = s;
    while (*s && *s != '"') {
        if (*s == '\\' && s[1])
            s++;
        s++;
    }
    if (*s != '"')
        return false;
    *len = (size_t)(s - *start);
    *p = s + 1;
    return true;
}

static bool json_has_literal_secret(const char *s) {
    const char *p = s;
    while (*p) {
        if (*p != '"') {
            p++;
            continue;
        }
        const char *k;
        size_t kl;
        if (!scan_json_string(&p, &k, &kl))
            return false;
        const char *q = p;
        j_ws(&q);
        if (*q != ':')
            continue;
        q++;
        j_ws(&q);
        if (*q != '"')
            continue;
        const char *v;
        size_t vl;
        p = q;
        if (!scan_json_string(&p, &v, &vl))
            return false;
        if (secret_param_name(k, kl) && literal_value(v, vl))
            return true;
    }
    return false;
}

static bool header_has_literal_secret(const char *h) {
    const char *colon = strchr(h, ':');
    if (!colon)
        return false;
    char low[64];
    size_t nl = (size_t)(colon - h);
    if (nl >= sizeof low)
        nl = sizeof low - 1;
    for (size_t i = 0; i < nl; i++)
        low[i] = (h[i] >= 'A' && h[i] <= 'Z') ? (char)(h[i] - 'A' + 'a') : h[i];
    low[nl] = '\0';
    if (!strstr(low, "auth") && !strstr(low, "key") && !strstr(low, "token") && !strstr(low, "secret"))
        return false;
    const char *v = colon + 1;
    while (*v == ' ' || *v == '\t')
        v++;
    return literal_value(v, strlen(v));
}

static ddns_err_t check_sources(const ddns_cfg_t *c, int *bad_slot) {
    int http = 0;
    for (int i = 0; i < 3; i++) {
        *bad_slot = i;
        const char *u = ddns_src_url(c, i);
        bool is_http = strncmp(u, "http://", 7) == 0; // NOSONAR: detects plain HTTP to limit it
        if (!is_http && strncmp(u, "https://", 8) != 0)
            return DDNS_E_SOURCES;
        char host[128];
        if (!ddns_url_host(u, host, sizeof host))
            return DDNS_E_SOURCES;
        uint32_t ip;
        if (ddns_ipv4_parse(host, strlen(host), &ip) && !ddns_ipv4_is_public(ip))
            return DDNS_E_SOURCES;
        if (is_http)
            http++;
        if (http > 1)
            return DDNS_E_SOURCES;
        for (int j = 0; j < i; j++)
            if (strcmp(u, ddns_src_url(c, j)) == 0)
                return DDNS_E_SOURCES;
    }
    *bad_slot = -1;
    return DDNS_OK;
}

static ddns_err_t dry_run(const ddns_cfg_t *c) {
    struct scratch {
        ddns_cfg_t cfg;
        ddns_req_t req;
    } *s = s_alloc(sizeof *s);
    if (!s)
        return DDNS_E_TOO_LONG;
    memcpy(&s->cfg, c, sizeof *c);
    memset(s->cfg.user, 'x', strlen(c->user));
    memset(s->cfg.pass, 'x', strlen(c->pass));
    memset(s->cfg.token, 'x', strlen(c->token));
    ddns_err_t e = ddns_fill(&s->cfg, 0x01020304u, &s->req);
    ddns_wipe(s, sizeof *s);
    s_release(s);
    return e;
}

ddns_err_t ddns_cfg_validate(ddns_cfg_t *c, char *msg, size_t msg_sz) {
    ddns_err_t e = DDNS_OK;
    const char *prefix = NULL;
    if (msg && msg_sz)
        msg[0] = '\0';

    /* NUL-terminate every string field so nothing below can run past a field. */
    c->url[sizeof c->url - 1] = c->header[sizeof c->header - 1] = c->body[sizeof c->body - 1] = '\0';
    c->user[sizeof c->user - 1] = c->pass[sizeof c->pass - 1] = c->token[sizeof c->token - 1] = '\0';
    c->match[sizeof c->match - 1] = '\0';
    for (int i = 0; i < 3; i++)
        c->src[i].custom[sizeof c->src[i].custom - 1] = '\0';

    if (c->interval_min < 1 || c->interval_min > 1440 || c->refresh_days > 365 || c->method > DDNS_M_PATCH ||
        c->body_type > DDNS_BODY_JSON) {
        e = DDNS_E_RANGE;
        goto out;
    }
    for (int i = 0; i < 3; i++)
        if (c->src[i].kind > DDNS_SRC_CUSTOM) {
            e = DDNS_E_SOURCES;
            goto out;
        }
    /* Disabled with no template at all is the factory state: nothing is ever sent, so allow it. */
    if (!c->enabled && !c->url[0] && !c->header[0] && !c->body[0])
        goto out;
    e = move_userinfo(c);
    if (e != DDNS_OK)
        goto out;
    if (strncmp(c->url, "https://", 8) != 0) {
        e = DDNS_E_SCHEME;
        goto out;
    }
    int bad = -1;
    e = check_sources(c, &bad);
    if (e != DDNS_OK) {
        static const char *const slot_prefix[] = {"IP source 1: ", "IP source 2: ", "IP source 3: "};
        prefix = bad >= 0 ? slot_prefix[bad] : NULL;
        goto out;
    }
    const char *q = strchr(c->url, '?');
    bool lit = q && params_have_literal_secret(q + 1, strlen(q + 1));
    if (!lit && c->body[0])
        lit = c->body_type == DDNS_BODY_JSON ? json_has_literal_secret(c->body)
                                             : params_have_literal_secret(c->body, strlen(c->body));
    if (!lit && c->header[0])
        lit = header_has_literal_secret(c->header);
    if (lit) {
        e = DDNS_E_SECRET_LITERAL;
        goto out;
    }
    if (c->body[0] && c->method == DDNS_M_GET) {
        e = DDNS_E_BODY_WITH_GET;
        goto out;
    }
    if (!c->src_ip && !strstr(c->url, "{ip}") && !strstr(c->header, "{ip}") && !strstr(c->body, "{ip}")) {
        e = DDNS_E_NO_IP;
        goto out;
    }
    e = dry_run(c);
out:
    if (e != DDNS_OK)
        set_msg(msg, msg_sz, prefix, e);
    return e;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static uint64_t fnv_str(uint64_t h, const char *s) { return fnv(h, s, strlen(s) + 1); }

uint64_t ddns_cfg_fingerprint(const ddns_cfg_t *c) {
    uint64_t h = 0xcbf29ce484222325ull;
    uint8_t flags[3] = {c->method, c->body_type, c->src_ip};
    h = fnv_str(h, c->url);
    h = fnv_str(h, c->header);
    h = fnv_str(h, c->body);
    h = fnv(h, flags, sizeof flags);
    h = fnv_str(h, c->user);
    h = fnv_str(h, c->pass);
    h = fnv_str(h, c->token);
    return h;
}

/* ------------------------------------------------------------------ */
/* Provider response                                                   */
/* ------------------------------------------------------------------ */

bool ddns_match(const char *pattern, const char *body) {
    if (!pattern || !pattern[0])
        return true;
    const char *p = pattern;
    for (;;) {
        size_t n = strcspn(p, "|");
        if (n > 0) {
            if (p[0] == '^') {
                if (n > 1 && strncmp(body, p + 1, n - 1) == 0)
                    return true;
            } else {
                char alt[65];
                if (n < sizeof alt) {
                    memcpy(alt, p, n);
                    alt[n] = '\0';
                    if (strstr(body, alt))
                        return true;
                }
            }
        }
        if (!p[n])
            return false;
        p += n + 1;
    }
}

static void sb_put_uint(sbuf_t *b, unsigned v) {
    char d[12];
    int n = 0;
    do {
        d[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        sb_putc(b, d[--n]);
}

static void set_reason(ddns_class_t *r, const char *prefix, int status) {
    sbuf_t b;
    sb_init(&b, r->reason, sizeof r->reason);
    sb_puts(&b, prefix);
    if (status >= 0)
        sb_put_uint(&b, (unsigned)status);
}

static uint32_t retry_after_s(const char *ra) {
    if (!ra)
        return 600;
    while (*ra == ' ')
        ra++;
    if (*ra < '0' || *ra > '9')
        return 600;
    uint64_t v = 0;
    for (; *ra >= '0' && *ra <= '9'; ra++) {
        v = v * 10 + (uint64_t)(*ra - '0');
        if (v > 86400)
            return 86400;
    }
    while (*ra == ' ')
        ra++;
    return *ra ? 600 : (uint32_t)v;
}

ddns_class_t ddns_classify(int http_status, bool timed_out, const char *body, size_t len, const char *match,
                           const char *retry_after) {
    ddns_class_t r = {.outcome = DDNS_OUT_TEMP};
    char text[1025];
    if (len > 1024)
        len = 1024;
    size_t a = 0, b = len;
    while (a < b && is_ws(body[a]))
        a++;
    while (b > a && is_ws(body[b - 1]))
        b--;
    memcpy(text, body + a, b - a);
    text[b - a] = '\0';
    char word[16];
    size_t wl = strcspn(text, " \r\n");
    if (wl >= sizeof word)
        wl = 0; /* longer than any code we look for */
    memcpy(word, text, wl);
    word[wl] = '\0';

    static const char *const reject[] = {"badauth", "nohost", "notfqdn", "numhost", "!donator", "abuse", "badagent"};
    if (timed_out) {
        set_reason(&r, "timeout", -1);
        return r;
    }
    if (http_status == 401 || http_status == 403 || http_status == 404) {
        r.outcome = DDNS_OUT_STOPPED;
        set_reason(&r, "http ", http_status);
        return r;
    }
    for (size_t i = 0; i < sizeof reject / sizeof reject[0]; i++)
        if (strcmp(word, reject[i]) == 0) {
            r.outcome = DDNS_OUT_STOPPED;
            set_reason(&r, word, -1);
            return r;
        }
    if (http_status == 429) {
        r.outcome = DDNS_OUT_COOLDOWN;
        r.cooldown_s = retry_after_s(retry_after);
        set_reason(&r, "http 429", -1);
        return r;
    }
    if (strcmp(word, "dnserr") == 0 || strcmp(word, "911") == 0) {
        r.outcome = DDNS_OUT_COOLDOWN;
        r.cooldown_s = 1800;
        set_reason(&r, word, -1);
        return r;
    }
    if (http_status >= 300 && http_status <= 399) {
        r.outcome = DDNS_OUT_REDIRECT;
        set_reason(&r, "redirect (3xx)", -1);
        return r;
    }
    if (http_status >= 200 && http_status <= 299) {
        if (ddns_match(match, text)) {
            r.outcome = DDNS_OUT_SUCCESS;
            set_reason(&r, word, -1);
        } else {
            set_reason(&r, "no match", -1);
        }
        return r;
    }
    set_reason(&r, "http ", http_status);
    return r;
}

void ddns_redact_snippet(const char *body, size_t len, const char *user, const char *pass, const char *token, char *out,
                         size_t out_sz) {
    char pct_pass[3 * 128 + 1], pct_token[3 * 128 + 1], b64[264];
    char json_pass[6 * 128 + 1], json_token[6 * 128 + 1];
    sbuf_t b;
    sb_init(&b, pct_pass, sizeof pct_pass);
    sb_put_pct(&b, pass);
    sb_init(&b, pct_token, sizeof pct_token);
    sb_put_pct(&b, token);
    sb_init(&b, json_pass, sizeof json_pass);
    sb_put_json(&b, pass);
    sb_init(&b, json_token, sizeof json_token);
    sb_put_json(&b, token);
    b64[0] = '\0';
    if (pass[0]) {
        char raw[64 + 1 + 128 + 1];
        size_t ul = strlen(user), pl = strlen(pass);
        if (ul > 64)
            ul = 64;
        if (pl > 128)
            pl = 128;
        memcpy(raw, user, ul);
        raw[ul] = ':';
        memcpy(raw + ul + 1, pass, pl);
        ddns_base64((const uint8_t *)raw, ul + 1 + pl, b64, sizeof b64);
        ddns_wipe(raw, sizeof raw);
    }
    enum { N_NEEDLES = 7 };
    const char *needles[N_NEEDLES] = {pass, token, pct_pass, pct_token, b64, json_pass, json_token};
    size_t nlen[N_NEEDLES];
    for (int i = 0; i < N_NEEDLES; i++)
        nlen[i] = strlen(needles[i]);
    /* longest first, so a raw secret inside its own encoded form never leaves a fragment */
    for (int i = 0; i < N_NEEDLES; i++)
        for (int j = i + 1; j < N_NEEDLES; j++)
            if (nlen[j] > nlen[i]) {
                const char *t = needles[i];
                needles[i] = needles[j];
                needles[j] = t;
                size_t tl = nlen[i];
                nlen[i] = nlen[j];
                nlen[j] = tl;
            }

    size_t cap = out_sz < 65 ? out_sz : 65;
    sb_init(&b, out, cap);
    size_t i = 0;
    while (i < len && is_ws(body[i]))
        i++;
    while (i < len && b.len + 1 < cap) {
        bool hit = false;
        for (int k = 0; k < N_NEEDLES && !hit; k++)
            if (nlen[k] && nlen[k] <= len - i && memcmp(body + i, needles[k], nlen[k]) == 0) {
                for (const char *s = "***"; *s && b.len + 1 < cap; s++)
                    sb_putc(&b, *s);
                i += nlen[k];
                hit = true;
            }
        if (hit)
            continue;
        unsigned char c = (unsigned char)body[i++];
        sb_putc(&b, (c >= 0x20 && c <= 0x7e) ? (char)c : '.');
    }
    ddns_wipe(pct_pass, sizeof pct_pass);
    ddns_wipe(pct_token, sizeof pct_token);
    ddns_wipe(b64, sizeof b64);
    ddns_wipe(json_pass, sizeof json_pass);
    ddns_wipe(json_token, sizeof json_token);
}

/* ---- Scheduler ---- */

#define US_PER_S 1000000LL

bool ddns_sched_try_force(ddns_sched_t *s, int64_t now_us, uint32_t *wait_s) {
    if (s->last_force_us && now_us - s->last_force_us < 60 * US_PER_S) {
        int64_t left = s->last_force_us + 60 * US_PER_S - now_us;
        if (wait_s)
            *wait_s = (uint32_t)((left + US_PER_S - 1) / US_PER_S);
        return false;
    }
    s->last_force_us = now_us;
    s->force_pending = true;
    if (s->stopped)
        s->stop_clear_pending = true;
    s->stopped = false;
    s->stop_reason[0] = '\0';
    return true;
}

bool ddns_sched_want_send(ddns_sched_t *s, uint32_t ip, uint64_t fp, int64_t wall_now, uint16_t refresh_days) {
    /* A success stamped in the future (clock later corrected backwards) counts from now, so
     * refresh is neither early nor postponed by the size of the jump. */
    if (s->have_ok && s->ok_wall > wall_now)
        s->ok_wall = wall_now;
    if (s->force_pending)
        return true;
    if (s->stopped)
        return false;
    if (!s->have_ok || s->ok_ip != ip || s->ok_fp != fp)
        return true;
    if (refresh_days > 0) {
        if (wall_now - s->ok_wall >= (int64_t)refresh_days * 86400)
            return true;
    }
    return false;
}

int64_t ddns_sched_provider_ready_us(const ddns_sched_t *s) {
    int64_t ready = s->last_attempt_us ? s->last_attempt_us + 60 * US_PER_S : 0;
    if (s->cooldown_until_us > ready)
        ready = s->cooldown_until_us;
    if (s->backoff_until_us > ready)
        ready = s->backoff_until_us;
    return ready;
}

bool ddns_sched_may_dispatch(const ddns_sched_t *s, int64_t now_us, uint32_t gen_at_start, uint32_t gen_now,
                             uint32_t epoch_at_start, uint32_t epoch_now) {
    return now_us >= ddns_sched_provider_ready_us(s) && gen_at_start == gen_now && epoch_at_start == epoch_now;
}

void ddns_sched_on_attempt(ddns_sched_t *s, int64_t now_us) {
    s->last_attempt_us = now_us;
    s->force_pending = false;
}

void ddns_sched_on_result(ddns_sched_t *s, const ddns_class_t *r, bool same_gen, int64_t now_us, int64_t wall_now,
                          uint32_t ip, uint64_t fp, uint16_t interval_min) {
    switch (r->outcome) {
    case DDNS_OUT_SUCCESS:
        if (same_gen) {
            s->have_ok = true;
            s->ok_ip = ip;
            s->ok_fp = fp;
            s->ok_wall = wall_now;
            s->backoff_s = 0;
            s->backoff_until_us = 0;
        }
        break;
    case DDNS_OUT_STOPPED:
        if (same_gen) {
            s->stopped = true;
            strncpy(s->stop_reason, r->reason, sizeof s->stop_reason - 1);
            s->stop_reason[sizeof s->stop_reason - 1] = '\0';
        }
        break;
    case DDNS_OUT_COOLDOWN:
        s->cooldown_until_us = now_us + (int64_t)r->cooldown_s * US_PER_S;
        break;
    case DDNS_OUT_REDIRECT:
    case DDNS_OUT_TEMP:
    default:
        if (s->backoff_s) {
            s->backoff_s = s->backoff_s * 2 > 3600 ? 3600 : s->backoff_s * 2;
        } else {
            uint32_t b = (uint32_t)interval_min * 60;
            if (b > 3600)
                b = 3600;
            if (b < 60)
                b = 60;
            s->backoff_s = b;
        }
        s->backoff_until_us = now_us + (int64_t)s->backoff_s * US_PER_S;
        break;
    }
}

void ddns_sched_on_config_saved(ddns_sched_t *s) {
    s->stopped = false;
    s->stop_reason[0] = '\0';
    s->backoff_s = 0;
    s->backoff_until_us = 0;
    s->force_pending = false;
}

void ddns_sched_on_uplink_moved(ddns_sched_t *s) { s->have_ok = false; }

void ddns_sched_apply_stored_stop(ddns_sched_t *s, const char *stored) {
    if (!stored || !stored[0])
        return;
    s->stopped = true;
    strncpy(s->stop_reason, stored, sizeof s->stop_reason - 1);
    s->stop_reason[sizeof s->stop_reason - 1] = '\0';
}
