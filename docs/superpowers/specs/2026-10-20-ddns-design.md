> Implementation follow-up after independent review: DDNS now uses nonblocking
> ESP-TLS and the HTTP parser, with asynchronous lwIP DNS and one 20-second
> deadline. The validated IPv4 is reused with the original TLS/HTTP hostname.
> Save returns busy after a 200 ms lock wait. Save-time filling uses real
> credentials and a 15-character IPv4. Status-only polling preserves edits.
> The original transport details below describe the initial design; see
> `components/ddns/ddns_http.c` and `docs/CONFIGURATION.md` for current behavior.

# Dynamic DNS client: design

Date: 2026-10-20 · Status: draft for approval (revision 3, after the
security, resource and edge-case review)

## Intent

The device is often installed at a remote site, for example a relative's home
network that someone looks after over Tailscale. Tailscale is the main way into
that site. Dynamic DNS is an extra service on the same box: it keeps one
hostname pointed at the site's current public IPv4 address, so other services
that need the WAN IP can find the site without anyone logging into the router.

The device is short on memory, so the feature must be small. It runs as one
task that sleeps between checks. It reuses the TLS stack, the certificate
bundle, `esp_http_client` and cJSON, all of which the firmware already links.
It adds no new libraries.

## Goals

- Work with any provider that can be updated with **one HTTP request**. The
  user pastes the provider's update URL with placeholders. Optionally they also
  set a method (POST, PUT or PATCH), one extra header and a body template.
  There is no per-provider code. Against pfSense's provider list, this covers
  39 of its 52 IPv4 providers (see "Provider coverage").
- Find the public IP safely: ask three independent HTTPS sources and require
  two of them to agree.
- Run every N minutes, again when the WAN uplink changes, and when the user
  presses **Force update now**.
- Show status in the web UI and over SNMP, using an existing well-known MIB.
- Be ready for upstream (Csontikka/esp32-tailscale-subnet-router) without
  changes.

## Non-goals (document these as limits)

- IPv6 / AAAA records. Only IPv4 is supported.
- Providers that need more than one request: zone or record ID lookup, cookie
  login, OAuth token exchange, or request signing. Examples: DigitalOcean,
  Linode, Route 53, Azure, Hover. Cloudflare works only with a zone ID and
  record ID the user enters.
- Providers that only accept plain `http://`, such as DyNS.
- More than one update profile.
- Keeping the last successfully sent IP across reboots. It lives in RAM only,
  so the first successful check after each boot sends one update. (The
  "stopped" state *is* kept, see below.)
- SNMP SET and SNMP traps.
- Tailscale STUN as an IP source. It was considered and dropped (decided with
  the user). Microlink keeps its last STUN result with no timestamp, so after
  a WAN change it can vote with the old address.

## Anti-goals (failure even if it "works")

- **Getting the user's provider account flagged.** Under dyndns2 rules,
  repeated updates with an unchanged IP, or retrying after an auth or hostname
  rejection, count as abuse. That includes doing either once per boot in a
  reboot loop.
- **Publishing a wrong IP.** This includes the Tailscale exit node's IP, a
  private or CGNAT address, or the answer of one misbehaving or tampered
  source.
- **Leaking secrets.** The password and token must never appear in API
  responses, logs, the SD log, the pre-crash log capture, SNMP output or the
  status text. Redirects and header injection must not leak them either.
- **Hurting the router.** No blocking the httpd task. No large internal-RAM
  allocations. No unbounded work from a slow or hostile server. No network
  traffic at all while the feature is disabled.

## Constraints

- **Upstream-clean.** No dependency on the W5500 layer. Upstream has no
  Ethernet, no `eth_connect`/`sta_connect` and no exported
  `netif_is_uplink()`. It uses `find_sta_netif()` and the `ap_connect` flag.
  The feature therefore relies only on what both codebases have:
  - "Has an uplink" is the `ap_connect` flag. On the fork that flag means
    Ethernet or Wi-Fi.
  - The uplink-change triggers are default-loop handlers for
    `IP_EVENT_STA_GOT_IP` / `IP_EVENT_STA_LOST_IP` and `IP_EVENT_ETH_GOT_IP` /
    `IP_EVENT_ETH_LOST_IP`. These are standard ESP-IDF events, so registering
    the ETH ones upstream is harmless; they never fire there.
  - The fork's `uplink_state_changed()`, which also covers an ETH-down → STA
    failover that fires no got-IP event, calls one exported
    `ddns_notify_uplink_changed()`. Upstream calls it from its equivalent
    place.
  - The feature code (task, quorum, request building, SNMP rows) should apply
    to `upstream/main` unchanged. Only the wiring in `web_ui.c`/`index.html`
    and `main.c` should need per-branch adjustment.
  - It follows the upstream CI checks: clang-format 18 on changed lines, 120
    columns, cppcheck clean, sensitive-check clean.
- **No sdkconfig changes.**
  - HTTP Basic auth is turned off in sdkconfig, so the `Authorization: Basic`
    header is built in code (mbedTLS base64 is already linked).
  - `CONFIG_MBEDTLS_HAVE_TIME_DATE` is off firmware-wide, so certificate
    expiry is **not** checked for any TLS connection: OTA, telemetry, the
    control plane, and this feature too. This is accepted, and the DDNS docs
    say so (decided with the user). Chain and hostname validation still
    apply.
- **Off by default.**
- **The DDNS task never touches NVS or flash.** Its stack is in PSRAM
  (`CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`). Flash writes disable the
  cache, and a PSRAM stack depends on it. The one write the task needs
  (saving the "stopped" state) is handed to an `esp_timer` callback, which
  runs on the internal-RAM esp_timer task.
- **Flash budget.** The app is about 1.63 MB of a 1.92 MB OTA slot. Growth
  should be a few tens of KB at most. CI reports the size.
- **Web handler slots.** 63 of 72 are used on the fork and 59 of 72
  upstream. This feature uses at most three. The httpd stack is 12 KB, so no
  large settings or request buffers go on it.
- **NVS.** The partition is 64 KB and has fragmented before. The settings are
  one versioned blob (see "Settings"), and they are written only on save or
  on a rejection.

## Behaviour

### Triggers

- **Periodic.** Every N minutes. N ranges 1–1440 and defaults to 5.
- **Uplink change.** Any uplink got-IP, lost-IP or failover event (see
  Constraints) increments the **uplink epoch**. When "Also when WAN gets an
  IP" is on (the default), it also schedules a check about 10 s later, so
  routes and DNS can settle.
- **Force update now** (web UI). This runs a check right away. Within the
  abuse limits below, it sends even if the IP hasn't changed, and it clears
  the "stopped" state. Force can be used at most once per 60 s; the button
  shows how long until it can be used again.
- **Time not yet synced.** Until time is synced, the task re-checks the
  prerequisites every 60 s instead of waiting a full interval.
- Checks never run concurrently. A trigger that arrives during a check sets
  a "rerun pending" flag, and exactly one more check runs afterwards.
- **Timekeeping.** All deadlines use 64-bit monotonic `esp_timer_get_time()`.
  Tick math must avoid the `pdMS_TO_TICKS` overflow noted in
  `main/telemetry.c:410`. Only the "refresh every N days" deadline uses wall
  time, and it is clamped if the clock jumps backwards.

### Consistency (generation guard and dispatch boundary)

- A **config generation** increments on every successful settings save and
  every enable or disable. A check starts by copying the active settings and
  recording the config generation and the uplink epoch, under the DDNS mutex.
- **Dispatch boundary.** The task takes the mutex, re-checks that both the
  config generation and the uplink epoch are unchanged, and records the
  attempt time used by the 60 s limit. It keeps the mutex until the request
  has been written to the socket, then releases it.
  - A settings save waits on the same mutex, so it can never land between
    the check and the send. The wait is bounded by the request's connect +
    TLS + write time, at most the 20 s request budget.
  - If either counter changed, the check gives up without sending, and the
    pending rerun takes over.
- **After the response**, results go into status only if the generation
  still matches. Attempt timestamps and cooldowns are **always** recorded,
  even for a request whose settings are now out of date. They apply to the
  account, not to one particular settings version.
- **Disabling** only makes the task idle. The task is never deleted (see
  Resources).

### Abuse limits

- At most one provider call per 60 s from any trigger, Force included.
- **HTTP 429** sets a cooldown: `Retry-After` in seconds (a date isn't
  parsed), capped at 24 h, or 10 min when the header is absent.
- **`dnserr` or `911`** set a 30 min cooldown (the dyndns2 rule).
- **During a cooldown**, IP checks continue, but no provider call is made,
  not even on Force. The status shows when the cooldown ends.
- **Ambiguous outcomes back off:** a truncated response, a timeout, or a 2xx
  that doesn't match. The wait between provider attempts doubles each time,
  starting at the interval and capped at 1 h, until a success.

### Prerequisites

The feature is enabled, an uplink exists (`ap_connect`), no OTA is running,
and the clock is plausible (`now >= 1700000000`, the same test as `ota.c`). If
one is missing, the status names it and the check is skipped.

### Public IP: quorum of 3 HTTPS sources

- There are three source slots, and each must name a different source.
  - Presets: `https://api.ipify.org`, `https://ipv4.icanhazip.com`,
    `https://checkip.amazonaws.com`, `https://ifconfig.me/ip`.
  - **Custom URL**, one per slot.
  - **At most one of the three may be `http://`.** Several plain-HTTP
    sources could all be faked by one attacker on the network path, which
    would defeat the quorum.
  - Defaults: ipify, icanhazip, AWS checkip.
- **Algorithm.** Ask all three slots on every check (changed 2026-10-20 at the
  user's request: a slot 3 shown as "not asked" was confusing). Accept any IP
  that two of the three agree on. If no two agree, nothing is updated. The status shows each
  source's answer or error, and the next trigger retries.
- **A valid answer** is HTTP 2xx with a body of at most 64 bytes, with no NUL
  or other control bytes except surrounding whitespace. After trimming, it
  must be exactly one dotted-quad IPv4 address. The address must not be
  private, loopback, link-local, CGNAT (100.64/10), multicast or reserved.
  Anything else counts as a failure of that source.
- **Routing.** In exit-node mode, the device's own traffic to public
  addresses already goes out the uplink, not the tunnel. This is the route
  hook's "self-origin public destination" branch (fork
  `main/lwip_route_hook.c:663`, upstream `:613–650`). So no interface pinning
  is used. A functional test checks that with an exit node selected, the
  quorum IP is not the exit node's IP.
- **CGNAT ISPs.** Behind carrier-grade NAT, the sources return the
  carrier's public address. The update "works", but the address can't
  receive incoming connections. Document this.

### Request building

- **Template parts (decided with the user).**
  - **URL**, required and always `https://`. It takes the placeholders
    `{ip}`, `{user}`, `{pass}` and `{token}`. Placeholders are only allowed
    in the userinfo, path and query parts. Values are percent-encoded,
    except `{ip}`.
  - **Method**: GET (the default), POST, PUT or PATCH.
  - **Extra header**: optional and allowed with **any** method, including
    GET (ClouDNS needs it). One `Name: value` line; the template is at most
    256 bytes. Values go in verbatim, then the result is checked (below).
  - **Body template**: optional, for POST/PUT/PATCH only. At most 512 bytes
    of template. The type is either form (`application/x-www-form-urlencoded`,
    values percent-encoded) or JSON (`application/json`). In JSON,
    placeholders are only allowed inside string values, and the values are
    JSON-escaped.
- **Each placeholder is filled exactly once.** Filled-in values are never
  scanned for placeholders again.
- **Size caps after filling in:** URL ≤ 512 bytes, header line ≤ 512 bytes,
  body ≤ 1024 bytes. Size math is overflow-checked. Exceeding a cap is a
  configuration error, and nothing is sent. The client's `buffer_size_tx` is
  set to at least the URL cap plus headroom; the default 512 would reject a
  long request line.
- **Checks on the filled-in result, before anything is sent:**
  - **Header.** The name must be a valid token. The value must contain no
    CR, LF, NUL or other control characters. The name must not be one of
    `Host`, `Content-Length`, `Transfer-Encoding`, `Connection` or `Cookie`.
  - **JSON body.** It must parse with cJSON.
  - **URL.** It must parse with the same `http_parser` the client uses. If
    it fails, the error is reported as "invalid URL" and the URL itself is
    never passed on or logged. This matters because the client logs the
    whole URL at ERROR level when its own parse fails
    (`esp_http_client.c:1140`).
- **Auth precedence.**
  - If the extra header is an `Authorization` header, no Basic auth is
    added. GoDaddy's `sso-key` and Bearer tokens work this way.
  - Otherwise Basic auth is sent when the URL has `{user}:{pass}@` (that part
    is removed from the request URL) or when User and Password are filled in.
- **Source-IP providers (decided with the user).** "Provider uses the
  request's source IP" makes `{ip}` optional, for providers such as DY.fi and
  OpenDNS. Without it, saving a template with no `{ip}` anywhere is refused.
  With it on, the quorum still decides *when* to send, but not what the
  provider publishes.
- **Destination policy.**
  - Before connecting, the provider host and every custom IP-source host is
    resolved.
  - A host is refused if it resolves to a private, loopback, link-local,
    CGNAT or tailnet (100.64/10) address. This prevents SSRF-style requests
    into the LAN or the tailnet.
  - There is no override. A DNS-rebinding race is still possible, which is
    acceptable for an admin-only setting.
- **TLS.** Every HTTPS request attaches the certificate bundle, and hostname
  checking stays on. Certificate dates aren't checked (see Constraints).
- **Redirects are off.** `disable_auto_redirect = true` on every request. A
  3xx counts as a temporary failure with the status "redirect (3xx)". The
  client would otherwise follow redirects, keep the `Authorization` header,
  and accept an `http` downgrade.

### Secrets

- **Secrets stay out of the templates (decided with the user).** The URL,
  header and body contain only placeholders. The real values live in the
  hidden User, Password and Token fields. On save:
  - a literal `user:pass@` (one that isn't `{user}:{pass}@`) is moved into
    the User and Password fields, and the URL is rewritten to
    `{user}:{pass}@`.
  - A query or form field, or a JSON key, holding a literal value is
    refused, with a hint to use `{token}` or `{pass}`. This covers the names
    `token`, `key`, `password`, `pass`, `pwd`, `secret`, `apikey`,
    `secretapikey` and `q`. The same applies to a header whose name contains
    `auth`, `key`, `token` or `secret`, unless its value has a placeholder.
  - These checks are **best effort**. A credential in a path segment, or
    under an unusual name, can get through. That's why the second rule
    exists.
- **What leaves the web UI.**
  - Logs, the SD log, the pre-crash capture and SNMP carry only the
    hostname, an error code and the HTTP status.
  - They never carry a template, a filled-in request or a response body.
  - The web UI shows the admin their own templates, and a response snippet
    of at most 64 printable-ASCII characters. In that snippet, the stored
    password and token are replaced by `***`, in raw, percent-encoded and
    base64 form.
- **RAM lifetime.**
  - The filled-in URL, header, body and Basic credentials live in PSRAM
    buffers owned by the task.
  - They are wiped (`mbedtls_platform_zeroize`) and freed on every exit
    path.
  - The settings copy held during a check is wiped the same way.
- **Known limits, to document.** NVS isn't encrypted, and flash core dumps
  include task stacks. Both are already true for the Wi-Fi and Tailscale
  secrets.

### Send rule and response handling

- **Send** when any of these holds:
  - the check was **forced**;
  - the pair (quorum IP, provider fingerprint) differs from the last
    successful pair;
  - a **refresh is due**, meaning "refresh every N days" is on and N days
    have passed since the last success.

  The provider fingerprint is a hash of every request setting: URL, method,
  header, body, body type, the source-IP flag, user, password and token.
  The last pair lives in RAM.
- **Reading the response.** The client uses `open` → `fetch_headers` →
  `read` in a loop rather than `perform`.
  - A total budget of **20 s** per request covers DNS, connect, TLS and
    body, checked against `esp_timer` between reads.
  - Reading stops after **1024 bytes** of body. The client is closed as soon
    as the result is known.
  - A body cut off by the time limit or a connection drop is a temporary
    failure. A body cut off by the byte limit is classified using the
    first 1024 bytes.
  - The body is copied into a PSRAM buffer and trimmed. Its first word is
    the text before the first space or newline.
- **Classification**, first match wins:
  1. **Permanent rejection ("stopped").** HTTP 401, 403 or 404, or a first
     word of `badauth`, `nohost`, `notfqdn`, `numhost`, `!donator`, `abuse`
     or `badagent`.
     - The "stopped" state and its reason are saved to NVS through the
       esp_timer handoff, so a reboot loop can't retry it.
     - No provider calls are made until the settings are saved again or
       Force is pressed.
     - IP checks continue.
  2. **Cooldown.** HTTP 429, or a first word of `dnserr` or `911`.
  3. **Redirect.** Any 3xx is a temporary failure, with the status "redirect
     (3xx)".
  4. **Success.** HTTP 2xx plus the success match. The last pair and its
     time are updated.
  5. **Temporary failure.** Anything else, with back-off as described above.
- **Success match (decided with the user).** The field holds `|`-separated
  alternatives, compared case-sensitively.
  - Each alternative is a plain substring; spaces and punctuation are
    allowed.
  - A leading `^` anchors the alternative to the start of the trimmed body.
  - Empty means "any 2xx".
  - About 20 lines of C, using `strncmp`/`strstr`, no regex.
  - Help examples: `^good|^nochg` (dyndns2-style), `OK` (DuckDNS),
    `<ErrCount>0</ErrCount>` (Namecheap). For JSON APIs that signal errors
    with the HTTP status (Cloudflare, Gandi, DNSimple, GoDaddy), leave it
    empty. A JSON substring such as `"success":true` breaks on whitespace,
    and the field can sit beyond 1024 bytes.

### Status (RAM)

- **State:** `disabled`, `waiting` (with the missing prerequisite), `ok`,
  `quorum-failed`, `provider-error`, `cooldown` (with its end time) or
  `stopped` (with its reason).
- Each source's last answer or error.
- The quorum IP.
- The last successful IP and time.
- The last HTTP status and error code.
- The redacted 64-character snippet, shown in the web UI only.
- The time of the next check.
- The status is updated under the mutex, and readers copy a snapshot.

## Settings (NVS)

The settings are stored as **one versioned blob** under key `ddns_cfg`,
holding a fixed struct with a `version` byte. This makes saves
all-or-nothing:

1. Validate the complete candidate.
2. Write the blob and commit.
3. Only then swap it into use, under the mutex.

A failed write keeps the old settings and returns an error; it never causes
a reboot. A separate small key, `ddns_stop`, holds the stopped reason.

| Field | Limit | Notes |
|---|---|---|
| enabled | bool | default off |
| url | 256 | template |
| method | enum | GET / POST / PUT / PATCH |
| header | 256 | template, optional |
| body | 512 | template, optional |
| body type | enum | form / JSON |
| source-IP flag | bool | `{ip}` optional when set |
| user | 64 | |
| pass | 128 | write-only |
| token | 128 | write-only |
| success match | 64 | `\|`-separated alternatives |
| sources 1–3 | enum + custom URL (128 each) | |
| interval | 1–1440 min | default 5 |
| WAN trigger | bool | default on |
| refresh | 0–365 days | default 0 (off) |

The blob is about 2 KB. It is copied into the task under the mutex at the
start of each check, then wiped afterwards.

**Secrets** follow the existing write-only pattern:

- GET returns `pass_set`/`token_set` and their lengths, never the values.
- On POST, an empty field keeps the stored value, and an explicit clear
  flag erases it.
- Saving clears the stopped state, both in RAM and in `ddns_stop`.

## Web API

The handlers use the existing pattern in `web_ui.c` (`require_auth`,
`reg_uri`). Authentication is the existing `SameSite=Strict` session cookie,
or open when no web password is set, the same as the rest of the UI. There
are three handlers:

- **`GET /api/ddns`** returns the settings (secrets masked) plus a status
  snapshot.
- **`POST /api/ddns`** accepts only `Content-Type: application/json`, which
  matches the existing cross-site protection. It validates the whole
  candidate and stores it as described above. The checks:
  - the interval, refresh and length ranges
  - the three sources are all different
  - at most one source is `http://`
  - the update URL is `https://`
  - `{ip}` appears somewhere, unless the source-IP flag is set
  - the placeholder syntax and placement
  - the literal-secret rules
  - no body with GET
  - the JSON body still parses after dummy values are filled in

  Changes apply at once, with no reboot.
- **`POST /api/ddns/force`** accepts only JSON. It queues a forced check and
  returns at once, giving the remaining wait if Force was used in the last
  60 s. It never makes httpd wait on network I/O.

## Web UI (decided with the user)

- A new top-level **DDNS** tab with one **Dynamic DNS** card. The card has,
  in order:
  - Enabled
  - Update URL, with help text on the placeholders and the limits (IPv4
    only, one request, HTTPS, certificate dates not checked)
  - "Provider uses the request's source IP"
  - **Advanced request**, collapsed by default: Method, Extra header, and
    Body template + Body type (shown only for methods other than GET)
  - User, Password, Token
  - Success if response contains
  - IP source 1/2/3 (a preset or Custom URL)
  - Check every N min, Also when WAN gets an IP, Refresh every N days
  - A status block
  - Save and Force update now (with a cooldown hint)
- **Password and Token** show a "Set (N chars), leave empty to keep"
  placeholder and a clear control. An **eye icon at the end of each field
  reveals only what is being typed**; the stored value never comes back from
  the device.
- **Status tab:** one extra row in the active uplink's card, showing the
  public IP, the DDNS hostname or state, and the last update.

## SNMP

The status is served through **NET-SNMP-EXTEND-MIB**
(`1.3.6.1.4.1.8072.1.3.2`), which common pollers already understand. The
private `99999` tree must not grow. There is one row, with index `"ddns"`,
encoded as the OID string `4.100.100.110.115`.

**Output table** (`…8072.1.3.2.3.1.<col>.<idx>`):

| Column | Object | Value |
|---|---|---|
| — | `nsExtendNumEntries.0` = `…8072.1.3.2.1.0` | 1 |
| 1 | `nsExtendOutput1Line` | e.g. `ok 84.2.17.203`, `quorum-failed`, `cooldown 429`, `stopped badauth`, `disabled` |
| 2 | `nsExtendOutputFull` | each source's answer, the last success IP and time, and the last HTTP status and error code. **No response snippet.** At most 255 bytes. |
| 3 | `nsExtendOutNumLines` | the number of lines in OutputFull |
| 4 | `nsExtendResult` | 0 ok, 1 disabled, 2 waiting, 3 quorum-failed, 4 provider-error, 5 cooldown, 6 stopped |

**Config table** (`…8072.1.3.2.2.1.<col>.<idx>`), minimal and read-only, so
that tools which walk the config table first still find the row:

| Column | Object | Value |
|---|---|---|
| 2 | `nsExtendCommand` | `"ddns"` |
| 3 | `nsExtendArgs` | `""` |
| 4 | `nsExtendInput` | `""` |
| 5 | `nsExtendCacheTime` | 0 |
| 6 | `nsExtendExecType` | exec(1) |
| 7 | `nsExtendRunType` | run-on-read(1) |
| 20 | `nsExtendStorage` | readOnly(5) |
| 21 | `nsExtendStatus` | active(1) |

Rules for the agent:

- Rows are present even when DDNS is disabled.
- `DisplayString` values are capped at 255 bytes by the MIB, and every
  output value comes from one status snapshot.
- `MAX_OID` in `components/snmp_agent/snmp_agent.c` goes from 16 to 24.
- These entries go into the GETNEXT order before `…99999`.
- `snmp_agent` does not depend on DDNS: DDNS registers a status-snapshot
  callback with the agent.
- Plain walks find the row. Alerting is a manual step for the user (decided
  with the user). Document an example item on `nsExtendResult."ddns" >= 3`.

## Resources

- **One persistent FreeRTOS task**, with its stack in PSRAM
  (`xTaskCreateWithCaps(…, MALLOC_CAP_SPIRAM)`), created on the first enable
  and never deleted. Disabling makes it idle and waiting. The stack is sized
  for a TLS handshake (start at 8192, then measure). It never touches NVS or
  flash.
- **It waits on a task notification** with a timeout, owned by DDNS. Event
  handlers are registered once at init and never removed. They and the
  Force endpoint only bump counters and notify the task.
- **All buffers are PSRAM, task-owned, bounded and wiped:**
  - request: 512 + 512 + 1024 bytes
  - response: 1024 bytes
  - IP answers: 64 bytes each
- **The client handle** is created and cleaned up inside one function, with
  a single cleanup exit used by every error path. The body pointer given to
  `esp_http_client_set_post_field` (which only borrows it) outlives the
  request, and is freed only after `esp_http_client_cleanup`.
- **TLS memory** comes from PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`, 16 KB
  in + 4 KB out). Checks are skipped while an OTA is running. The plan
  includes measuring on hardware: the stack high-water mark, the minimum
  free internal and PSRAM heap, and the largest free internal block, during
  a check that overlaps telemetry and the Tailscale reconnect.

## Testing and docs

- **Host-side C unit tests**, built with plain gcc in `tools/` (e.g.
  `tools/ddns_host_test/`). They cover the pure functions:
  - placeholder filling and encoding (URL, form, JSON), including
    fill-once and size caps
  - the checks on the filled-in header, JSON and URL
  - IP answer validation and the quorum
  - the success matcher and the response classification
  - the send rule, including refresh and fingerprint changes
  - the state transitions and cooldown/back-off arithmetic, including
    64-bit time and clock-jump cases
  - the literal-secret detection and snippet redaction

  The DDNS core is written so these functions don't depend on ESP-IDF.
  Whether to wire the tests into CI is upstream's call.
- **Device functional tests**, in `tools/test_lib/ddns.py` registered in
  `tools/run_tests.py`, following `docs/TESTING.md`:
  - API validation
  - secrets never returned
  - keep-if-empty and clear
  - JSON-only POST
  - Force returns at once and its cooldown is reported
  - the SNMP extend OIDs exist and walk in order (including the config
    columns and the order before `99999`)
  - in exit-node mode, the quorum IP is not the exit node's IP
- **Hardware measurements**, as described under Resources.
- **Docs:**
  - a new `## DDNS` section in `docs/CONFIGURATION.md`, covering:
    - the fields and behaviour
    - the quorum
    - the stopped, cooldown and back-off states
    - **limits:** IPv4 only, one request, one profile, HTTPS only, no
      STUN, the last IP not persisted, certificate dates not checked, CGNAT,
      NVS not encrypted
    - the provider coverage table, with ready-to-paste examples
    - the pfSense credit
  - a row in the SNMP "What it serves" table
  - a `CHANGELOG.md` entry

## Provider coverage

**Credit.** The provider survey is based on pfSense CE's DynDNS client. The
provider list, request shapes and dyndns2 return-code handling come from
`src/etc/inc/dyndns.class` in pfsense/pfsense `master` at `9363ac5`
(2026-03-31), the code base for pfSense CE 2.9.0, Apache License 2.0. No
pfSense code is copied; it served as the reference list and for behaviour.
This credit goes in `docs/CONFIGURATION.md` next to the table, and in the PR
descriptions.

pfSense lists 52 IPv4 entries, not counting "Custom". DynDNS has 3 entries and
No-IP 2, so there are about 49 distinct services.

| Group | Count | Providers |
|---|---|---|
| Single GET | 32 | All-Inkl, City Network, ClouDNS, deSEC, DNSexit, DNS Made Easy, DNS-O-Matic, Domeneshop, DuiaDns, DynDNS ×3, Dynv6, easyDNS, EuroDNS, freeDNS v1/v2, GratisDNS, HE.net, HE Tunnelbroker, Loopia, Namecheap, NIC.RU, No-IP, No-IP free, OVH, SelfHost, SPDYN, Strato, ZoneEdit, DY.fi and OpenDNS (both via "source IP") |
| Single request, other than GET | 7 | GleSYS, Mythic Beasts (form POST + Basic), GoDaddy (PUT JSON, `Authorization: sso-key` header), Gandi LiveDNS (PUT JSON + Bearer), DNSimple (**PATCH** JSON + Bearer, with account and record IDs the user enters), Cloudflare (PUT with zone and record IDs the user enters), Porkbun (`editByNameType` JSON POST) |
| Not supported: multi-step | 10 | DigitalOcean, Linode, LuaDNS, Name.com, DreamHost, Yandex (ID lookup), Hover, One.com (cookie login), Azure (OAuth), Route 53 (SigV4) |
| Not supported: other | 3 | DyNS (plain HTTP), ODS (raw TCP), Google Domains (shut down) |

**Total: 39 of 52 supported (about 75%).**

Provider notes, checked in the review against provider docs:

- **deSEC:** Basic auth with the token as the password. The URL must include
  `myipv4={ip}&myipv6=preserve`, otherwise the AAAA record is deleted.
- **ClouDNS:** the dynamic URL's `q` value is a secret, so use `{token}`.
  Without the source-IP flag it needs an extra header for an explicit IP,
  which is why headers are allowed with GET.
- **Porkbun:** `editByNameType` updates **every** record with that name and
  type.
- **Cloudflare:** PUT replaces the whole record, and the body must set every
  field the user wants to keep.
- **GoDaddy:** the `Authorization` extra header takes precedence over Basic
  auth.
- **No-IP free:** requires a manual monthly confirmation on the account. The
  refresh option doesn't replace it.

## Delivery (decided with the user)

1. A feature branch on the fork (gszigethy), with a PR into the fork's
   `main`. The user tests it on hardware there.
2. The same change as a PR to upstream (Csontikka), on a branch from upstream
   `main`, without the W5500 parts. The PR body says honestly what was tested
   on hardware.
3. When upstream releases it, the fork picks it up through its normal
   upstream-update merge flow.

## Review history

- **Rev 1:** reviewed by a fresh Claude builder check and by Codex
  (read-only, against HEAD and `upstream/main`).
- **Rev 2:** the pfSense provider survey; the advanced request; the
  source-IP flag; the substring/`^` matcher.
- **Rev 3:** a full security, resource, leak and edge-case review by Claude
  and Codex. Its findings:
  - redirects
  - the URL being logged on parse errors
  - header injection after filling in
  - the uplink epoch and the dispatch boundary
  - STUN dropped
  - the TLS date limit
  - bounded reads
  - size caps after filling in
  - the persistent task and its cleanup
  - the no-NVS rule for the PSRAM task
  - the send-rule gaps
  - the stopped state kept across reboots
  - the atomic settings blob
  - the destination policy
  - JSON-only POST
  - the provider corrections
  - the SNMP column numbers
  - host unit tests

## Calls made by Claude (not explicitly decided by the user)

- Ranges: interval 1–1440 min, refresh 0–365 days. Timings: a WAN delay of
  about 10 s, a 60 s re-check while waiting for time sync.
- Default sources: ipify, icanhazip, AWS checkip. At most one `http://`
  source.
- What counts as a valid IP answer.
- Advanced request limits: one header; template caps of 256/512 bytes;
  filled-in caps of 512/512/1024 bytes; the list of reserved headers;
  placeholders only in userinfo, path and query, and only in JSON string
  values.
- `Authorization` in the extra header takes precedence over Basic auth.
- Response classification order, the rejection and cooldown code lists,
  3xx counting as temporary, the 20 s request budget and the 1024-byte read
  limit.
- Abuse limits: 60 s between provider calls; 429 → `Retry-After` (capped at
  24 h) or 10 min; 30 min for `dnserr`/`911`; doubling back-off capped at 1 h.
- Force: limited to once per 60 s, clears "stopped" but not a cooldown.
- The "stopped" state survives reboots, written through an esp_timer handoff.
- The uplink epoch, the config generation and the mutex-held dispatch
  boundary.
- The settings as one versioned NVS blob plus a `ddns_stop` key.
- The destination policy: refuse private, LAN and tailnet destinations, with
  no override.
- The literal-secret name list, which is best effort; logs and SNMP never
  carry templates or bodies.
- A persistent task, never deleted.
- Skipping checks while an OTA runs.
- No interface pinning, since the route hook already handles it.
- The `nsExtendResult` codes, the `"ddns"` index, the config columns, and no
  snippet in SNMP.
- Building Basic auth in code instead of changing sdkconfig.
- Host-side C unit tests in `tools/`, not wired into CI.
- This spec stays out of the upstream PR and lives on the fork branch only.
