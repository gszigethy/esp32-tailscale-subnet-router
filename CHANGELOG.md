# Changelog

All notable changes to this project are documented here. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.20-W5500] — 2026-09-10

Merges upstream [v0.1.20](https://github.com/Csontikka/esp32-tailscale-subnet-router/releases/tag/v0.1.20)
into this fork; no W5500-specific changes in this round. See upstream's
notes below for the merged content (microlink line-up, IPNVersion,
exit-node save fix, one-boot crash signatures).

### Added
- **Client version (`Hostinfo.IPNVersion`)** — new *Client version* field on the Tailscale card (NVS `ts_ipn_ver`, API `settings.ipn_version`). The Tailscale admin console gates some device operations on the reported client version and shows "Device is too old" when it is empty; set e.g. `1.98.9` to clear that (the `version.Long()` hash suffix is appended automatically). Empty by default — a public client should not claim a version it is not. Port of esphome-tailscale#39 by @timmills.
- **Netmap-buffer allocation failures are explicit.** All four control-plane allocation sites log the requested size against free PSRAM/internal memory and the largest free block instead of failing silently into "MapRequest failed, will retry" (found via esphome-tailscale#45 on a 2 MB-PSRAM board).
- **One-line DISCO summary** every 10 s while inbound discovery traffic flows (`DISCO: N packets in 10 s, M iterations budget-capped`), replacing per-packet chatter.

### Changed
- **The DISCO/WireGuard manager budgets its work per iteration** (8 DISCO / 40 ms; each WireGuard drain 64 packets / 30 ms in its own window) and always reaches its 10 ms sleep, so a peer that keeps probing can no longer monopolise the core. Per-packet DISCO lines moved to DEBUG; `DISCO PONG unmatched` / `probe table full` rate-limited to one line per 10 s. In the same-window A/B under a live DISCO loop this gave a 3× better on-device download and half the AP-client latency versus 0.1.19 (esphome-tailscale#46).
- **`/key` sends the real capability version** (`ML_CTRL_PROTOCOL_VER`, 131) instead of a hardcoded 88. Headscale ≥ 0.29 rejects 88 as an unsupported client at the very first step; SaaS accepted both.

### Fixed
- **Refused registrations now say why** — `RegisterResponse.Error`, `NodeKeyExpired` and `AuthURL` are acted on like the reference client does; `User.ID`/`DisplayName` are display-only (auth-key and tag-owned nodes legitimately have no user). Port of esphome-tailscale#38 by @timmills.
- **Peers removed by the control plane disappear immediately** — `PeersRemoved` carries NodeIDs; the handler expected nodekey strings, so a deleted node lingered (probed, DISCO-pinged) until the next full netmap. Port of esphome-tailscale#42.
- **Selecting an exit node no longer black-holes AP clients until the next restart.** Saving `exit_node_ip` flipped the live default route to the tunnel within seconds, while microlink only learns the exit node when it (re)starts — so every AP client lost the internet until the operator restarted (measured on 0.1.19: public IP unreachable, a 5 MB download stuck at 0 bytes for 90 s). The save now persists the choice only, as its `restart_required` response always implied; *Settings* shows the saved value (plus `exit_node_restart_pending`), *Status* the live one, and the route follows on restart.
- **Stale crash signatures are no longer re-sent.** A BROWNOUT or watchdog reset produces no coredump, yet the telemetry gate treated it as a crash and re-sent the *previous* panic's signature from NVS, so the fleet view showed phantom repeat crashes. The signature is now erased on any boot without a fresh coredump — it lives for exactly the one boot after the panic.

## [0.1.19-W5500] — 2026-09-10

First release of this fork. Adds native W5500 SPI Ethernet uplink support
on top of upstream 0.1.19, plus the per-interface routing and uplink
master-switch work it depends on.

### Added

- **W5500 SPI Ethernet uplink.** When a W5500 module is wired to the defined SPI pins and detected at boot, it automatically becomes the WAN uplink. WiFi shifts from STA+AP dual-role to AP-only, giving IoT clients a dedicated 2.4 GHz radio free of uplink contention. Tailscale and NAPT use the ETH interface transparently — no extra configuration needed.

- **Per-interface subnet route advertisement.** Routes are now managed per interface and composed at Tailscale connect time, separately from the manual `ts_routes` textarea (which is never modified by auto-detection):
  - **ETH LAN** — auto-detected on first DHCP lease; re-cached only when the address changes (`ip_changed=true`). Enabled by default (`eth_route_en=1`). Zero-touch on ETH-equipped hardware.
  - **WiFi STA** — opt-in, off by default (`sta_route_en=0`) to preserve original WiFi-only behaviour. Toggle in Status → Uplink WiFi card. When enabled, the CIDR is populated immediately from the live netif so the next Tailscale restart doesn't wait for the next DHCP event.
  - **AP subnet** — opt-in, off by default (`ap_route_en=0`). Toggle in Status → Access Point card. Always computed live from the AP netif (static IP), so no NVS cache is needed.
  - All sources are deduplicated before being passed to microlink. The manual textarea remains the user's own; auto-routes appear alongside it, never inside it.

- **Optional WiFi STA uplink (`sta_uplink_en`).** The WiFi uplink is now a master switch, off by default, exposed as "Use as uplink" on the Status → Uplink WiFi card and via `POST /api/wifi-uplink`. The device's primary role is a drop-in wired Tailscale router: Ethernet is the uplink, the soft-AP exists for provisioning, and WiFi-as-uplink is opt-in. With it off, the STA netif is still created (the AP DNS copy, ACL hooks and route composition reference it) but no credentials are installed and it never associates — an unprovisioned board no longer sits in a permanent association-retry loop against the Kconfig placeholder SSID, scanning on the same radio the AP is using. Applied live, no reboot. On update from a build without the key, the default is derived rather than forced: a board with saved WiFi networks keeps WiFi enabled, a fresh board gets the wired-first default.

- **Mirrored Ethernet uplink master switch (`eth_uplink_en`).** For boards with both interfaces present: the same "Use as uplink" row now appears on the Status → Ethernet card and via `POST /api/eth-uplink`, letting the operator take the wired uplink out of service (a spare port reserved for something else, WiFi-only failover testing) without unplugging the cable. **On by default**, the opposite of the WiFi switch — this device's primary role is wired, so an update from a firmware without the key changes nothing. Applied live via `esp_eth_start()`/`esp_eth_stop()`: the driver stays installed and glued to the netif either way, so re-enabling needs no reboot, and `esp_netif`'s own `ETHERNET_EVENT_STOP` → `esp_netif_action_stop()` path already brings the netif down, stops its DHCP client and clears its IP — no manual state cleanup needed on the disable side.

- **Ethernet traffic counters.** `eth.bytes_in` / `eth.bytes_out` in `/api/status`, rendered as a "Traffic" row on the Status → Ethernet card. Previously only the WiFi STA interface had counters, so a wired device reported no throughput at all.

- **`/favicon.ico` handler + inline SVG icon in the SPA.** Nothing served this URL and the SPA declared no icon, so every open tab produced a recurring "URI not found" WARN plus a 404 in the device log.

### Fixed

- **Auto-detected routes no longer overwrite user-managed manual routes.** The previous design wrote the ETH CIDR directly into `ts_routes` (the NVS key backing the web-UI textarea), destroying any routes the user had entered by hand and also breaking `maintain_ap_cidr_in_routes()`. Replaced by `tailscale_compose_routes()`, which merges all sources at connect time without ever writing to `ts_routes`.

- **WiFi STA CIDR not populated when enabling auto-routing from the web UI.** When `sta_route_en=0` at boot, the DHCP event handler skips the CIDR cache write. If the user later enabled auto-routing via the Status card, `sta_route_cidr` was still empty, causing the next Tailscale restart to silently advertise no STA routes. The `/api/sta-routing` handler now reads the live STA netif IP immediately on enable and writes the CIDR to NVS.

- **DNS relay task deadlock on boot.** The DNS relay state callback (`dns_relay_state_cb`) was registered before the default netif was set, causing a deadlock when the relay task interacted with the still-initialising lwIP DNS stack. Fixed by deferring relay initialisation until after the default netif is established.

- **ACL firewall was not enforced on the Ethernet uplink.** `netif_hooks_init()` attached the packet filter to the WiFi STA and soft-AP netifs only, so on a wired device every `TO_ESP` / `FROM_ESP` rule was accepted by the UI, persisted to NVS and displayed as active while being enforced on nothing — as was the STA TTL override. The two uplinks now share those chains, matching how `acl.h` defines them ("uplink input" / "uplink output"). **Review your firewall rules after updating:** a rule that has silently been inert on a wired box will start taking effect.

- **Concurrent Tailscale reconnects could use freed memory.** `tailscale_connect_task` is spawned from six call sites (the STA and ETH got-IP handlers, three route-toggle endpoints and the Tailscale config save) with nothing serialising them. They all end in `tailscale_connect()`, which tears down `s_microlink` and builds a new instance — two tasks in there at once meant one calling `microlink_destroy()` on the handle the other was initialising through, or both calling `microlink_init()` and leaking the first instance with its tasks and sockets live. Trivially reachable: the three per-interface "Auto-route" toggles sit next to each other on the Status page and each one restarts Tailscale. Connect and disconnect are now mutually exclusive, and redundant requests coalesce instead of piling up 30-second SNTP waiters.

- **Exit-node teardown could restore a dead default route.** Turning exit-node mode off handed the default route back to whichever netif held it when the mode was switched on. On a wired box those routinely differ — it boots with the WiFi STA as default and ETH takes over on its DHCP lease — so the restore pointed at an interface with no address and black-holed everything outbound. The live uplink is now preferred, with the remembered netif only as a fallback.

- **Route hook could return a down interface.** The lan-bypass netif-prefix match (step 2a) accepted any netif holding an address, without checking it was up and link-up. Returning a netif from `ip4_route_src_hook` bypasses the check `ip4_route()` would otherwise apply, so an interface with a stale address became a silent black hole rather than a reportable routing failure. Mirrored into `route_explain()` so the `/diag` route lookup keeps matching the live hook.

- **Out-of-bounds read parsing the session cookie.** `session_find_for_req()` indexed the candidate token at its full length to check the delimiter, without bounding it against the header buffer — a `ts_session=` landing within a token-length of the end of a long `Cookie` header read past the buffer. The comparison is now length-bounded and constant-time. The buffer also grew from 160 to 512 bytes and tolerates `ESP_ERR_HTTPD_RESULT_TRUNC`: previously a browser holding a couple of unrelated cookies for the origin overflowed it and was logged out with no indication why.

- **URI-handler registration failures were silent.** All 57 `httpd_register_uri_handler()` calls discarded their return value, so filling `max_uri_handlers` (which stood at 60) would have dropped whichever endpoints came last with no diagnostic. Failures now log; the limit is 80.

- **Ethernet MAC reported as `00:00:00:00:00:00` before link-up**, which is indistinguishable from the all-zero-SHAR bug that made the W5500 silently undiagnosable. The status cache is now seeded when the MAC is programmed.

- **Netif glue leaked when `esp_eth_start()` failed** — the error path destroyed the netif with the glue still attached.

- **`ap_connect` / `sta_connect` / `connect_count` were not `volatile`** despite being written from the WiFi and ETH event tasks and read from the HTTP server task, with `telemetry.c` spin-waiting on `ap_connect`. It happened to work only because `vTaskDelay()` is a call the compiler cannot see through.

- **SNAT silently force-enabled on ETH detection.** The ETH IP event handler and `/api/eth-routing` handler were unconditionally setting `tailscale_snat_subnet_routes=1` in NVS whenever ETH routing was enabled. SNAT is a security-sensitive user choice; this hidden side-effect has been removed.

## [0.1.19] — 2026-08-15

Stable release. Promotes the 0.1.19-beta1 DERP-liveness work to a stable
build after an eight-day beta bake; **no functional changes versus
0.1.19-beta1** (the only delta is the version string).

Bake evidence: the reference device ran 168 hours without a single reboot
or crash, during which the new control-plane map-stream watchdog caught and
self-healed **eight** silently dead map sessions — roughly one per day, each
one an outage that earlier firmware would have ridden out as a wedged
control plane until someone power-cycled the board. A second device on a
different continent ran the same build with no faults reported.

## [0.1.19-beta1] — 2026-08-07

**Beta pre-release** — DERP relay liveness. Ports the esphome-tailscale
v0.5.5 fix bundle and the HTTP/2 reassembly series into the microlink
submodule (reported and analyzed by timmills in esphome-tailscale#31/#32/#33,
bench-verified there), plus two earlier fixes those trees had and ours
didn't.

### Fixed

- **A dead DERP relay connection now recovers on its own.** Previously 3
  failed connect attempts parked the relay task forever with nothing left to
  re-arm it — the device stayed "connected" while relay-only peers could
  never reach it again. Now: endless exponential backoff (5 s → 60 s cap),
  a 90 s RX-liveness watchdog on a "connected" relay, and every connect
  failure path tears the TLS context down completely (each leaked ~17 KB
  before, eventually killing all later handshakes). (microlink submodule.)
- **ACL revocation reaches the device.** A full `Peers` list in a
  MapResponse is now authoritative — table entries omitted from it are
  swept, and peer removal also drops the NVS boot-cache entry, so revoked
  peers stay gone across reboots instead of resurrecting.
- **Large MapResponses no longer corrupt the long-poll stream.** HTTP/2
  frames that span a read boundary are reassembled instead of dropped
  (a big netmap deterministically desynced the stream parser).
- **Control-plane map-stream watchdog** (silent mapSession death behind a
  live HTTP/2 front end → automatic re-register within ~5 min) and the
  register-after-stream-read ordering fix — catch-up ports of
  esphome-tailscale v0.5.4/v0.5.2 fixes that predate this repo's submodule
  pin.
- `DISCO decrypt failed` now names the claimed sender and arrival path.

### Added

- **Per-cause reconnect counters in telemetry** (`rcs`/`rct`/`rcd`/`rcr`):
  map-stream watchdog fires, coord transport deaths, DERP RX-watchdog
  fires, and failed DERP connect attempts — so the fleet can distinguish
  watchdog saves from ordinary transport flaps (a single combined connect
  count overcounts).

### Known limitation

- The opt-in netcheck home-DERP override (`tailscale_netcheck_override`,
  default OFF) still announces the *configured* region in
  `NetInfo.PreferredDERP` while connecting to the measured one — peers
  would dial the announced region. Leave the override OFF (the default)
  until the announce-before-move rework (esphome-tailscale#36) lands
  upstream and gets ported.

## [0.1.18] — 2026-08-06

Stable release. Promotes the 0.1.18-beta1 changes (end-to-end Headscale
support, netmap via the streaming long-poll, discoverable first-boot AP) to a
stable build after a five-week beta bake with no regressions reported; no
functional changes versus 0.1.18-beta1. Also folds in the GCC 14 build fix
(#10): the false-positive `-Wmaybe-uninitialized` in ESP-IDF's
`esp_driver_i2c` is downgraded to non-fatal for that one component, so fresh
builds succeed on hosts whose toolchain trips it.

## [0.1.18-beta1] — 2026-07-03

### Added

- **Headscale support works end-to-end.** (#7) The login-server field now
  accepts `host`, `host:port`, `http://host[:port]` and `https://host[:port]`
  (TLS via the ESP-IDF public-CA bundle — Let's Encrypt works, self-signed
  does not), and the device fetches the control plane's Noise public key from
  `/key?v=88` instead of assuming the hosted-Tailscale key, so the ts2021
  machine-key handshake succeeds against Headscale. Validated live against
  Headscale v0.28.0 over both plain HTTP (`http://host:8080`) and HTTPS
  behind a Let's Encrypt proxy; the hosted-Tailscale path re-validated over
  both plain TCP and `https://controlplane.tailscale.com`. (microlink
  submodule; ports esphome-tailscale `5ecc5ee` + `9be7d81`.)

### Fixed

- **Netmap arrives via the streaming long-poll (Headscale ≥ 0.26).**
  Headscale only delivers the full netmap on a streaming map request — the
  old one-shot fetch got an empty response and reconnect-looped ("Empty
  MapResponse"). The initial netmap is now consumed from the long-poll
  stream; the hosted-Tailscale flow is unchanged.
- **First-boot AP is discoverable.** (#9) Default SSID renamed `myssid` →
  `ESP32-TSR-Setup`, the README now documents the default credentials and
  `http://192.168.4.1`, and boot prints the AP name + web-UI URL on serial
  at the default log level.

## [0.1.17] — 2026-06-11

Stable release. Promotes the 0.1.16 fixes (exit-node TAI64N handshake timestamps,
DNS-relay task-stack hardening, OTA beta-channel JSON buffer) to a stable build;
no functional changes versus 0.1.16.

## [0.1.16] — 2026-06-11

This release folds in the fixes that had only been serial-flashed since 0.1.12
(0.1.13–0.1.15 were test builds, never published).

### Fixed

- **Exit node now establishes itself reliably (WireGuard handshake timestamps).**
  The handshake's TAI64N timestamp is now sampled from the SNTP wall clock on every
  emit, instead of a per-boot counter that could fall behind the value a peer had
  already stored and get every initiation silently dropped as a replay. The ESP now
  brings up an exit-node tunnel on its own and self-heals across reboots, so AP
  clients keep their internet through the exit node. (microlink submodule.)
- **DNS relay no longer crash-loops at boot under verbose logging.** The relay
  listener/worker task stacks were enlarged (to 8 KB) so enabling INFO-level SD
  recording at boot can no longer overflow them into a panic loop.
- **Firmware-update check on the beta channel no longer fails to parse.** The
  `/releases` JSON the beta channel scans had grown past the 32 KB download buffer
  and was being truncated, so "Check now" reported `parse failed (no firmware.bin
  asset?)` even though every release ships the asset. The buffer is now 128 KB.

## [0.1.9] — 2026-06-04

### Added

- **Source-NAT advertised routes** — an opt-in toggle (Tailscale settings, off by
  default) that masquerades tunnel→LAN forwarded traffic to this device's uplink
  IP, the way Tailscale's `--snat-subnet-routes` does. With it on, advertising the
  uplink subnet makes that LAN reachable from the tailnet out of the box (the
  upstream router no longer needs a route back to the tailnet). Enabling it offers
  to add the live uplink subnet to the advertised routes.
- **Subnet prefix in the Status cards** — the Uplink and Access Point cards now
  show the address with its CIDR prefix (e.g. `192.168.4.1/24`, `192.168.32.1/24`).
- **Factory-reset dialog recommends a backup** — the confirmation now highlights
  that you should download an encrypted config backup first, with a one-click
  "Back up first" link into the backup flow.
- **Project logo in the header** — the web UI nav brand icon now uses an inline
  chip + WiFi mark (crisp vector, stays sharp at small sizes) instead of the
  generic placeholder icon.
- **Mascot in the Support card** — a small hand-drawn mascot next to the
  "Buy me a coffee" button, centred with it.

### Changed

- **OTA version compare is pre-release-aware** — the updater now follows SemVer
  pre-release precedence (`0.1.9-beta1 < 0.1.9-beta2 < … < 0.1.9`), so a device on
  one beta is offered the next beta and the final stable release supersedes every
  beta of that version. Beta builds carry the pre-release tag in their version
  string. Previously the suffix was ignored, so successive betas of the same
  `x.y.z` were never offered.

## [0.1.8] — 2026-06-03

### Added

- **OTA beta channel** — an opt-in toggle (System → Firmware update) that makes
  the updater also offer GitHub **pre-releases** (highest semver wins), so test
  devices can pull beta builds before they go stable. Off by default, so
  production devices keep tracking only stable releases (`/releases/latest`).
  Promote a beta by publishing it as a full (non-pre-release) release.

## [0.1.7] — 2026-06-03

Reliability-focused early-access update: over-the-air updates are now
dependable, crash reports are actionable, and the Tailscale client (microlink)
is consolidated. No breaking changes — settings and tailnet identity are
preserved across the update.

### Fixed

- **OTA updates are now durable** — the running image marks itself valid after a
  healthy boot, so the bootloader rollback no longer reverts a fresh update on
  the next reboot.
- **OTA downloads from GitHub Releases succeed** — the updater's HTTP buffer was
  too small to hold the release-asset redirect header (it failed with
  `ESP_FAIL`).
- **A failed OTA no longer crashes the device** — the install path used to panic
  and reboot on any download error (risking a loop with auto-install enabled); it
  now surfaces the error and keeps running.
- **Crash signatures are debuggable** — panic reports captured only the generic
  `abort()` frames; they now record the backtrace through to the real fault
  (Diagnostics → Reset history, and telemetry).

### Changed

- **AP channel is read-only**, auto-following the uplink channel (single radio).
  Removed the non-functional manual channel control and the dead "Disable web
  interface" placeholder.
- **microlink** consolidated onto a single maintained line.
- **Telemetry hardened** — the anonymous device hash now carries an integrity
  check so the collector can drop spoofed/garbage events; the collector keeps
  only coarse country/region and never stores a raw IP.

### Known limitations

- **Headscale does not currently work** — the ts2021 control-plane Noise
  handshake fails (tracked in
  [#7](https://github.com/Csontikka/esp32-tailscale-subnet-router/issues/7));
  supersedes the "untested" note in 0.1.0. Hosted Tailscale is unaffected.
- Carried over from 0.1.0: single 2.4 GHz radio (channel realign after a roam),
  MCU-class throughput, tailnet lock unsupported.

## [0.1.0] — 2026-05-31

First public early-access release. The firmware turns one ESP32-S3 into
a WiFi NAT router and a Tailscale subnet router, configured entirely
from a built-in web UI.

### Added

- **Dual role**: simultaneous WiFi STA uplink + AP (NAPT, DHCP, DNS
  forwarder) + Tailscale subnet router.
- **Tailscale stack** via [microlink](https://github.com/Csontikka/microlink):
  DISCO discovery, direct paths with DERP relay fallback, NAT traversal,
  exit-node client and gateway, DERP-aware automatic MTU.
- **Exit-node aware routing** with fail-closed behaviour, and DERP
  re-handshake on the WireGuard data plane so an exit-node session
  survives a direct↔DERP transition.
- **ACL firewall**: four hook points (Internet↔ESP, Clients↔ESP),
  first-match-wins rules by protocol / CIDR / port / action, per-rule
  hit counters.
- **DNS forwarder** for AP clients with a PSRAM-backed response cache,
  worker pool, and cache stats.
- **Diagnostics**: on-device ping, traceroute, route-explain, a 1 MB
  download/upload speed test (cancellable), live WiFi scan, and a
  microSD "flight recorder" for control-plane stalls.
- **DHCP**: reservations, live lease table with per-client signal, MAC
  denylist; **port forwarding**.
- **Web UI**: responsive single-page admin served from the device —
  first-run password setup, WiFi join, tailnet enrolment, firewall,
  diagnostics, system.
- **Operations**: encrypted config backup/restore, OTA updates, per-sink
  (console + SD) log levels with an INFO ceiling, pre-crash log capture,
  auto AP-channel realign on STA roam.
- **Anonymous telemetry** (on by default, one-toggle opt-out): daily
  salted one-way device hash + boot/flash counters + reboot/crash cause +
  firmware/chip/uptime; no SSIDs/IPs/MACs/tailnet/peers. Fully inspectable
  in `main/telemetry.c`.
- **Project hardening**: SECURITY policy, CodeQL (C/C++ + Python),
  Dependabot, secret scanning, and a custom Sensitive Data Check.

### Known limitations

- Single 2.4 GHz radio shared by STA and AP (channel realign needed
  after an upstream roam).
- MCU-class throughput (a few Mbit/s through the tunnel).
- **Headscale is untested**; only hosted Tailscale has been validated.
- Tailnet lock is unsupported.

[Unreleased]: https://github.com/gszigethy/esp32-tailscale-subnet-router/compare/v0.1.20-W5500...HEAD
[0.1.20-W5500]: https://github.com/gszigethy/esp32-tailscale-subnet-router/releases/tag/v0.1.20-W5500
[0.1.19-W5500]: https://github.com/gszigethy/esp32-tailscale-subnet-router/releases/tag/v0.1.19-W5500
[0.1.9]: https://github.com/Csontikka/esp32-tailscale-subnet-router/releases/tag/v0.1.9
[0.1.8]: https://github.com/Csontikka/esp32-tailscale-subnet-router/releases/tag/v0.1.8
[0.1.7]: https://github.com/Csontikka/esp32-tailscale-subnet-router/releases/tag/v0.1.7
[0.1.0]: https://github.com/Csontikka/esp32-tailscale-subnet-router/releases/tag/v0.1.0
