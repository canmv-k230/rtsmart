# RT-Thread WLAN offload framework

This directory provides an RT-Thread-native control plane for Wi-Fi devices
whose firmware owns the 802.11 MAC. It is intentionally similar to Linux
`cfg80211` at the behavioral level, but it is not a Linux ABI and contains no
Linux kernel object types. The existing RT-Thread WLAN management, netdev, and
lwIP layers remain the public application interface.

The framework is intended for:

- ESP-Hosted-NG with a host supplicant and explicit auth/assoc/key commands.
- ESP-Hosted-FG with connection and AP management offloaded through its RPC
  control channel.
- AIC8800 firmware using its command/event protocol.
- SPI, SDIO, and USB host interfaces without coupling a vendor protocol to a
  particular RT-Thread bus driver.

It does not replace the current ESP-Hosted-MCU driver in
`kernel/bsp/maix3/drivers/extdrv/esp_hosted_mcu`. NG and FG are separate protocol
implementations and should be added as new WLAN offload vendor drivers.

## Object model

```text
RT-Thread WLAN management / lwIP       wpa_supplicant / WLAN offload example
                 |                                  |
        rt_wlan_device (STA/AP)              userspace libwlan_offload
                 |                                  |
                 +------------ rt_wlan_offload_radio ----+
                 /       |       \
       capabilities     vifs     rt_wlan_offload_ops
       bands/channels             (vendor core)
       ciphers/regulatory               |
                                       vendor command/event protocol
                                                    |
                                          rt_wlan_offload_bus
                                          /      |      \
                                        SPI     SDIO     USB
```

`rt_wlan_offload_radio` is the physical Wi-Fi device, comparable to the useful
parts of a cfg80211 `wiphy`. It publishes immutable capabilities, supported
bands/channels/rates, cipher suites, interface combinations, regulatory data,
and scan limits. `rt_wlan_offload_vif` represents an interface. The compatibility
adapter currently registers the station and AP interfaces supported by the
existing `rt_wlan_device` API.

`rt_wlan_offload_ops` is the vendor boundary. It contains no framing or bus logic.
`rt_wlan_offload_bus` only moves opaque vendor frames and reports transport state;
it does not interpret ESP or AIC messages.

## CEVA reference findings

The RivieraWaves reference separates its host-facing Wi-Fi API from firmware
IPC, command management, TX scheduling, and transport/platform code. That is
the useful boundary for RT-Smart. This framework intentionally supports only
devices whose firmware owns the MAC; it does not provide or plan a SoftMAC
stack.

| Reference subsystem | RT-Smart placement |
| --- | --- |
| firmware-MAC host operations | `rt_wlan_offload_radio`, VIFs, operations, events |
| request/confirmation tracking | `rt_wlan_offload_command_manager` |
| firmware version/features/limits | `rt_wlan_offload_firmware_info` |
| data and control queue separation | vendor core plus priority-aware `rt_wlan_offload_bus` |
| VIF, peer, channel-context tables | vendor core, sized from negotiated firmware limits |
| SDIO/SPI/USB mechanics | transport driver below `rt_wlan_offload_bus` |

The reference sources are GPL and are used only as an architectural and
behavioral reference. This implementation is independent and uses RT-Thread
types and synchronization primitives.

## Control model

Operations which complete later carry a nonzero `request_id`. A driver returns
`RT_EOK` when it has accepted the request, then reports completion with
`rt_wlan_offload_report_event()` using the same ID. The framework rejects stale scan,
connect, and AP completions. Request IDs can be allocated with
`rt_wlan_offload_alloc_request_id()`.

The principal mappings are:

| WLAN offload request | WLAN offload completion | RT-Thread event |
| --- | --- | --- |
| `scan` | `SCAN_RESULT`, `SCAN_DONE` | `SCAN_REPORT`, `SCAN_DONE` |
| `connect` | `CONNECT_RESULT` | `CONNECT` or `CONNECT_FAIL` |
| `disconnect` | `DISCONNECTED` | `DISCONNECT` |
| `start_ap` | `AP_STARTED` | `AP_START` |
| `stop_ap` | `AP_STOPPED` | `AP_STOP` |
| `del_station` | `DEL_STATION` | `AP_DISASSOCIATED` |
| firmware station event | `NEW_STATION` | `AP_ASSOCIATED` |

Channel-bearing requests and events use `rt_wlan_offload_channel_definition`, not a
bare channel number. It carries the band, primary channel and frequency,
channel width, and both center frequencies, matching the useful cfg80211
channel-definition model for 2.4/5/6 GHz and 20 through 320 MHz operation. The
framework validates the definition against the radio's published channel table
and per-channel bandwidth restrictions before calling a vendor driver.

Management RX, auth RX, assoc RX, management TX status, EAPOL, external-auth,
regulatory changes, and firmware errors are delivered to the optional WLAN offload
event handler. With `RT_WLAN_OFFLOAD_CONTROL`, a radio can expose operations
and events through an exclusive-open `/dev/wlanctlN` control device. The private
control protocol carries `RT_WLAN_OFFLOAD_EVENT_EXTERNAL_AUTH_REQUIRED` and an
external-auth response command, so a userspace supplicant can complete an SAE
exchange without including kernel headers. Host-supplicant drivers additionally
enable `RT_WLAN_OFFLOAD_SUPPLICANT` and advertise
`RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT`; firmware-offload drivers can expose the
control device for diagnostics without that capability.

Control device names are assigned by the framework, not by the driver, so
several vendor drivers can be built into one image without colliding. The
assigned index follows the radio's phy index: `wlanctl0` belongs to the same
radio as `phy0-sta` and `phy0-ap`. Drivers read the assigned name back with
`rt_wlan_offload_control_get_name()`, and userspace reads all three names plus
the radio index from the control device's names command.

Operation request pointers and event payload pointers are borrowed. A driver
must copy request data before its operation callback returns. The framework and
event consumer must copy event data before `rt_wlan_offload_report_event()` returns.
Vendor callbacks must enqueue long-running work: they must not wait for an
asynchronous WLAN offload completion while still inside the callback.

The framework serializes command submission across station and AP interfaces.
Ethernet TX has its own lock, and event and Ethernet RX paths remain independent
so firmware responses and data traffic can make progress while a command is
being submitted. Framework callbacks run without the internal state mutex held,
so an event consumer may submit a follow-up operation. Driver event workers must
be stopped and flushed before `rt_wlan_offload_unregister_radio()` returns.

## Firmware protocol runtime

`wlan_offload_command.h` provides the common request/confirmation mechanism needed
by CEVA-style IPC, AIC firmware commands, and ESP-Hosted control messages. It
provides nonzero tokens, a bounded pending list, response matching, response
size checking, timeouts, failure fan-out, and reset after recovery. It does not
define vendor message IDs or wire layouts.

A vendor core initializes one manager with its protocol-specific `push()`
callback, calls `rt_wlan_offload_command_execute()` from a command worker or a
synchronous operation callback, and passes decoded confirmations to
`rt_wlan_offload_command_complete()`. A fatal firmware or transport error calls
`rt_wlan_offload_command_manager_fail()` before queues are torn down; after firmware
restart and compatibility checks it calls `rt_wlan_offload_command_manager_reset()`.
Use `max_pending = 1` for protocols which cannot disambiguate two responses.
Protocols supporting parallel commands must encode the manager token on the
wire and return it with the confirmation.

At registration, drivers set `rt_wlan_offload_radio_config.api_version` to
`RT_WLAN_OFFLOAD_API_VERSION`. Firmware start should query the device protocol
version, feature bits, and resource limits, reject incompatible firmware, then
publish the negotiated values with `rt_wlan_offload_update_firmware_info()`.
`firmware_generation` changes on each successful firmware start/online cycle,
allowing diagnostics to identify a restarted session. The `/dev/wlanctlN` info
reply exposes these values without exposing kernel headers to userspace.

## RT-Thread compatibility

`rt_wlan_offload_register_radio()` creates the requested `rt_wlan_device` instances.
The adapter translates current WLAN requests into the richer WLAN offload request
structures. This preserves existing calls such as `rt_wlan_scan()`,
`rt_wlan_connect()`, and `rt_wlan_start_ap()` for firmware which implements
connection offload.

Applications can select an AP band and primary channel with
`rt_wlan_start_ap_with_channel()`. The adapter resolves the channel inside the
requested band and rejects disabled, no-IR, and DFS channels before invoking the
vendor driver. The legacy `rt_wlan_start_ap()` entry point continues to select
2.4 GHz channel 6. Because the RT-Thread compatibility API has no width field,
the adapter selects VHT80 for a 5 GHz VHT radio when all four constituent
channels are available; otherwise it retains 20 MHz.

ESP-Hosted-FG fits that mode directly. For firmware command devices such as AIC,
`RT_WLAN_OFFLOAD_EMBEDDED_WPA2` adds an in-kernel Personal-security path. The
option supports WPA-PSK/TKIP or CCMP, WPA2-PSK with TKIP or CCMP,
WPA2-PSK-SHA256/CCMP, WPA2 CCMP with required management-frame protection,
and WPA3-SAE/CCMP. PSK-SHA256 uses the SHA-256 PTK KDF, AES-CMAC EAPOL-Key
integrity, and BIP-CMAC-128 management protection. The implementation retains
a bounded copy of each scanned BSS's WPA/RSN IEs, selects the AP's actual
pairwise/group profile, and falls back to ordinary PSK when an AP advertises
PSK-SHA384, FT-PSK, or a WPA2/WPA3 transition AKM alongside ordinary PSK. It
derives or negotiates the PMK, handles EAPOL messages 1 through 4, installs
pairwise, group, and management-protection keys, and delays the RT-Thread
CONNECT event until the controlled port is usable. Applications continue to
call `rt_wlan_connect()` and do not need a userspace process.

`RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD` provides the corresponding compact AP-side
path for drivers that advertise `RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR`.
It supports bare open SoftAP and WPA2-PSK/CCMP SoftAP, generates the required
management responses, runs the authenticator side of the four-way handshake,
installs GTK/PTK material, and authorizes each station only after message 4.
It deliberately does not implement WPA/TKIP, WPA3, Enterprise/802.1X, WPS,
FT, PMF, RADIUS, or per-station VLAN policy; those deployments still require
a complete hostapd port.

`rt_wlan_security_t` also classifies WPA3-Enterprise, OWE, SHA-256, FT, FILS,
DPP, OSEN, WAPI, and CCKM scan results. These values do not imply that the
compact embedded supplicant implements those protocols. `wifi join` uses the
embedded path only for its supported Personal modes and the documented
ordinary-PSK fallbacks; standalone SHA-384, FT, FILS, DPP, OSEN, and other
credential-rich modes require a complete userspace supplicant through the
WLAN offload control device unless a vendor driver explicitly offloads them.
Standalone WPA2 PSK-SHA256 is supported with CCMP. Required PMF currently
supports BIP-CMAC-128; BIP-GMAC and 256-bit management ciphers are rejected
during profile selection.

WPA3 currently implements SAE group 19 with the hunting-and-pecking PWE method
and mandatory management-frame protection. SAE H2E, anti-clogging token retry,
SAE-PK, FT-SAE, and enterprise authentication remain outside this compact
embedded path. The SAE arithmetic uses the bundled BSD-licensed TinyCrypt
P-256 implementation; `tests/wlan_offload_sae_test.c` covers the IEEE 802.11 Annex
J.10 commit/KCK/PMK/PMKID vector and a complete two-peer confirm exchange.

The complete external-supplicant boundary remains available through
`rt_wlan_offload_auth()`, `rt_wlan_offload_assoc()`, the key APIs, and
`rt_wlan_offload_transmit_mgmt()`. Drivers that delegate SAE report
`RT_WLAN_OFFLOAD_EVENT_EXTERNAL_AUTH_REQUIRED` and accept completion through
`rt_wlan_offload_external_auth_response()`. The userspace client library is under
`src/rtsmart/libs/wlan_offload`; its public `wlan_offload_client.h` has no dependency on
RT-Thread or kernel headers. The private kernel wire definition in
`wlan_offload_control_protocol.h` must never be installed as an SDK header.

Ethernet RX frames enter through `rt_wlan_offload_rx()`. They must not include the
vendor transport header. Ethernet TX arrives at `rt_wlan_offload_ops.transmit()`;
the vendor core adds its protocol header and sends the resulting frame through
`rt_wlan_offload_bus_transmit()`. For an active embedded Personal-security
connection, the core
consumes Ethernet type `0x888e` in-kernel. Other EAPOL frames on an external-
supplicant radio are diverted to the control-device event queue; the framework
also builds Ethernet frames for EAPOL TX requests.

## Transport contract

All transports implement the same lifecycle:

```text
init -> STOPPED -> STARTING -> STARTED -> SUSPENDED
                    |            |           |
                    +---------- FAILED <-----+
                                 |
                               STOPPED
```

`rt_wlan_offload_bus_ops.transmit()` and `transmit_priority()` have synchronous
buffer ownership: after they return, their caller may reuse the input buffer. A
DMA or asynchronous transport therefore copies or takes an internal reference
before returning. Bus operations are serialized separately from bus state and
callbacks, so a synchronous transport operation may report an event without
recursing into the state mutex. Transport RX and event notification must run in
a worker thread, never directly in a hard interrupt handler.

The bus RX callback returns `RT_EOK` only when the vendor protocol consumes a
valid frame. It returns `-RT_EEMPTY` for padding or an empty transfer and an
error for malformed input. Event-driven transports can use this result to stop
draining or apply a bounded retry delay instead of spinning on a stuck-ready
signal.

A transport error changes the radio to `FAILED`, completes pending WLAN state,
and queues recovery on the system workqueue. Recovery stops the failed vendor
stack and bus, starts a new firmware generation, and restores each WLAN
interface whose RT-Thread mode was active. Drivers should report persistent
transport failures with `RT_WLAN_OFFLOAD_BUS_EVENT_ERROR`; they should not retry
forever in a transport worker.

The shared bus API standardizes type, lifecycle, maximum frame sizes,
alignment, headroom/tailroom, reset, suspend/resume, hotplug notification,
priority selection, and opaque frame delivery. A transport advertising
`RT_WLAN_OFFLOAD_BUS_CAP_TX_PRIORITY` implements `transmit_priority()`; otherwise
priority requests fall back to `transmit()`. The following details remain in
each transport driver:

- SPI: controller configuration, fixed-size full-duplex exchange, handshake
  and data-ready GPIO IRQs, reset GPIO, padding, and an RX/TX worker. ESP-Hosted
  normally uses 1600-byte exchanges; this is a vendor transport setting, not a
  WLAN offload constant.
- SDIO: function probe/remove, CCCR/FBR setup, CMD52/CMD53 accesses, block size,
  function IRQ acknowledgement, flow-control counters, and worker scheduling.
- USB: VID/PID and interface matching, endpoint discovery, bulk URB equivalent
  queues, disconnect cancellation, and hotplug events.

A vendor transport is expected to embed or own one `rt_wlan_offload_bus`, fill
`rt_wlan_offload_bus_config`, and call `rt_wlan_offload_bus_rx()` after stripping no data:
the buffer passed upward is the complete opaque vendor frame. The vendor core
then parses and validates it.

## Vendor driver layout

Keep protocol, transport, and OS adaptation separate:

```text
kernel/bsp/maix3/drivers/extdrv/
    esp_hosted/
        esp_hosted_wifi.c      common radio integration
        esp_hosted_ng.c        NG command/event codec and operations
        esp_hosted_fg.c        FG protobuf/RPC control and operations
        esp_hosted_transport_spi.c
        esp_hosted_transport_sdio.c
    aic8800/
        aic8800_wifi.c         radio operations and event dispatch
        aic8800_firmware.c     firmware loading and boot setup
        aic8800_usb.c          USB transport
```

Do not put SPI/SDIO/USB calls in the radio integration file, and do not expose
`rt_wlan_device` to protocol parsers. The vendor core is the only layer which
knows both the WLAN offload request/event model and the vendor wire protocol.

### ESP-Hosted-NG

Use `<esp-hosted>/esp_hosted_ng/host/esp_cfg80211.c` as a behavior map for
radio capabilities and operations, `esp_cmd.c` plus `include/adapter.h` for
command/event formats, and `host/spi` or `host/sdio` for the wire protocol.
Replace Linux work queues, sk_buffs, net_device, and cfg80211 calls with
RT-Thread queues, owned buffers, `rt_wlan_offload_bus`, and `rt_wlan_offload_report_event`.
Do not wrap or emulate Linux kernel objects.

NG is the host-supplicant personality. Its adapter should advertise
`RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT` and implement auth, assoc, key, management
TX, scan, disconnect, and AP callbacks as supported by the selected firmware.

### ESP-Hosted-FG

Use `<esp-hosted>/esp_hosted_fg/common` for the shared framing and protobuf
definitions, and `host/control_lib` for RPC behavior. The RT port
should replace the platform wrapper and allocation/thread primitives rather
than importing the Linux host application. FG maps RPC scan/connect/softAP
commands to the offloaded WLAN offload callbacks and maps asynchronous RPC events to
WLAN offload events.

### AIC8800

Keep firmware loading and chip-specific boot/setup in the AIC core. Map the
firmware's cfg80211-facing command set to the same radio/vif operations and
events, then attach either the SDIO or USB transport. Firmware and bus variants
must publish their actual band, cipher, offload, concurrency, and scan limits;
they must not advertise a capability solely because the framework defines it.

## Registration outline

Each vendor instance follows this order:

1. Allocate driver state, queues, and RX/event worker threads.
2. Initialize exactly one SPI, SDIO, or USB `rt_wlan_offload_bus`.
3. Install bus RX/event callbacks which feed the vendor core.
4. Fill static radio metadata and `rt_wlan_offload_ops`, including
   `api_version = RT_WLAN_OFFLOAD_API_VERSION`.
5. Set `control_device` when exposing a userspace control device.
   External-supplicant radios must set it. Then call
   `rt_wlan_offload_register_radio()`.
6. On WLAN initialization, let the framework start the bus, reset firmware,
   query and validate its protocol/features/limits, publish
   `rt_wlan_offload_firmware_info`, then bring the vendor core to command-ready
   state. Use radio online/offline events for later hotplug or firmware
   availability changes.
7. During removal, stop new firmware traffic, flush workers, unregister the
   radio, then deinitialize the bus.

The metadata arrays referenced by `rt_wlan_offload_radio_config` remain owned by the
driver and must remain valid until unregister completes.

## Source and license policy

Linux cfg80211 is the behavioral reference because ESP-Hosted-NG and common
AIC drivers already target it. The RT-Thread API must remain independently
implemented and Apache-2.0 compatible. Linux GPL implementation code and Linux
internal structures must not be copied into this framework. Review the license
of every vendor protocol, firmware, and utility file before porting it; reuse
Apache-compatible protocol definitions where available and rewrite OS-specific
glue against the interfaces in this directory.
