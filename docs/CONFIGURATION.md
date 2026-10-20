# Configuration reference

Every setting below is configured from the device web UI. Changes are
written to NVS; some require a reboot to take effect (the UI flags a
pending reboot when so).

> IP addresses, SSIDs and peer names shown in screenshots throughout the
> docs are generic placeholders.

## Network → Device-wide network

| Field | Meaning |
|---|---|
| **Hostname** | Name advertised on the uplink network (DHCP/mDNS). |
| **DNS mapping TTL override** | `0` = off. Overrides the TTL the device applies to DNS-learned routes, for roaming continuity. |

## Network → Uplink networks (STA)

Up to **5** networks, tried in order. Per network:

| Field | Meaning |
|---|---|
| **SSID** | Upstream 2.4 GHz network to join. |
| **Password** | WPA2/WPA3 PSK. Leave empty to keep the stored one. |
| **Static IP** *(optional)* | `ip` / `mask` / `gw` / `dns`; empty = DHCP. |
| **WPA2-Enterprise (EAP)** *(optional)* | Method, phase-2, identity/username/password, optional CA bundle. |

## Network → Access Point (AP)

| Field | Default | Meaning |
|---|---|---|
| **AP SSID** | — | Network this device broadcasts. |
| **AP password** | — | WPA2 PSK; leave empty to keep current. |
| **Channel** | *auto* | Read-only. The single 2.4 GHz radio is shared with the STA, so the AP automatically follows the uplink channel in place (no reboot) — it is not operator-settable. The status page shows the channel the radio is actually on. |
| **AP IP / Subnet mask** | `192.168.4.1` / `255.255.255.0` | The AP subnet. Changing it offers to update the advertised Tailscale route. |
| **DNS served to AP clients** | this device | Empty = the on-board forwarder; or push a specific resolver. |
| **On-board DNS forwarder** | on | Resolver + PSRAM response cache for AP clients. |
| **Forwarder upstream** | learned / `1.1.1.1` | Upstream resolver the forwarder queries. |
| **Hide SSID** | off | Stop broadcasting the SSID in beacons. |

## Network → DHCP, clients, denylist, port forwarding

- **DHCP reservations** — pin an IP to a MAC (max 16).
- **Connected clients / Active leases** — live tables with per-client signal.
- **Denied MAC addresses** — block specific clients from associating.
- **Port forwarding** — map an external port to an AP-side client.

## Tailscale

| Field | Meaning |
|---|---|
| **Enabled** | Master switch for the tailnet client. |
| **Auth key** | Tailscale auth key (`tskey-…`). Leave empty to keep current. |
| **Hostname** | Node name on the tailnet. |
| **Login server** | Custom control plane (Headscale). Empty = hosted Tailscale. Accepts `host`, `host:port`, `http://host[:port]`, `https://host[:port]` (TLS validates against public CAs only — no self-signed). Requires Headscale ≥ 0.26 (validated on v0.28.0). |
| **Client version** | `Hostinfo.IPNVersion` reported to the control plane. Empty (default) = not reported. The Tailscale admin console gates some operations (e.g. reassigning a node address) on this and shows "Device is too old" when empty; set e.g. `1.98.9` to clear it, the `version.Long()` hash suffix is added automatically. Headscale users need none of this. Applies on the next restart. |
| **Advertise the AP subnet** | On by default. The AP subnet is announced as a subnet route, computed from the AP settings (so changing the AP address needs no route edit). Peers use it only after you approve it in the admin console. Off = announce only the list below. |
| **Additional advertised subnet routes** | One CIDR per line, on top of the AP subnet — e.g. the uplink LAN (see Source-NAT). |
| **Source-NAT advertised routes** | Off by default. Masquerades traffic forwarded from the tunnel out to the uplink LAN (or out to the internet when this device is an exit node) to the device's own STA IP — the Tailscale default for subnet routers. **On** = the uplink subnet works the moment you advertise it (upstream hosts reply to this device, which un-NATs back into the tunnel). **Off** = the upstream router needs a static route for the tailnet (`100.64.0.0/10 → this device`) instead. Enabling it offers to add your uplink subnet to the advertised routes. ⚠️ Do **not** advertise *and* accept the same subnet — that loops the inbound route back into the tunnel. |
| **Exit node** | Route AP-client public traffic through this tailnet exit node. Fails closed if unreachable. |
| **Max peers** | Upper bound on tracked peers. |
| **Accept peer subnet routes** | Install routes other nodes advertise. |
| **LAN bypass when using an exit node** | RFC1918 destinations stay on the local LAN even with an exit node selected. |

### Offering the router as an exit node

Switch on **Offer this router as an exit node**, save and restart, then
approve the exit node for the device in the Tailscale admin console and
select it on a client. That one switch does three things:

- advertises the default routes (`0.0.0.0/0` and `::/0`) next to the
  subnet routes;
- masquerades the clients' traffic behind the router's uplink address, so
  replies find their way back;
- advertises and runs the DNS service official Tailscale clients expect
  from an exit node. Without it a client reaches addresses but cannot
  resolve names.

What to expect:

- **IPv4 only.** `::/0` is announced because Tailscale wants the pair; IPv6
  traffic from a client is not carried.
- **Roughly 5–6 Mbit/s, and it depends on the setup.** On the reference
  bench an official Linux client gets about 5 Mbit/s on one connection and
  about 6 on four over a direct path (5 MB in 8 s); a contributor measured
  6.3 Mbit/s with a phone on 5G. Less when the path is relayed or when the
  WiFi link to the uplink is poor. Fine for browsing and API calls; do not
  plan video around it.
- **Leave the console log level at WARN** (System → SD card logging) unless
  you are reading the log. Console output is written synchronously on the
  115200-baud UART, so an INFO console costs the tasks that log their
  share of the wire time; before 0.1.32 that alone capped the tunnel at
  about 0.9 Mbit/s. The SD recorder's own level is not affected.
- It **cannot be combined with using an exit node** (the setting below):
  the peers' traffic would go straight back into the tunnel. The save is
  refused if both are set.
- With a selected exit node the same combination is ignored at boot, the
  exit node in use wins.
- A `0.0.0.0/0` typed into the additional routes (how it was done before
  the switch existed) counts as the switch being on, so an upgraded router
  that already offered an exit node also gets the DNS service and the NAT.

The DNS service is a small HTTP server on its own port (41180; clients
learn it from the node's Hostinfo). It runs only in this mode and answers
only requests that arrive on the router's tailnet address from a known
tailnet peer; anything from the AP or the uplink side gets `403`. Lookups
are forwarded to the uplink resolver, over UDP, with TCP for truncated
answers. `POST /dns-query` takes `application/dns-message`,
`GET /dns-query?dns=` takes unpadded base64url; messages up to 4096 bytes.
It does not interpret tailnet ACLs beyond that: a peer the control plane
lets talk to the router may use it.

### Tunnel MTU

`Auto` is recommended — 1280, the Tailscale tunnel MTU. Every peer's tun
device is 1280 whether the path is direct or DERP-relayed, so that is the
largest packet the far end carries; the AP-side TCP MSS clamp (1240) and the
ICMP path-MTU replies follow from it. A fixed MTU can be forced (576–1500)
if a path misbehaves.

## Firewall

Four chains, **first match wins**; an empty chain allows by default.

| Chain | Direction |
|---|---|
| `TO_ESP` | Internet → ESP |
| `FROM_ESP` | ESP → Internet |
| `TO_AP` | Clients → ESP |
| `FROM_AP` | ESP → Clients |

Per rule: **source** / **destination** (`any` or CIDR), **protocol**
(Any / ICMP / TCP / UDP), **source/dest port** (`0` = any, TCP/UDP only),
**action** (Allow / Deny).

## SNMP

Read-only SNMPv1/v2c agent, **off by default**. When enabled it binds
UDP `0.0.0.0:161` on every interface — including the tailnet, so a poller
anywhere on your tailnet can reach it without opening anything to the LAN.
Requests whose community string does not match are dropped without a reply,
so a wrong community is indistinguishable from the agent being off.

| Field | Meaning |
|---|---|
| **Enable SNMP** | Starts/stops the agent in place; no reboot. |
| **Community** | Shared secret for v1/v2c. There is no v3 — treat this as a password sent in clear text and prefer reaching the device over the tailnet. |
| **sysName / sysContact / sysLocation** | Served as `1.3.6.1.2.1.1.{5,4,6}.0`. Free text. |

Each text field is limited to 255 UTF-8 bytes. An enabled agent requires a
nonempty community; if an older installation has an empty community stored,
the agent stays off at boot until a valid one is saved. The four fields and
enable flag are saved together, and existing individual NVS keys are read
for migration.

Only GET and GETNEXT are answered; there is no SET, so nothing can be
changed over SNMP. GETBULK is accepted but returns one successor per
varbind, so bulk walks simply take more round trips.

### What it serves

| MIB | OIDs | Notes |
|---|---|---|
| MIB-II system | `1.3.6.1.2.1.1` | sysDescr carries the firmware version. |
| MIB-II interfaces | `1.3.6.1.2.1.2` | Fixed indices: **1** `eth0`, **2** `wlan0`, **3** `ts0` (Tailscale tunnel). Octet and packet counters on all three. |
| HOST-RESOURCES-MIB | `1.3.6.1.2.1.25` | `hrProcessorLoad.{1,2}` per core (0–100, 5 s window), `hrStorageTable` for internal DRAM and PSRAM, `hrSystemProcesses` for the task count. |
| ENTITY-SENSOR-MIB | `1.3.6.1.2.1.99` | Die temperature, deci-degrees Celsius (`entPhySensorPrecision` = 1, so 508 means 50.8 °C). |
| NET-SNMP-EXTEND-MIB | `1.3.6.1.4.1.8072.1.3.2` | One row with index `"ddns"` (`4.100.100.110.115`) carrying the [DDNS](#ddns) status; present even when DDNS is off. |
| Private | `1.3.6.1.4.1.99999.1.1.4.0` | `heapMinFreeBytes`, the all-time-low free-heap watermark — the one reading with no standard equivalent. **99999 is not an IANA-assigned enterprise number**; do not grow this tree. |

Interface indices are deliberately fixed rather than derived from lwIP's
`netif_list`, so a graph pointed at ifIndex 2 keeps meaning `wlan0` even
as interfaces come and go. An interface that is down — or that this board
does not have at all, such as `eth0` on a WiFi-only build — reports
`ifOperStatus down` with zero counters rather than disappearing and
renumbering the rows after it.

Quick check from any machine with net-snmp:

```bash
snmpwalk -v2c -c <community> <device-ip> 1.3.6.1.2.1.1        # system
snmpwalk -v2c -c <community> <device-ip> 1.3.6.1.2.1.2.2.1.10 # ifInOctets
snmpget  -v2c -c <community> <device-ip> 1.3.6.1.2.1.25.3.3.1.2.1  # CPU core 0
snmpget  -v2c -c <community> <device-ip> 1.3.6.1.2.1.99.1.1.1.4.1  # temperature
```

## DDNS

Keeps one DNS name pointed at the router's public IPv4 address, **off by
default**. The device finds its public address by asking three HTTPS
"what is my IP" services on every check and only acts on an address at least
two of them agree on, then sends one HTTPS request to your DNS provider when that
address (or the provider settings) changed. Everything is on the **DDNS**
tab; changes apply on Save, no restart.

| Field | Meaning |
|---|---|
| **Enabled** | Off = no DDNS traffic at all. |
| **Update URL** | The provider's update URL, `https://` only, with the placeholders `{ip}` `{user}` `{pass}` `{token}`. Values are percent-encoded (`{ip}` as is). Put secrets in the fields below, never literally in the URL: a literal value after `token=`, `key=`, `password=`, `pass=`, `pwd=`, `secret=`, `apikey=`, `secretapikey=` or `q=` is refused on Save, and a literal `user:pass@` is moved into User/Password for you. This check is best effort — a secret in a path segment gets through, so keep them out. |
| **Provider uses the request's source IP** | For providers that publish the address the request comes from (DY.fi, OpenDNS, ClouDNS dynamic URLs). Makes `{ip}` optional; the quorum still decides *when* to send. |
| **Advanced request → Method** | GET (default), POST, PUT or PATCH. |
| **Advanced request → Extra header** | One `Name: value` line, allowed with any method. An `Authorization` header replaces Basic auth. `Host`, `Content-Length`, `Transfer-Encoding`, `Connection` and `Cookie` are refused. |
| **Advanced request → Body template / Body type** | POST/PUT/PATCH only. Form (values percent-encoded) or JSON (placeholders only inside string values, values JSON-escaped; the result must parse). |
| **User / Password** | Sent as Basic auth when both are set (or when the URL has `{user}:{pass}@`), and available as `{user}` / `{pass}`. |
| **Token** | Available as `{token}`. |
| **Success if response contains** | `|`-separated alternatives, case-sensitive substrings; a leading `^` anchors to the start of the trimmed answer. Empty = any 2xx counts. Examples: `^good|^nochg` (dyndns2), `OK` (DuckDNS), `<ErrCount>0</ErrCount>` (Namecheap). For JSON APIs that report errors with the HTTP status, leave it empty. |
| **IP source 1–3** | Each slot picks a different source: api.ipify.org, ipv4.icanhazip.com, checkip.amazonaws.com, ifconfig.me, or a Custom URL. Defaults: ipify, icanhazip, AWS. At most one source may be plain `http://`. |
| **Check every N min** | 1–1440, default 5. |
| **Also when WAN gets an IP** | Default on: an uplink gaining or losing an address (including failover) schedules a check about 10 s later. |
| **Refresh every N days** | Re-sends an unchanged address after N days, 0 (default) = never. |

Password and Token are write-only: the API and the page only ever say
whether they are set and how long they are. Save validates the fully encoded
request, including the longest IPv4 string. If a provider dispatch is active,
Save returns busy after 200 ms; retry it after the request finishes.
Leave the input empty to keep
the stored value; **Clear** removes it on Save. The eye button only reveals
what you typed in this session.

**Force update now** runs a check at once and sends even when nothing
changed. It can be used once per 60 s; the button counts down. While the DDNS
tab is visible, status refreshes every five seconds without replacing form
fields or typed secrets.

### How a check works

1. Prerequisites: DDNS enabled, an uplink up, no OTA install running, and
   the clock set (until it is, the check retries every 60 s). A missing one
   shows as `waiting` with the reason.
2. Ask all three sources, every check, and show each answer or error. The
   address is the one at least two of them return. A valid answer is a 2xx
   with one dotted-quad public address (not private, loopback, link-local, CGNAT, multicast or
   reserved). No agreement = `quorum-failed`, nothing is sent, and the next
   trigger retries.
3. Send when the check was forced, the address or any request setting
   changed since the last success, or a refresh is due. The last successful
   address lives in RAM, so the first check after every boot sends once.
4. Before connecting, the provider and custom-source hosts are resolved and
   refused if they point at a private, loopback, link-local, CGNAT or
   tailnet address. Redirects are never followed. One request, 20 s budget,
   at most 1024 bytes of answer retained; classification uses that prefix at
   the byte cap. A response that ends before its declared length and before
   the cap is a temporary failure. DNS resolution and
   nonblocking TLS/HTTP I/O share one deadline; the validated IPv4 address is
   reused for the connection while preserving the hostname for TLS and HTTP.

The device's own traffic to public addresses leaves through the uplink even
when an exit node is selected, so the quorum sees the uplink's address, not
the exit node's.

### States

| State | Means | Leaves it |
|---|---|---|
| `disabled` | Enabled is off. | Turn it on. |
| `waiting` | A prerequisite is missing (no uplink, OTA in progress, waiting for time sync). | On its own once the prerequisite is met. |
| `ok` | Last check agreed on an address and the provider is up to date. | — |
| `quorum-failed` | The IP sources did not agree. | Next trigger retries. |
| `provider-error` | Temporary failure: timeout, connect failure, cut-off answer, `redirect (3xx)`, 5xx, or a 2xx that did not match. Ambiguous results back off: the wait between provider attempts doubles from the interval up to 1 h until a success. | On its own; saving the settings resets the back-off. |
| `cooldown` | The provider asked to slow down: HTTP 429 (honours `Retry-After` in seconds, capped at 24 h, 10 min without it) or `dnserr` / `911` (30 min). IP checks continue; no provider call is made, not even on Force. | On its own when the shown time runs out. |
| `stopped` | Permanent rejection: HTTP 401, 403 or 404, or `badauth`, `nohost`, `notfqdn`, `numhost`, `!donator`, `abuse`, `badagent`. Kept in NVS, so a reboot does not retry it. | Fix the settings and Save, or press Force. |

Whatever the trigger, the provider is called at most once per 60 s.

The status block shows each source's answer, the agreed address, the last
success and its age, the last HTTP status and error, and the first 64
printable characters of the provider's answer with the stored password and
token masked as `***`. Logs, the SD log, the pre-crash capture and SNMP
only ever carry the provider's host name, an error code and the HTTP
status — never a URL, request or answer body.

### Limits

- **IPv4 only**; no AAAA records.
- **One request per update.** Providers that need an ID lookup, a login,
  OAuth or request signing (DigitalOcean, Linode, Route 53, Azure, Hover,
  …) are not supported.
- **One profile**: one provider, one name (or whatever one request updates).
- **HTTPS only** for the provider; plain-HTTP-only providers (DyNS) are
  not supported.
- **No STUN**: Tailscale's STUN result is not used as an IP source.
- **The last IP is not kept across reboots**, so each boot sends one update.
  The `stopped` state is kept.
- **Certificate dates are not checked** (the host name and chain are), as for
  OTA and telemetry: the device may not know the time yet.
- **CGNAT**: behind carrier-grade NAT the sources report the carrier's
  public address. The update "works", but nothing can connect in on it.
- **NVS is not encrypted**, and flash core dumps include task stacks — the
  same as for the Wi-Fi and Tailscale secrets.

### Providers

The provider survey is based on pfSense CE's DynDNS client. The provider
list, request shapes and dyndns2 return-code handling come from
`src/etc/inc/dyndns.class` in pfsense/pfsense `master` at `9363ac5`
(2026-03-31), the code base for pfSense CE 2.9.0, Apache License 2.0. No
pfSense code is copied; it served as the reference list and for behaviour.

| Group | Providers |
|---|---|
| Single GET (32) | All-Inkl, City Network, ClouDNS, deSEC, DNSexit, DNS Made Easy, DNS-O-Matic, Domeneshop, DuiaDns, DynDNS ×3, Dynv6, easyDNS, EuroDNS, freeDNS v1/v2, GratisDNS, HE.net, HE Tunnelbroker, Loopia, Namecheap, NIC.RU, No-IP, No-IP free, OVH, SelfHost, SPDYN, Strato, ZoneEdit, DY.fi and OpenDNS (both with "source IP") |
| Single request, not GET (7) | GleSYS, Mythic Beasts (form POST + Basic), GoDaddy (PUT JSON, `sso-key` header), Gandi LiveDNS (PUT JSON + Bearer), DNSimple (PATCH JSON + Bearer, account and record IDs entered by you), Cloudflare (PUT, zone and record IDs entered by you), Porkbun (`editByNameType` JSON POST) |
| Not supported: multi-step (10) | DigitalOcean, Linode, LuaDNS, Name.com, DreamHost, Yandex, Hover, One.com, Azure, Route 53 |
| Not supported: other (3) | DyNS (plain HTTP), ODS (raw TCP), Google Domains (shut down) |

That is 39 of pfSense's 52 IPv4 entries. Ready-to-paste settings — replace
the names, put your secret in the field named, and leave unnamed fields at
their defaults:

| Provider | Settings |
|---|---|
| DuckDNS | URL `https://www.duckdns.org/update?domains=myname&token={token}&ip={ip}` · Token = DuckDNS token · match `OK` |
| dyndns2 (No-IP) | URL `https://dynupdate.no-ip.com/nic/update?hostname=myhost.ddns.net&myip={ip}` · User / Password = account (or DDNS key) · match `^good|^nochg` |
| dyndns2 (DynDNS) | URL `https://members.dyndns.org/nic/update?hostname=myhost.dyndns.org&myip={ip}` · User / Password = account (or updater key) · match `^good|^nochg` |
| deSEC | URL `https://update.dedyn.io/?hostname=myname.dedyn.io&myipv4={ip}&myipv6=preserve` · User = `myname.dedyn.io` · Password = deSEC token · match `^good|^nochg`. Keep `myipv6=preserve`, or deSEC deletes the AAAA record. |
| Namecheap | URL `https://dynamicdns.park-your-domain.com/update?host=@&domain=example.com&password={pass}&ip={ip}` · Password = Dynamic DNS password (Advanced DNS tab) · match `<ErrCount>0</ErrCount>` |
| Cloudflare | Method PUT · URL `https://api.cloudflare.com/client/v4/zones/ZONE_ID/dns_records/RECORD_ID` · header `Authorization: Bearer {token}` · body (JSON) `{"type":"A","name":"home.example.com","content":"{ip}","ttl":60,"proxied":false}` · Token = API token with DNS edit · match empty. PUT replaces the whole record, so the body must set every field you want to keep. |
| GoDaddy | Method PUT · URL `https://api.godaddy.com/v1/domains/example.com/records/A/home` · header `Authorization: sso-key {token}` · body (JSON) `[{"data":"{ip}","ttl":600}]` · Token = `KEY:SECRET` · match empty |
| ClouDNS | URL `https://ipv4.cloudns.net/api/dynamicURL/?q={token}` · **Provider uses the request's source IP** on · Token = the `q` value of your dynamic URL (it is a secret) |
| Porkbun | Method POST · URL `https://api.porkbun.com/api/json/v3/dns/editByNameType/example.com/A/home` · body (JSON) `{"secretapikey":"{pass}","apikey":"{token}","content":"{ip}","ttl":"600"}` · Password = secret API key · Token = API key · match empty. `editByNameType` updates **every** A record with that name. |

No-IP free hosts still need the monthly confirmation on the No-IP account;
the refresh option does not replace it. After the first Save, press
**Force update now** and look at the provider's answer in the status block
to confirm the settings.

### Monitoring over SNMP

With [SNMP](#snmp) on, the status is one NET-SNMP-EXTEND-MIB row with
index `"ddns"` (OID suffix `4.100.100.110.115`), so pollers that know the
MIB find it on a plain walk:

| Object | OID | Value |
|---|---|---|
| `nsExtendOutput1Line."ddns"` | `1.3.6.1.4.1.8072.1.3.2.3.1.1.4.100.100.110.115` | e.g. `ok 84.2.17.203`, `quorum-failed`, `cooldown http 429`, `stopped badauth`, `disabled` |
| `nsExtendOutputFull."ddns"` | `….3.1.2.4.100.100.110.115` | each source's answer, the last success address and time, the last HTTP status and error; never the provider's answer |
| `nsExtendOutNumLines."ddns"` | `….3.1.3.4.100.100.110.115` | lines in OutputFull |
| `nsExtendResult."ddns"` | `….3.1.4.4.100.100.110.115` | 0 ok, 1 disabled, 2 waiting, 3 quorum-failed, 4 provider-error, 5 cooldown, 6 stopped |

The row also has a read-only config part (`….2.2.1.<col>.4.100.100.110.115`)
for tools that walk it first. Alerting is up to your poller; a useful
trigger is `nsExtendResult."ddns" >= 3`, for example:

```bash
snmpget -v2c -c <community> <device-ip> 1.3.6.1.4.1.8072.1.3.2.3.1.4.4.100.100.110.115
# Zabbix: item on that OID, trigger  last(/esp32-router/ddns.result)>=3
```

#### LibreNMS

LibreNMS graphs and alerts on `nsExtendResult."ddns"` as a **Custom OID**.
It can't show the text rows: Custom OIDs only take numbers, and `ddns` is
not one of LibreNMS's built-in Applications.

1. Add the router as an SNMP v2c device with its community. It reports no
   sysObjectID, so LibreNMS treats it as a generic device.
2. Device → Edit → **Custom OID** → Add:
   - **Name** `ddns`
   - **OID** `1.3.6.1.4.1.8072.1.3.2.3.1.4.4.100.100.110.115`
   - **Data Type** `GAUGE`
   - **Divisor** / **Multiplier** `1`
   - **Alert Thresholds → High** `3`, so every failure state alerts
   - **Alerts Enabled** on

   Press **Test**: only an OID that passed is polled.
3. Alerts → Alert Rules → *Create rule from collection* → **CustomOID over
   limit** (`customoid_current >= customoid_limit`), then attach a transport.

The graph is under the device's Graphs → Custom OID, or in a dashboard Graph
widget of type *Customoid*. A warning threshold of `2` would also flag
`waiting`, which is normal while no uplink is up. To see the text line, add a
LibreNMS Service with the monitoring plugin `check_snmp` on
`nsExtendOutput1Line."ddns"`
(`1.3.6.1.4.1.8072.1.3.2.3.1.1.4.100.100.110.115`).

## System

| Field | Meaning |
|---|---|
| **Device name** | Friendly name shown in the UI header and `/api`. |
| **Timezone** | Device TZ; a real change flags a pending reboot. |
| **TX power** | 0 = IDF default (≈ 20 dBm), 8–84 in 0.25 dBm steps. Applied live and at boot. |
| **WiFi power save** | On by default (modem sleep, the lower-current choice). With it on, traffic sent *to* the router waits for the uplink AP's next beacon: LAN ping 1–135 ms (average about 50) on the reference router against 1–35 ms (average about 8) with it off, and the same shift on tunnel round trips. Throughput is the same either way. Applied live and at boot. |
| **Anonymous telemetry** | On by default; one toggle in the **About** card opts out. Daily anonymous hash + counters (see README → Telemetry). |
| **SD card logging** | Enable + per-sink (console / SD) log level; the on-device flight recorder. |
| **Firmware update (OTA)** | Manual upload, or polled auto-install window. Optional **beta channel** also offers GitHub pre-releases (off by default = stable releases only). |
| **Encrypted backup / restore** | Export/import the full config, encrypted with a passphrase. |
| **Danger zone** | Reboot, factory reset, reset Tailscale identity. |
