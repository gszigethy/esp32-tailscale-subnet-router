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
| **Channel** | *auto* | Read-only. The single 2.4 GHz radio is shared with the STA, so the AP automatically follows (and realigns to) the uplink channel — it is not operator-settable. |
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
| **Advertised subnet routes** | One CIDR per line; the AP subnet is offered automatically. |
| **Source-NAT advertised routes** | Off by default. Masquerades traffic forwarded from the tunnel out to the uplink LAN (or out to the internet when this device is an exit node) to the device's own STA IP — the Tailscale default for subnet routers. **On** = the uplink subnet works the moment you advertise it (upstream hosts reply to this device, which un-NATs back into the tunnel). **Off** = the upstream router needs a static route for the tailnet (`100.64.0.0/10 → this device`) instead. Enabling it offers to add your uplink subnet to the advertised routes. ⚠️ Do **not** advertise *and* accept the same subnet — that loops the inbound route back into the tunnel. |
| **Exit node** | Route AP-client public traffic through this tailnet exit node. Fails closed if unreachable. |
| **Max peers** | Upper bound on tracked peers. |
| **Accept peer subnet routes** | Install routes other nodes advertise. |
| **LAN bypass when using an exit node** | RFC1918 destinations stay on the local LAN even with an exit node selected. |

### Advertised routes — two tiers

Routes advertised to the tailnet come from two independent sources, merged at connect time:

1. **Manual routes** — the *Advertised subnet routes* textarea on the Tailscale tab. One CIDR per line. This field is the user's to manage; the firmware never modifies it.
2. **Per-interface auto-routes** — opt-in toggles in the Status cards (see below). Each interface caches or computes its CIDR independently and merges it with the manual list. Duplicates are removed before the combined list is passed to microlink.

### Status page — WiFi uplink master switch

The **Use as uplink** row at the top of the Status → Uplink WiFi card
controls whether WiFi STA is used as an uplink at all. It is **off by
default**: this device's primary role is a drop-in wired Tailscale router
— Ethernet is the uplink and the soft-AP exists for provisioning.

Two things this switch is *not*:

- It is **not** the soft-AP. The provisioning AP is independent and stays
  up regardless — you configure a wired-only device over the AP with the
  WiFi uplink switched off, which is the normal case.
- It is **not** the **Auto-route** toggle below it. Auto-route controls
  whether the STA's *subnet* is advertised to the tailnet; this controls
  whether the radio associates upstream at all.

With it off, the STA netif still exists (the AP DNS copy, ACL hooks and
route composition reference it) but no credentials are installed and it
never associates — so an unprovisioned board doesn't sit in a permanent
association-retry loop, scanning on the same single 2.4 GHz radio the AP
is using. Changes apply live; no reboot.

On update from a firmware without this setting, the default is derived
rather than forced: a board that already had saved WiFi networks keeps
WiFi enabled, a fresh board gets the wired-first default.

Overall health on the Status page reflects this: **Degraded** means *no*
uplink is up. A wired-only box with WiFi deliberately off reads
`Disabled` on the WiFi card and stays **Online**.

### Status page — Ethernet uplink master switch

The same **Use as uplink** row appears at the top of the Status →
Ethernet Uplink card, for the opposite case: a board with both interfaces
present where the operator wants to run on WiFi only (a spare ETH port
reserved for something else, a cable that needs freeing up, or simply
testing WiFi-only failover) without physically unplugging anything.

Unlike the WiFi switch, this one is **on by default** — Ethernet is this
device's primary role, so a fresh board and one updating from a firmware
without this setting both keep working exactly as before.

Turning it off stops the W5500 driver (`esp_eth_stop()`) rather than
tearing it down: no link, no DHCP lease, no traffic, but the driver stays
installed and ready, so switching it back on (`esp_eth_start()`) needs no
reboot. The Ethernet card reads `Disabled` (neutral) while off — as
opposed to a plain `Disconnected`, which stays neutral too here rather
than the WiFi card's red: this card renders even on hardware with no
W5500 physically present, and a permanent red badge on every WiFi-only
device would be a false alarm.

### Status page — subnet routing toggles

Each network interface on the Status dashboard has an **Auto-route** (or **Advertise AP**) row with a checkbox and **Save** button. Enabling it advertises that interface's subnet to the tailnet; disabling it removes it from the composed route list. Changes take effect on the next Tailscale reconnect (triggered automatically).

| Interface | Default | Notes |
|---|---|---|
| **Ethernet (ETH)** | **On** (when ETH hardware present) | CIDR is cached on first DHCP lease and re-cached on address change. Zero-touch — no configuration needed on ETH-equipped hardware. |
| **WiFi uplink (STA)** | **Off** | Opt-in; preserves original WiFi-only behaviour. Enabling immediately reads the live STA IP so the CIDR is ready for the next restart, without waiting for the next DHCP event. |
| **Access Point (AP)** | **Off** | Opt-in. CIDR is always computed live from the static AP IP — no NVS cache. |

> **These toggles are independent of the manual textarea.** Enabling STA or AP auto-routing adds that subnet *alongside* whatever is in the textarea — it never removes or replaces user-entered CIDRs.

> **Approve the route in Tailscale admin.** Any newly advertised subnet (manual or auto) must be approved under Machines → *your device* → **Edit route settings** before tailnet peers can use it.

### Tunnel MTU

`Auto (peer-aware)` is recommended — it derives the effective MTU/MSS
from the active path (direct vs DERP, exit vs not). A fixed MTU can be
forced (576–1500) if a path misbehaves.

## Firewall

Four chains, **first match wins**; an empty chain allows by default.

| Chain | Direction |
|---|---|
| `TO_ESP` | Internet → ESP (any uplink: wired ETH or WiFi STA) |
| `FROM_ESP` | ESP → Internet (any uplink) |
| `TO_AP` | Clients → ESP |
| `FROM_AP` | ESP → Clients |

The `*_ESP` chains apply to **both** uplinks. Before v0.1.20 they were
attached only to the WiFi STA interface, so on a wired device every
`TO_ESP` / `FROM_ESP` rule was accepted, saved and displayed but enforced
on nothing — review your rules after updating.

Per rule: **source** / **destination** (`any` or CIDR), **protocol**
(Any / ICMP / TCP / UDP), **source/dest port** (`0` = any, TCP/UDP only),
**action** (Allow / Deny).

## System

| Field | Meaning |
|---|---|
| **Device name** | Friendly name shown in the UI header and `/api`. |
| **Timezone** | Device TZ; a real change flags a pending reboot. |
| **Anonymous telemetry** | On by default; one toggle in the **About** card opts out. Daily anonymous hash + counters (see README → Telemetry). |
| **SD card logging** | Enable + per-sink (console / SD) log level; the on-device flight recorder. |
| **Firmware update (OTA)** | Manual upload, or polled auto-install window. Optional **beta channel** also offers GitHub pre-releases (off by default = stable releases only). |
| **Encrypted backup / restore** | Export/import the full config, encrypted with a passphrase. |
| **Danger zone** | Reboot, factory reset, reset Tailscale identity. |
