"""DDNS client coverage.

* Web API: defaults, write-only secrets, validation errors, JSON-only
  guard, Force rate limit and the /api/status summary. The original
  settings are restored at the end (secrets cleared if the run set them).
* SNMP: the NET-SNMP-EXTEND-MIB rows for the "ddns" extend entry are walked
  with a tiny built-in SNMPv2c GETNEXT client (no net-snmp / pysnmp needed).
  Skipped when SNMP is disabled on the device. ESP_SNMP_COMMUNITY, when set,
  is used directly instead of reading it from /api/snmp.
"""
from __future__ import annotations

import ipaddress
import json
import os
import secrets
import socket
import time
from urllib.parse import urlparse

from .common import Context, Result, SpaClient, check, skip

MODULE_ID = "ddns"
MODULE_DESC = "DDNS client: web API + SNMP extend rows"

EXT = "1.3.6.1.4.1.8072.1.3.2"
IDX = "4.100.100.110.115"  # "ddns"
EXPECTED_OIDS = (
    [f"{EXT}.1.0"]
    + [f"{EXT}.2.1.{c}.{IDX}" for c in (2, 3, 4, 5, 6, 7, 20, 21)]
    + [f"{EXT}.3.1.{c}.{IDX}" for c in (1, 2, 3, 4)]
)


# ---- minimal BER / SNMPv2c ----

def _len(n: int) -> bytes:
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b


def _tlv(tag: int, body: bytes) -> bytes:
    return bytes([tag]) + _len(len(body)) + body


def _int(v: int) -> bytes:
    return _tlv(0x02, v.to_bytes(max(1, (v.bit_length() + 8) // 8), "big", signed=True))


def _oid_enc(oid: str) -> bytes:
    arcs = [int(x) for x in oid.split(".")]
    out = bytes([arcs[0] * 40 + arcs[1]])
    for a in arcs[2:]:
        chunk = [a & 0x7F]
        a >>= 7
        while a:
            chunk.insert(0, 0x80 | (a & 0x7F))
            a >>= 7
        out += bytes(chunk)
    return _tlv(0x06, out)


def _read(buf: bytes, pos: int) -> tuple[int, bytes, int]:
    tag = buf[pos]
    n = buf[pos + 1]
    pos += 2
    if n & 0x80:
        k = n & 0x7F
        n = int.from_bytes(buf[pos:pos + k], "big")
        pos += k
    return tag, buf[pos:pos + n], pos + n


def _oid_dec(b: bytes) -> str:
    arcs = [b[0] // 40, b[0] % 40]
    v = 0
    for x in b[1:]:
        v = (v << 7) | (x & 0x7F)
        if not x & 0x80:
            arcs.append(v)
            v = 0
    return ".".join(map(str, arcs))


def snmp_getnext(host: str, community: str, oid: str, timeout: float = 3.0):
    """Returns (oid, tag, value) of the successor, value decoded for INTEGER/OCTET STRING."""
    rid = 1 + secrets.randbelow(0x7FFFFFFF)
    vb = _tlv(0x30, _oid_enc(oid) + b"\x05\x00")
    pdu = _tlv(0xA1, _int(rid) + _int(0) + _int(0) + _tlv(0x30, vb))
    msg = _tlv(0x30, _int(1) + _tlv(0x04, community.encode()) + pdu)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)
        s.sendto(msg, (host, 161))
        data, _ = s.recvfrom(4096)
    _, body, _ = _read(data, 0)
    pos = 0
    for _ in range(2):  # version, community
        _, _, pos = _read(body, pos)
    _, pdu_body, _ = _read(body, pos)
    pos = 0
    for _ in range(3):  # request-id, error-status, error-index
        _, _, pos = _read(pdu_body, pos)
    _, vbl, _ = _read(pdu_body, pos)
    _, vb1, _ = _read(vbl, 0)
    _, oid_b, p = _read(vb1, 0)
    tag, val, _ = _read(vb1, p)
    if tag == 0x02:
        value = int.from_bytes(val, "big", signed=True) if val else 0
    elif tag == 0x04:
        value = val.decode(errors="replace")
    else:
        value = val
    return _oid_dec(oid_b), tag, value


def check_snmp_extend(results: list[Result], host: str, community: str) -> None:
    got = []
    oid = EXT
    try:
        for _ in range(len(EXPECTED_OIDS) + 1):
            oid, tag, val = snmp_getnext(host, community, oid)
            got.append((oid, tag, val))
    except (OSError, IndexError) as e:
        check(results, MODULE_ID, "snmp extend walk", False, repr(e))
        return
    rows, after = got[:-1], got[-1]
    check(results, MODULE_ID, "snmp extend: 13 rows in order",
          [o for o, _, _ in rows] == EXPECTED_OIDS,
          f"got {[o for o, _, _ in rows]}")
    vals = {o: v for o, _, v in rows}
    check(results, MODULE_ID, "snmp extend: nsExtendNumEntries = 1",
          vals.get(f"{EXT}.1.0") == 1, f"got {vals.get(f'{EXT}.1.0')!r}")
    res = vals.get(f"{EXT}.3.1.4.{IDX}")
    check(results, MODULE_ID, "snmp extend: nsExtendResult in 0..6",
          isinstance(res, int) and 0 <= res <= 6,
          f"result={res!r} line1={vals.get(f'{EXT}.3.1.1.{IDX}')!r}")
    check(results, MODULE_ID, "snmp extend: walk continues into 99999",
          after[0].startswith("1.3.6.1.4.1.99999"), f"next={after[0]}")


# ---- web API ----

TOKEN = "T0kenValue"
DUCK = {
    "enabled": False,
    "url": "https://www.duckdns.org/update?domains=example&token={token}&ip={ip}",
    "method": "GET", "body": "", "header": "", "match": "OK",
}


def _post_raw(spa: SpaClient, path: str, text: str, ctype: str) -> dict:
    return spa.page.evaluate(
        f"""async () => {{
            const r = await fetch({json.dumps(path)}, {{method:'POST', cache:'no-store',
                headers:{{'Content-Type':{json.dumps(ctype)}}}, body:{json.dumps(text)}}});
            return {{__http_status: r.status, __body: await r.text()}};
        }}"""
    )


def _get_text(spa: SpaClient, path: str) -> str:
    return spa.page.evaluate(
        f"async () => (await fetch({json.dumps(path)}, {{cache:'no-store'}})).text()")


def _body(r: dict) -> dict:
    try:
        return json.loads(r.get("__body") or "{}")
    except ValueError:
        return {}


def check_get_defaults(results: list[Result], cfg: dict) -> None:
    srcs = [x.get("preset") for x in cfg.get("sources", [])]
    check(results, MODULE_ID, "GET /api/ddns defaults",
          cfg.get("enabled") is False and srcs == ["ipify", "icanhazip", "aws"]
          and cfg.get("interval_min") == 15 and cfg.get("wan_trigger") is True
          and cfg.get("refresh_days") == 0 and "status" in cfg,
          f"enabled={cfg.get('enabled')} sources={srcs} interval={cfg.get('interval_min')} "
          f"wan={cfg.get('wan_trigger')} refresh={cfg.get('refresh_days')}")


def check_secret_write_only(results: list[Result], spa: SpaClient) -> None:
    r = spa.post_json("/api/ddns", {**DUCK, "token": TOKEN})
    check(results, MODULE_ID, "POST DuckDNS config", r.get("__http_status") == 200, str(r)[:200])
    text = _get_text(spa, "/api/ddns")
    cfg = json.loads(text)
    check(results, MODULE_ID, "token write-only",
          cfg.get("token_set") is True and cfg.get("token_len") == len(TOKEN) and TOKEN not in text,
          f"token_set={cfg.get('token_set')} token_len={cfg.get('token_len')} leaked={TOKEN in text}")
    spa.post_json("/api/ddns", {"match": "OK"})
    cfg = spa.fetch_json("/api/ddns")
    check(results, MODULE_ID, "POST without token keeps it", cfg.get("token_set") is True, str(cfg.get("token_set")))
    spa.post_json("/api/ddns", {"token_clear": True, "url": "https://www.duckdns.org/update?domains=example&ip={ip}"})
    cfg = spa.fetch_json("/api/ddns")
    check(results, MODULE_ID, "token_clear empties it", cfg.get("token_set") is False, str(cfg.get("token_set")))


def check_validation_errors(results: list[Result], spa: SpaClient) -> None:
    secret = "S3cretVal"
    cases = {
        "http provider URL": {"url": "http://example.com/u?ip={ip}"},
        "literal token in URL": {"url": "https://example.com/u?token=abc&ip={ip}"},
        "two http sources": {"sources": [{"preset": "custom", "url": "http://a.example/ip"},
                                         {"preset": "custom", "url": "http://b.example/ip"},
                                         {"preset": "aws", "url": ""}]},
        "interval 0": {"interval_min": 0},
        "body with GET": {"method": "GET", "body": "ip={ip}"},
        "private custom source": {"sources": [{"preset": "custom", "url": "https://192.168.1.1/"},
                                              {"preset": "icanhazip", "url": ""},
                                              {"preset": "aws", "url": ""}]},
    }
    for name, patch in cases.items():
        body = {**DUCK, "url": "https://www.duckdns.org/update?domains=example&token={token}&ip={ip}",
                "token": secret, **patch}
        r = spa.post_json("/api/ddns", body)
        err = _body(r).get("error", "")
        check(results, MODULE_ID, f"400 on {name}",
              r.get("__http_status") == 400 and bool(err) and secret not in (r.get("__body") or ""),
              f"http={r.get('__http_status')} error={err!r}")


def check_json_only(results: list[Result], spa: SpaClient) -> None:
    for path in ("/api/ddns", "/api/ddns/force"):
        r = _post_raw(spa, path, "enabled=true", "application/x-www-form-urlencoded")
        check(results, MODULE_ID, f"415 on form POST {path}", r.get("__http_status") == 415,
              f"http={r.get('__http_status')}")


def check_force_rate(results: list[Result], spa: SpaClient) -> None:
    a = _body(spa.post_json("/api/ddns/force", {}))
    b = _body(spa.post_json("/api/ddns/force", {}))
    check(results, MODULE_ID, "Force rate limited",
          b.get("queued") is False and isinstance(b.get("wait_s"), (int, float)) and 1 <= b["wait_s"] <= 60,
          f"first={a} second={b}")


def check_status_has_ddns(results: list[Result], spa: SpaClient) -> None:
    st = spa.fetch_json("/api/status") or {}
    d = st.get("ddns")
    check(results, MODULE_ID, "/api/status has ddns", isinstance(d, dict) and "state" in d, str(d)[:200])


def check_spa_has_ddns_tab(results: list[Result], spa: SpaClient) -> None:
    html = _get_text(spa, "/")  # the browser's fetch undoes Content-Encoding: gzip
    missing = [s for s in ("Dynamic DNS", "Force update now", "/api/ddns/force") if s not in html]
    check(results, MODULE_ID, "SPA has the DDNS tab", not missing, f"missing {missing}" if missing else "")


def _restore(results: list[Result], spa: SpaClient, orig: dict) -> None:
    keys = ("enabled", "url", "method", "header", "body", "body_type", "src_ip", "user", "match",
            "sources", "interval_min", "wan_trigger", "refresh_days")
    body = {k: orig[k] for k in keys if k in orig}
    if not orig.get("token_set"):
        body["token_clear"] = True
    if not orig.get("pass_set"):
        body["pass_clear"] = True
    r = spa.post_json("/api/ddns", body) or {}
    check(results, MODULE_ID, "restore original config", r.get("__http_status") in (None, 200),
          f"http={r.get('__http_status')} {r.get('__body', '')}")


# The stored password/token cannot be read back, so a test that replaces them cannot restore them.
SECRETS_SKIP = "device has a stored password/token (set DDNS_TEST_RESET_SECRETS=1 to overwrite it)"


def _keeps_secrets(orig: dict) -> bool:
    return bool(orig.get("pass_set") or orig.get("token_set")) and os.environ.get("DDNS_TEST_RESET_SECRETS") != "1"


def run_api(results: list[Result], ctx: Context) -> None:
    spa = SpaClient(ctx)
    try:
        orig = spa.fetch_json("/api/ddns") or {}
        if orig.get("__http_status"):
            check(results, MODULE_ID, "GET /api/ddns", False, f"http={orig.get('__http_status')}")
            return
        if not orig.get("enabled") and not orig.get("url") and not (orig.get("pass_set") or orig.get("token_set")):
            check_get_defaults(results, orig)
        else:
            skip(results, MODULE_ID, "GET /api/ddns defaults", "device already configured")
        try:
            if _keeps_secrets(orig):
                skip(results, MODULE_ID, "secrets are write-only", SECRETS_SKIP)
            else:
                check_secret_write_only(results, spa)
            check_validation_errors(results, spa)
            check_json_only(results, spa)
            check_force_rate(results, spa)
            check_status_has_ddns(results, spa)
            check_spa_has_ddns_tab(results, spa)
        finally:
            _restore(results, spa, orig)
    finally:
        spa.close()


# ---- live (internet) ----

ANYTHING = "https://httpbin.org/anything?ip={ip}"


def _ddns(spa: SpaClient) -> dict:
    return spa.fetch_json("/api/ddns") or {}


# A save right after a provider call waits out the spec's 60 s call spacing first.
def _wait_status(spa: SpaClient, pred, timeout: float = 90.0) -> dict:
    end = time.time() + timeout
    st: dict = {}
    while time.time() < end:
        st = _ddns(spa).get("status") or {}
        if pred(st):
            break
        time.sleep(2)
    return st


def _save_live(spa: SpaClient, url: str, match: str = "") -> int:
    r = spa.post_json("/api/ddns", {"enabled": True, "url": url, "method": "GET", "header": "", "body": "",
                                    "match": match, "src_ip": False, "token_clear": True, "pass_clear": True})
    return r.get("__http_status") or 0


def _relogin(spa: SpaClient, ctx: Context) -> None:
    spa.page.goto(ctx.esp_host + "/", wait_until="domcontentloaded", timeout=15000)
    spa.page.wait_for_timeout(800)
    if spa.page.locator("#authLogin").is_visible():
        spa.page.fill("#authLogin input[type=password]", ctx.web_pw)
        spa.page.click("#authLogin button[type=submit], #authLogin .btn-primary")
        spa.page.wait_for_timeout(1500)


def _snmp_outputs(host: str, community: str) -> tuple[str, str]:
    _, _, line1 = snmp_getnext(host, community, f"{EXT}.3.1.1")
    _, _, full = snmp_getnext(host, community, f"{EXT}.3.1.2")
    return str(line1), str(full)


def check_quorum_live(results: list[Result], spa: SpaClient, host: str, community: str) -> dict:
    code = _save_live(spa, ANYTHING, '"url"')
    st = _wait_status(spa, lambda x: x.get("state") == "ok")
    ip = st.get("quorum_ip") or ""
    try:
        public = ipaddress.ip_address(ip).is_global
    except ValueError:
        public = False
    check(results, MODULE_ID, "live: quorum ok with a public IP",
          code == 200 and st.get("state") == "ok" and public,
          f"save={code} state={st.get('state')} detail={st.get('detail')!r} quorum={ip!r} "
          f"last_http={st.get('last_http')} err={st.get('last_error')!r}")
    srcs = st.get("sources") or []
    check(results, MODULE_ID, "live: all three sources asked and shown",
          len(srcs) == 3 and all(x.get("asked") and (x.get("ip") or x.get("error")) for x in srcs),
          f"sources={srcs!r}")
    # The admin's own response snippet may echo the URL (web UI only, by spec); nothing else may.
    leak = "httpbin.org/anything"
    others = json.dumps({k: v for k, v in st.items() if k != "snippet"})
    top = json.dumps((spa.fetch_json("/api/status") or {}).get("ddns"))
    line1, full = _snmp_outputs(host, community) if community else ("", "")
    check(results, MODULE_ID, "live: URL not in status/SNMP",
          leak not in others and leak not in top and leak not in line1 and leak not in full
          and st.get("provider_host") == "httpbin.org",
          f"provider_host={st.get('provider_host')!r} line1={line1!r} full={full!r}")
    return st


def check_exit_node_quorum(results: list[Result], spa: SpaClient, ctx: Context, quorum_ip: str) -> None:
    exit_ip = os.environ.get("EXIT_NODE_IP", "")
    if not exit_ip or ctx.pi_host in ("", "unused"):
        skip(results, MODULE_ID, "live: quorum ignores the exit node", "EXIT_NODE_IP / test Pi not configured")
        return
    from .common import SshClient
    from .routing import _pi_curl_public_ip, _set_exit_node
    saved = ((spa.fetch_json("/api/tailscale") or {}).get("settings") or {}).get("exit_node_ip", "")
    pi = SshClient(ctx.pi_host, ctx.pi_user, password=ctx.pi_pw, timeout=10)
    try:
        _set_exit_node(spa, exit_ip)
        time.sleep(10)
        exit_public = _pi_curl_public_ip(pi)
        spa.post_json("/api/ddns/force", {})
        st = _wait_status(spa, lambda x: x.get("state") == "ok" and x.get("next_check_s", 0) > 30, 60)
        check(results, MODULE_ID, "live: quorum ignores the exit node",
              bool(exit_public) and st.get("quorum_ip") not in ("", exit_public),
              f"quorum={st.get('quorum_ip')!r} exit_public={exit_public!r} (direct {quorum_ip!r})")
    finally:
        _set_exit_node(spa, saved)
        pi.close()


def check_redirect_not_followed(results: list[Result], spa: SpaClient) -> None:
    code = _save_live(spa, "https://httpbin.org/redirect-to?url=https%3A%2F%2Fexample.com&ip={ip}")
    st = _wait_status(spa, lambda x: x.get("last_http") == 302 or x.get("state") == "stopped")
    check(results, MODULE_ID, "live: redirect not followed",
          code == 200 and st.get("last_http") == 302 and st.get("last_error") == "redirect (3xx)",
          f"save={code} state={st.get('state')} last_http={st.get('last_http')} err={st.get('last_error')!r}")


def _reboot(spa: SpaClient, ctx: Context) -> None:
    spa.post_json("/api/system/restart", {})
    time.sleep(18)
    end = time.time() + 60
    while time.time() < end:
        try:
            _relogin(spa, ctx)
            if _ddns(spa).get("status"):
                return
        except Exception:  # noqa: BLE001 — device still booting
            time.sleep(3)


def check_force_clears_stop(results: list[Result], spa: SpaClient, ctx: Context) -> None:
    """badauth-style stop, then Force: the lifted stop must not come back after a reboot.

    Force is pressed while the 60 s call spacing still holds the forced attempt, and the device is
    rebooted before it runs. A lifted stop shows as a provider call after boot (last_http 401 again);
    a stop restored from flash shows as no call at all (last_http 0)."""
    code = _save_live(spa, "https://httpbin.org/status/401?ip={ip}")
    st = _wait_status(spa, lambda x: x.get("state") == "stopped")
    f = spa.post_json("/api/ddns/force", {}) or {}
    time.sleep(5)
    _reboot(spa, ctx)
    after = _wait_status(spa, lambda x: (x.get("last_http") or 0) != 0, 90)
    check(results, MODULE_ID, "live: Force lifts a stored stop across reboot",
          code == 200 and st.get("state") == "stopped" and f.get("__http_status") in (None, 200)
          and after.get("last_http") == 401,
          f"save={code} before={st.get('state')} force={f.get('__http_status')} {f.get('__body', '')} "
          f"after={after.get('state')} detail={after.get('detail')!r} last_http={after.get('last_http')}")


def check_sta_retry_no_starvation(results: list[Result], spa: SpaClient, ctx: Context) -> None:
    """ETH up, Wi-Fi uplink retrying an absent SSID: an update must still reach the provider."""
    name = "live: Wi-Fi retries do not block updates"
    if os.environ.get("DDNS_STA_CHURN") != "1":
        skip(results, MODULE_ID, name, "DDNS_STA_CHURN=1 not set (reconfigures Wi-Fi, two reboots)")
        return
    net = spa.fetch_json("/api/network") or {}
    if net.get("networks"):
        skip(results, MODULE_ID, name, "device has Wi-Fi networks configured")
        return
    try:
        spa.post_json("/api/network", {"networks": [{"ssid": "ddns-test-absent", "password": "absent-pass-123"}]})
        spa.post_json("/api/wifi-uplink", {"enabled": True})
        _reboot(spa, ctx)
        code = _save_live(spa, ANYTHING, '"url"')
        st = _wait_status(spa, lambda x: x.get("state") == "ok" and x.get("last_http") == 200, 120)
        sta = (spa.fetch_json("/api/status") or {}).get("sta") or {}
        check(results, MODULE_ID, name,
              code == 200 and sta.get("uplink_en") and not sta.get("connected")
              and st.get("state") == "ok" and st.get("last_http") == 200,
              f"save={code} sta={sta.get('uplink_en')}/{sta.get('connected')} state={st.get('state')} "
              f"detail={st.get('detail')!r} last_http={st.get('last_http')}")
    finally:
        spa.post_json("/api/wifi-uplink", {"enabled": False})
        spa.post_json("/api/network", {"networks": []})
        _reboot(spa, ctx)


def check_stopped_on_401(results: list[Result], spa: SpaClient, ctx: Context) -> None:
    code = _save_live(spa, "https://httpbin.org/status/401?ip={ip}")
    st = _wait_status(spa, lambda x: x.get("state") == "stopped")
    check(results, MODULE_ID, "live: 401 stops updates",
          code == 200 and st.get("state") == "stopped",
          f"save={code} state={st.get('state')} detail={st.get('detail')!r} last_http={st.get('last_http')}")
    _reboot(spa, ctx)
    time.sleep(20)  # give a (wrongly) running check time to reach the provider
    st = _ddns(spa).get("status") or {}
    check(results, MODULE_ID, "live: stopped survives reboot without a provider call",
          st.get("state") == "stopped" and st.get("last_http") == 0,
          f"state={st.get('state')} detail={st.get('detail')!r} last_http={st.get('last_http')}")
    code = _save_live(spa, ANYTHING, '"url"')
    st = _wait_status(spa, lambda x: x.get("state") not in ("stopped", None, ""), 30)
    check(results, MODULE_ID, "live: saving leaves stopped",
          code == 200 and st.get("state") not in ("stopped", None, ""),
          f"save={code} state={st.get('state')}")


def run_live(results: list[Result], ctx: Context, host: str, community: str) -> None:
    names = ("live: quorum ok with a public IP", "live: all three sources asked and shown",
             "live: URL not in status/SNMP", "live: quorum ignores the exit node",
             "live: redirect not followed", "live: 401 stops updates",
             "live: stopped survives reboot without a provider call", "live: saving leaves stopped",
             "live: Force lifts a stored stop across reboot", "live: Wi-Fi retries do not block updates")
    if os.environ.get("DDNS_LIVE") != "1":
        for n in names:
            skip(results, MODULE_ID, n, "DDNS_LIVE=1 not set (needs internet)")
        return
    spa = SpaClient(ctx)
    orig = _ddns(spa)
    if _keeps_secrets(orig):
        for n in names:
            skip(results, MODULE_ID, n, SECRETS_SKIP)
        spa.close()
        return
    try:
        st = check_quorum_live(results, spa, host, community)
        check_exit_node_quorum(results, spa, ctx, st.get("quorum_ip") or "")
        check_redirect_not_followed(results, spa)
        check_stopped_on_401(results, spa, ctx)
        check_force_clears_stop(results, spa, ctx)
        check_sta_retry_no_starvation(results, spa, ctx)
    finally:
        spa.post_json("/api/ddns", {"enabled": False})
        _restore(results, spa, orig)
        spa.close()


def run(ctx: Context) -> list[Result]:
    results: list[Result] = []
    run_api(results, ctx)
    host = urlparse(ctx.esp_host).hostname or ctx.esp_host
    community = os.environ.get("ESP_SNMP_COMMUNITY", "")
    if not community:
        snmp = SpaClient(ctx).fetch_json("/api/snmp") or {}
        if snmp.get("enabled") and snmp.get("running"):
            community = snmp.get("community") or ""
    if not community:
        skip(results, MODULE_ID, "snmp extend", "SNMP disabled on the device")
    else:
        check_snmp_extend(results, host, community)
    run_live(results, ctx, host, community)
    return results
