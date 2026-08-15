# AIC8800 USB/SDIO Wi-Fi and USB Bluetooth driver

This directory contains the Apache-2.0 RT-Smart AIC8800 source implementation:

- an RT-Thread WLAN offload Wi-Fi driver;
- a USB boot-ROM firmware loader for AIC8800, AIC8800D80/D40, and
  AIC8800D80X2 devices;
- an SDIO transport and boot-ROM firmware loader for AIC8801 and
  AIC8800D80 devices;
- a USB Bluetooth HCI transport for BLE host stacks.

`SConscript` builds the top-level `aic8800_*.c` files and the fixed-width
protocol declarations in `port/aic8800_protocol.h`. All source files in this
component are licensed under Apache-2.0.

## Firmware loading

Disable `RT_USING_ESP_HOSTED_WIFI`, enable `RT_USING_AIC8800_WIFI`, then
enable `AIC8800_WIFI_TRANSPORT_USB`, `AIC8800_WIFI_TRANSPORT_SDIO`, or both.
Each transport owns an independent device context and may attach at the same
time. Radios are numbered in registration order. Their network interfaces use
Linux-style names: `wlanN` for station mode and `wlanNap` for SoftAP mode. USB
Ethernet interfaces use `ethN`. The WLAN management devices use `phyN-sta` and
`phyN-ap`; applications should use the network-interface names or query the
selected `WLAN` object. The private control device is assigned by the WLAN
core and follows the same index, so a radio using `phyN-sta` and `phyN-ap`
owns `/dev/wlanctlN`. Nothing in this driver hard-codes those names, so it can
be built alongside other WLAN offload drivers.
RT-Thread's legacy `wifi` shell commands and high-level `rt_wlan_*` management
API still keep one active station device and one active AP device globally, so
they target the most recently selected pair. If that pair is unplugged, the
WLAN core immediately selects an available SDIO, SPI, then USB replacement;
connect, scan, and AP-start operations also perform this selection lazily.
Netmgmt can select the physical radio before using those role-based calls.
CanMV performs that selection for each `WLAN` object operation:

```python
import network

usb = network.WLAN(network.STA_IF, network.WLAN_USB)
sdio = network.WLAN(network.STA_IF, network.WLAN_SDIO)
automatic = network.WLAN(network.STA_IF)  # Backward-compatible AUTO mode.

usb.connect("usb-ap", "password")
sdio.connect("sdio-ap", "password")
network.set_default_dev(sdio)

# Radio numbers follow probe order; query when the transport matters.
print(usb.netdev_name())
print(sdio.netdev_name())
```

`network.WLAN_AUTO`, `network.WLAN_USB`, `network.WLAN_SDIO`, and
`network.WLAN_SPI` are also valid with `network.AP_IF`. AUTO keeps the current
radio while it is present.
If a selected USB radio is unplugged, the next AUTO operation selects the
non-removable SDIO radio, or USB when SDIO is unavailable. An explicitly
selected USB object reports unavailable after unplug, while an explicitly
selected SDIO object continues to address SDIO regardless of its radio number.

Native RT-Smart applications select the radio before the existing netmgmt
operation:

```c
netmgmt_wlan_select_device(NETMGMT_WLAN_DEVICE_SDIO,
                           RT_NET_DEV_WLAN_STA);
netmgmt_wlan_sta_connect_with_ssid("sdio-ap", "password");
```

The selector and following legacy operation are separate calls, so callers
that share netmgmt between threads must serialize each select-and-operate
sequence. Each radio's `/dev/wlanctlN` remains available for direct,
independent transport control; the attachment log line reports the name each
device was given.
The default firmware root is
`/bin/firmware`; it can be changed with
`AIC8800_WIFI_FIRMWARE_PATH`. The packaged firmware is separated by transport
so files with the same vendor name cannot be mixed:

```text
firmware/
|-- usb/
|   |-- aic8800/
|   |-- aic8800D80/
|   |-- aic8800D80X2/
|   `-- aic8800DC/
`-- sdio/
    |-- aic8800/
    `-- aic8800D80/
```

For a single-transport build, the SCons component stages the selected
`firmware/usb/` or `firmware/sdio/` subtree directly under the RT-Smart firmware
directory. The RT-Smart image step therefore installs the files with this
layout:

```text
/bin/firmware/
|-- aic8800/
|   |-- fmacfw.bin
|   |-- fw_adid.bin
|   |-- fw_adid_u03.bin
|   |-- fw_patch.bin
|   |-- fw_patch_u03.bin
|   |-- fw_patch_table.bin
|   `-- fw_patch_table_u03.bin
|-- aic8800D80/
|   |-- fmacfw_8800d80_u02.bin
|   |-- fmacfw_8800d80_h_u02.bin
|   |-- fw_adid_8800d80_u02.bin
|   |-- fw_patch_8800d80_u02.bin
|   |-- fw_patch_8800d80_u02_ext0.bin
|   `-- fw_patch_table_8800d80_u02.bin
`-- aic8800D80X2/
    |-- fmacfw_8800d80x2.bin
    |-- fw_adid_8800d80x2_u03.bin
    |-- fw_patch_8800d80x2_u03.bin
    `-- fw_patch_table_8800d80x2_u03.bin
```

For a dual-transport build, both trees are retained so same-named USB and SDIO
firmware cannot overwrite each other:

```text
/bin/firmware/
|-- usb/
|   |-- aic8800/
|   |-- aic8800D80/
|   |-- aic8800D80X2/
|   `-- aic8800DC/
`-- sdio/
    |-- aic8800/
    `-- aic8800D80/
```

The packaged binaries are copied verbatim from the AIC firmware package's
`fw/` directory. Their distribution terms are independent of this source code;
confirm the vendor redistribution grant before publishing binary images.

With `AIC8800_WIFI_FORCE_FIRMWARE_DOWNLOAD=y`, an initially detected
runtime PID is rebooted to its boot PID. The loader streams firmware in 1 KiB
debug-memory commands, applies the vendor patch table, starts the application,
and waits for the runtime PID to re-enumerate. If the required files are not
present, an already-running runtime device is used without rebooting.

SDIO devices do not re-enumerate after firmware startup. The driver downloads
the transport-specific FMAC image through function 1, starts it in place, then
registers the WLAN offload radio. If card discovery completes before `/bin` is
mounted, attachment waits for the firmware files up to the configured command
timeout. Do not substitute the USB FMAC binaries: the files have the same
vendor names but different contents.

Supported Wi-Fi USB IDs are:

| Family | Boot ID | Runtime ID |
| --- | --- | --- |
| AIC8800/AIC8801 | `a69c:8800` | `a69c:8801` |
| AIC8800D80 | `a69c:8d80` | `a69c:8d81`, `a69c:8d83`-`8d88` and the `368b` equivalents |
| AIC 88M80 (WiFi 6 + BT) | `1111:1111` (MSC mode), then `a69c:8d80` | `a69c:8d81` composite |
| AIC8800D40 | `a69c:8d40` | `a69c:8d41` |
| AIC8800D80X2 | `368b:8d90` | `368b:8d91`, `368b:8d99` |
| AIC8800DC/DW | `a69c:5721` (AIC8800FC MSC mode) | `a69c:88dc`, `a69c:88dd` |

An AIC8800D40 module reports the D80/D81 IDs above and its `MM_VERSION` PHY
register advertises an 80 MHz-capable modem, so neither the product ID nor the
firmware capabilities can distinguish it from an 80 MHz part. Set
`AIC8800_WIFI_USB_LIMIT_40MHZ` for D40 boards. Leaving it clear makes the
driver advertise 80 MHz bandwidth, short GI 80, an 80 MHz VHT highest data
rate, and 80 MHz HE PPE thresholds on hardware that cannot use them; on a
40 MHz link this was observed to hold the rate controller at MCS7 where the
corrected capabilities reach MCS9.

Supported Wi-Fi SDIO IDs are:

| Family | Manufacturer | Function 1 product |
| --- | --- | --- |
| AIC8801 | `5449` | `0145` |
| AIC8800D80 | `c8a1` | `0082` |

The SDIO transport uses 512-byte fixed-address FIFO transfers, firmware flow
control, interrupt-driven receive processing, and the D80 header CRC-8. It does
not currently enable the optional function-2 Bluetooth channel. Configure the
board's controller with `BSP_WIFI_SDIO_HOST_0` or `_HOST_1`; set
`BSP_WIFI_SDIO_REG_ON_PIN` when the module power-enable signal is driven by a
K230 GPIO. The 01Studio configuration builds both transports so USB and SDIO
remain compile-tested; the matching device on an enabled bus selects the path.

The packaged firmware does not provide the older D80 U01 or D80X2 U05 files.
The loader recognizes their vendor filenames but will fail clearly until those
matching files are installed. AIC8800FC devices initially expose a fake MSC
interface at `a69c:5721`; the USB driver walks the standard mass-storage
initialization and issues the bulk-only SCSI eject, then binds the device
after it re-enumerates with its AIC runtime ID.

`1111:1111` is deliberately not claimed by default. It is not an AIC identity:
AIC's USB-IF vendor ID is `0xa69c`, and no INF in any vendor Windows driver
package binds `1111:1111`. It is the placeholder that unprogrammed and no-name
devices ship with, so claiming it would take the mass-storage interface away
from unrelated devices and eject them.
`AIC8800_WIFI_USB_MODESWITCH_PLACEHOLDER_ID` enables it for bring-up.

Not every fake-storage module switches on an eject. The AIC 88M80 WiFi 6 +
Bluetooth module reports `1111:1111` with USB strings `'AIC'` / `'88M80'` and
SCSI INQUIRY `'LGX' 'WIFI6' rev '2.30'`, peripheral device type `0x00`. It
accepts an eject with a good status and stays in mass-storage mode; worse, the
eject removes its medium and its private command channel then refuses
everything, so the eject has to be skipped entirely for this module.

It switches on a private command instead. `tool/Usb_Driver.dll` in the vendor
`Wifi6_install_bt` package sends these through `IOCTL_SCSI_PASS_THROUGH_DIRECT`
as 16-byte CDBs, opcode `0xfd` with the sub-command in the last byte:

| export | CDB | data |
| --- | --- | --- |
| `GetHippo` | `fd 00 .. 00 f3` | reads 5 bytes |
| `Set_CS1_0` | `fd 00 .. 00 f2` | none |

The routine in `AicWifiService.exe` that calls them opens the device, reads the
five identification bytes, compares them against an expected string, and only
then issues `Set_CS1_0`. It never ejects. The module answers `"88M80"`, matching
its USB product string, and acts on `Set_CS1_0` once the bus is cycled - the
vendor service follows up with `devcon rescan`, and this driver gets the same
effect by failing the probe once so the hub re-resets the port. The module then
enumerates as `a69c:8d80`, takes the normal D80 firmware download, and returns
as `a69c:8d81` with Bluetooth on interfaces 0-1 and Wi-Fi on interface 2.

Because `1111:1111` is a shared placeholder rather than an AIC identity, the
driver treats the private command as the only proof of identity: a device at
that ID which does not answer `0xf3` is left completely untouched rather than
being ejected. It is still claimed, though - the USB host core resolves one
class driver per interface and cannot hand a declined device to another - so
`AIC8800_WIFI_USB_MODESWITCH_PLACEHOLDER_ID` exists to drop the ID entirely on
systems where something else uses it. It defaults to on so the 88M80 works
without configuration.

AIC8800DC/DW WLAN startup uses the vendor v2 TX-power ABI, revision-specific
2.4 GHz TX gain tables, 20/40 MHz RX gain tables, and DC calibration mask. The
driver reads the chip and sub-revision before starting the firmware stack so
the matching request formats are selected correctly. AIC8800DC/DW U02 non-H
and H silicon receive their matching vendor system configuration, Wi-Fi ROM
patch, LDPC/AGC/TX-gain tables, patch descriptor, and DPD calibration helper
before the patched ROM is started. Other DC/DW ROM patch revisions are
rejected explicitly until their matching host tables are ported.

## Wi-Fi and BLE interfaces

After runtime enumeration, Wi-Fi is registered through the RT-Thread WLAN offload
framework as station and SoftAP devices. Protected
WPA/TKIP, WPA2/TKIP/CCMP, WPA2-PSK-SHA256/CCMP, and WPA3-SAE/CCMP networks
use the framework's embedded authentication and four-way handshake, so existing
`rt_wlan_connect()` and `wifi join <ssid> <password>` callers work without a
userspace `wpa_supplicant`. For WPA3, the driver maps the vendor
`SM_EXTERNAL_AUTH_REQUIRED_IND/RSP` exchange to WLAN offload management-frame and
external-auth operations. Enterprise authentication still requires a complete
external supplicant. See the generic WLAN offload README for the current SAE H2E
and anti-clogging limitations.

Open, WEP40, and WEP104 station connections remain handled directly by the
firmware. The advertised key ciphers match the vendor Linux driver: WEP40,
WEP104, TKIP, CCMP-128, and BIP-CMAC-128. The firmware ABI defines additional
GCMP and 256-bit cipher numbers, but the Linux driver does not expose or install
those keys, so RT-Smart does not claim them without device-level validation.

The scan parser distinguishes WPA2/WPA3 transition networks, SHA-256 and FT
AKMs, WPA3 Enterprise, SAE extended-key, FILS, OWE, DPP, OSEN, WAPI, and CCKM
where their standard information elements identify them. Reporting a security
mode is separate from supporting `wifi join`: only the embedded Personal modes
above are handled without userspace credentials.

SoftAP uses the AIC firmware's APM beacon/channel service and the framework's
compact embedded authenticator. It supports open networks and WPA2-Personal
with CCMP without a userspace `hostapd`. The host builds authentication and
association responses, performs the WPA2 four-way handshake, installs per-client
and group keys, controls station authorization, and keeps the firmware station
table synchronized. WPA/WPA2-TKIP, WPA3-SAE, Enterprise/802.1X, WPS, FT, PMF,
VLAN assignment, and dynamic beacon reconfiguration are not implemented for
SoftAP.

With `AIC8800_WIFI_AUTO_START=y`, both `wlan0` (station) and `wlan0ap` (AP) are
initialized when the device attaches; the AP VIF consumes no airtime until it
is started. Existing shell commands provide the bare interface:

```text
wifi ap <ssid>                 # open SoftAP on the default/concurrent channel
wifi ap <ssid> <passphrase>    # WPA2-PSK/CCMP SoftAP
wifi ap <ssid> 2g              # open 2.4 GHz SoftAP on default channel 6
wifi ap <ssid> <passphrase> 5g # WPA2 SoftAP on default channel 149
wifi ap <ssid> 2g 1            # open 2.4 GHz SoftAP on channel 1
wifi ap <ssid> <passphrase> 5g 149  # WPA2 SoftAP on 5 GHz channel 149
wifi ap_stop
```

The explicit-band form accepts `2g`/`2.4`/`2.4g` and `5g`/`5.8`/`5.8g`.
When no band is supplied, a SoftAP sharing a transport with the active station
uses the station's band and primary channel; otherwise it defaults to 2.4 GHz
channel 6. An explicit band without a channel defaults to channel 6 for 2.4 GHz
and channel 149 for 5 GHz.
The selected channel must be enabled by the device and regulatory configuration.
Channels marked no-IR or DFS are rejected because the compact AP path does not
implement DFS channel-availability checks. The C API provides explicit selection
through `rt_wlan_start_ap_with_channel()`; `rt_wlan_start_ap()` applies the same
concurrent-channel selection as the implicit shell form. Stop an active AP before
selecting a different band, channel, SSID, or key.

On VHT-capable firmware, a 5 GHz compatibility-API SoftAP selects an 80 MHz
channel definition when the complete four-channel block is permitted. The
beacon and association response advertise one-stream VHT MCS 0-9, and client
VHT capabilities are forwarded to the firmware station table. A standalone
channel such as 165, or a block containing a restricted channel, remains 20 MHz.

Station-only, SoftAP-only, and concurrent station plus SoftAP modes are
advertised. The AIC firmware exposes one channel context, so concurrent mode
requires the station and SoftAP to use the same primary channel. Their channel
widths may differ, as Linux permits for compatible interfaces sharing one RF
channel. Starting an AP on another primary channel, or joining one while the AP
is running, returns `-RT_EBUSY` instead of silently disrupting the other
interface.

The driver negotiates VHT, HE, 5 GHz availability, and power-save support from
`MM_VERSION_CFM` before publishing the radio capabilities. `wifi powersave
on|off` maps to the firmware's dynamic power-save mode. The AIC WLAN offload
firmware's primary RF channel is selected by the connect operation (matching
the vendor Linux driver); the generic primary `set_channel` operation is
therefore a no-op for the current single-chain device and returns
`-RT_ENOSYS` for a different channel. Standalone channel changes while
associated remain rejected; AP start carries its complete 20/40/80 MHz channel
definition in `APM_START_REQ`, matching the Linux driver path.

Dual-band scans are issued as consecutive 2.4 GHz and 5 GHz firmware requests
under one WLAN offload request ID. Some AIC8800D80 firmware revisions return only
the final band when both channel sets are supplied in one `SCANU_START_REQ`.
RT-Thread receives `SCAN_DONE` only after both requests finish, so its scan
cache contains results from both bands.

The current Apache implementation does not advertise Linux-driver P2P, mesh,
TDLS, remain-on-channel, DFS/CAC, monitor-mode frame delivery, or multiple
channel contexts. Enterprise 802.1X still belongs in an external supplicant.
AIC8800DC/DW U02 non-H and H Wi-Fi ROM patch and DPD calibration are supported.
DC/DW U01, optional LOFT calibration, and the Bluetooth patch stage are not
yet ported.

The USB receive workers keep their URB queues alive across transient host
controller errors. Protocol, data-integrity, overflow, and endpoint-stall
completions are retried with bounded backoff; a stalled endpoint is cleared
first. After `AIC8800_WIFI_RX_RECOVERY_ERRORS` consecutive failures, the
transport marks the WLAN offload bus failed; the framework then restarts the bus,
firmware stack, and previously enabled WLAN interface. Recovery counters are
included in the endpoint shutdown log.

The SDIO receive worker distinguishes controller I/O failures from malformed
firmware aggregates. Malformed records are discarded without taking the radio
offline. Isolated controller errors are retried by the receive worker, and
recovery is requested only after `AIC8800_WIFI_SDIO_RX_RECOVERY_ERRORS`
consecutive failures. The worker stops receiving before reporting that failure
so only one recovery can run. Ethernet records are copied into preallocated
queues and processed by a second worker. The default 256-record queue is a
bounded embedded counterpart to the reference driver's 2000-entry dynamic RX
queue. Normal records use the
`AIC8800_WIFI_SDIO_RX_QUEUE_DEPTH` 4 KiB pool; larger A-MSDUs use the separate
`AIC8800_WIFI_SDIO_RX_LARGE_QUEUE_DEPTH` reserve. This keeps firmware command
responses synchronous, lets the bus worker return to CMD53 FIFO reads without
waiting for lwIP, and avoids sizing every queue entry for the firmware's
maximum 11454-byte receive A-MSDU. The data worker uses
`AIC8800_WIFI_SDIO_DATA_THREAD_BUDGET` to leave periodic scheduler windows for
the watchdog and other lower-priority services under sustained RX load. SDIO
record lengths are 16-bit and must not be limited to USB's 12-bit length.

Firmware loading and automatic WLAN startup run on a dedicated transport
attach workqueue. The USB hub or SDIO discovery thread only identifies the
device and queues that work, so firmware parsing, VFS reads, and WLAN
initialization do not consume the discovery thread's stack. The worker stack
and priority are configurable with
`AIC8800_WIFI_ATTACH_THREAD_STACK_SIZE` and
`AIC8800_WIFI_ATTACH_THREAD_PRIORITY`.

Wi-Fi data transmission uses an asynchronous USB request and an aligned DMA
buffer. Firmware control messages remain synchronous so command ordering and
error reporting are preserved. A reusable frame staging buffer also removes
per-packet heap allocation from the normal Wi-Fi transmit path. The default
pool contains two requests: one active and one ready.

Enable `AIC8800_WIFI_DEBUG_STATS` to collect the optional USB, network, and
receive-reorder diagnostics and export the `aic8800_stat` FinSH command. It
also enables transmit ICMP checksum validation and firmware rate-control
statistics used for transport tuning and fault diagnosis. With the option
disabled, stat-only fields and per-frame accounting are not compiled.

Transmit throughput is limited by the radio, not by this pool. Measured on an
AIC8800D40 over high-speed USB with a saturated UDP stream, raising
`AIC8800_WIFI_DATA_TX_URBS` from two to four left the fraction of submissions
that had to wait for a free request unchanged at 96%, while `aic8800_stat`
reported bursts of over a thousand frames submitted without ever blocking. The
frame rate matched the rate controller's own throughput estimate for the
negotiated MCS, so frames accumulate because the firmware drains them at the
air rate. Deepening the pool or the queue only adds buffering latency and
memory pressure; a 128-entry queue costs roughly 260 KB, which the USB host
controller then cannot use for its own transfers. The vendor Linux driver keeps
200 requests, but throttles at the network interface once free requests fall
below a quarter of the pool, so it is not queueing 200 frames either. Raise
either value only if `aic8800_stat` shows a low wait fraction together with a
saturated queue, which would mean the host really is the bottleneck.

`AIC8800_WIFI_TX_WAIT_MS` bounds the backpressure wait when every request is
busy. Because transmit is normally radio-limited, this wait is what paces the
sender: shortening it converts the stall into a dropped frame rather than
raising throughput. Ordinary data is posted through a bounded queue to a
dedicated transmit worker, matching the reference driver's asynchronous
submission policy. Firmware control traffic bypasses the queue. On DC/DW,
EAPOL and management traffic enters at the queue head so the worker applies
the mandatory USB aggregate envelope without adding ordinary-data latency;
unaggregated devices continue to submit it directly.
D80/D80X2 records remain unaggregated because their bundled runtime does not
accept the vendor DC/DW aggregate envelope. Transmit completion, byte,
queue-wait, burst, and error counters are printed when the bus stops.

The receive workers must run at a higher priority than the transmit worker.
They rearm the bulk IN requests, so if the transmit worker can preempt them a
saturated transmit path stops receive entirely. Keep
`AIC8800_WIFI_RX_THREAD_PRIORITY` ahead of
`AIC8800_WIFI_USB_TX_THREAD_PRIORITY`; `aic8800_stat` reports the receive
completion backlog high-water mark to confirm the workers are keeping up.

USB and SDIO receive data marked `flags_need_reord` by the firmware is reordered
per VIF, station, QoS TID, and 12-bit 802.11 sequence number before it reaches
lwIP.
The default window is 64 packets, backed by 128 records and the 50 ms update
timeout used by the AIC reference driver. Common records use inline storage;
large A-MSDUs allocate external storage only while queued. EAPOL and multicast
records bypass reordering as they do in the reference driver. USB receive
records are copied into the assembly buffer before the request is rearmed, so
command responses cannot race reuse of the DMA buffer.

SDIO TX waits once, before taking the transport mutex, for up to 1 ms for a
second queued frame. It then drains the remaining ready records without
blocking and combines them into the firmware's 32-frame CMD53 aggregate. Queue
admission applies up to `AIC8800_WIFI_TX_WAIT_MS` of producer backpressure when
the bounded pool is saturated instead of immediately dropping socket traffic.
Four records remain reserved for EAPOL and other high-priority traffic, while
firmware control traffic bypasses the data queue. The standard lwIP Ethernet TX
worker keeps this wait out of application threads. Queue reset is serialized
with producers so recovery and shutdown cannot strand TX pool records.

Pure IPv4 TCP acknowledgements are coalesced for at most 5 ms, with every tenth
new acknowledgement sent immediately. Window updates, duplicate ACKs, and the
next ACK after received TCP push data bypass coalescing. This matches the
throughput optimization enabled in the AIC reference USB and SDIO drivers.

The transport-start logs report USB speed/endpoint packet size or the actual
SDIO clock, bus width, and timing. AIC8800D80 uses the card's negotiated
50 MHz SDIO high-speed clock. This matches the AIC D80 v3 reference path,
where the optional clock and I/O-phase override block is disabled. The K230
SDIO1 host also advertises a 50 MHz maximum. Running this path at 100 MHz
without a tuned UHS mode causes SDHCI data-CRC errors. A 50 MHz four-bit SDIO
bus has a 200 Mbit/s raw ceiling before CMD53, firmware, 802.11, and IP
overhead.

`wifi country <code>` can update the AIC channel/power table while the radio is
started but idle. A country change is rejected during scan, association, or an
active connection. If the firmware rejects the replacement channel table, the
driver restores the previous country and channel metadata.

With the USB transport, `AIC8800_WIFI_BLE=y` exposes the standard Bluetooth USB interface
(`e0/01/01`) is registered by the common HCI framework as the next available
`/dev/hciX` device. Reads and writes use H:4 framing. HCI commands use USB
control transfers, events use interrupt IN, and ACL traffic uses bulk IN/OUT.
This supports BLE host stacks; SCO audio is not implemented. The Bluetooth USB
matcher binds `a69c:8801`, `a69c:88dc`,
`a69c:88dd`, `a69c:8d41`, `a69c:8d81`, `368b:8d91`, and `368b:8d99`.
AIC8800DC requires its Bluetooth patch stage before the HCI interface is usable.

## Licensing

The RT-Smart source files are licensed under Apache-2.0; see
`LICENSE.Apache-2.0`.

The firmware binaries have separate, currently undocumented redistribution
terms. Confirm the firmware grant before publishing images containing them.
