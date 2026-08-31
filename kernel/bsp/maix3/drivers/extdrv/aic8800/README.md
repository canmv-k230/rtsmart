# AIC8800 USB/SDIO Wi-Fi and USB Bluetooth driver

This directory contains the Apache-2.0 RT-Smart AIC8800 source implementation:

- an RT-Thread WLAN offload Wi-Fi driver;
- a USB boot-ROM firmware loader for AIC8800, AIC8800D80/D40, and
  AIC8800D80X2 devices;
- an SDIO transport and boot-ROM firmware loader for AIC8801, AIC8800D80,
  and AIC8800DC/DW/DL devices;
- a USB Bluetooth HCI transport for BLE host stacks.

`SConscript` builds the top-level `aic8800_*.c` files and the fixed-width
protocol declarations in `port/aic8800_protocol.h`. All source files in this
component are licensed under Apache-2.0. The firmware files are maintained in
a separate repository checked out at `firmware/`.

## Firmware loading

Enable `RT_USING_AIC8800_WIFI`, then
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
    |-- aic8800D80/
    `-- aic8800DC/
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
|-- aic8800D80X2/
|   |-- fmacfw_8800d80x2.bin
|   |-- fw_adid_8800d80x2_u03.bin
|   |-- fw_patch_8800d80x2_u03.bin
|   `-- fw_patch_table_8800d80x2_u03.bin
`-- aic8800DC/
    |-- fmacfw_patch_8800dc_u02.bin
    |-- fmacfw_patch_8800dc_h_u02.bin
    |-- fmacfw_calib_8800dc_u02.bin
    |-- fmacfw_calib_8800dc_h_u02.bin
    |-- fmacfw_patch_tbl_8800dc_u02.bin
    `-- fmacfw_patch_tbl_8800dc_h_u02.bin
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
    |-- aic8800D80/
    `-- aic8800DC/
```

The firmware repository is distributed separately because its vendor licensing
terms are independent of this source code. Review `firmware/README.md` and the
applicable vendor terms before using or redistributing the files, including in
binary images.

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
firmware capabilities can distinguish it from an 80 MHz part at startup. The
vendor Linux driver has the same blind spot and answers it with a `use_80=0`
module parameter; `AIC8800_WIFI_USB_LIMIT_40MHZ` is its equivalent here, and
setting it for a known D40 board is still the way to get the first association
right.

This is confirmed hardware behaviour, not a precaution. An AIC8800D40L module
enumerates as `a69c:8d81`, loads the `aic8800D80/` firmware, and reports an
80 MHz modem, while the AIC8800D40 datasheet specifies "data rates up to
286.8 Mbps with 20/40 MHz bandwidth" - 286.8 Mbps being HE40, one spatial
stream, MCS11 at 0.8 us GI. Such a module never negotiates wider than 40 MHz,
including against a dedicated 80 MHz AP at -51 dBm. Loading the D80 firmware is
correct: AIC ships one image for the family, and the vendor driver likewise maps
its D40N/D40LN/D40WN product IDs onto `PRODUCT_ID_AIC8800D80N`. Only the
bandwidth inference is wrong.

The driver corrects itself instead, so one image serves both parts. The
firmware chooses the operating width from the AP's HT/VHT operation elements
and its own modem, and reports the result in `SM_CONNECT_IND`. An association
that settles for 40 MHz where the AP offered 80 MHz or more indicates the
`MM_VERSION` width is wrong, because a station that could use 80 MHz would have
taken it. After `AIC_BANDWIDTH_80_FAILURES` such associations the driver stops
advertising 80 MHz and republishes `ME_CONFIG` before the next association.

The threshold exists because the costs are asymmetric: capping a real
AIC8800D80 at 40 MHz throws away half of its 600.4 Mbps, while letting an
AIC8800D40 advertise a width it never uses costs little. So the evidence has to
repeat, and any association that does reach 80 MHz proves the modem outright
and clears the count. The correction is logged at warning level and lasts until
the device is detached; `AIC8800_WIFI_USB_LIMIT_40MHZ` remains available for
integrators who know their board and want the very first association right.

This matters beyond the rate controller. The AP builds its **downlink** rate
table from what the station advertises, so claiming a width the receiver cannot
demodulate costs inbound frames, not just outbound rate. Uplink is unaffected
because the station picks its own width, which makes the failure look like a
transmit-only success: bulk sending runs at full rate while anything waiting on
a reply - EAPOL, DHCP, a TCP handshake - stalls for seconds and then arrives in
a burst.

The AP's operating width is decoded from both VHT Operation encodings. Besides
the deprecated width codes 2 and 3, a 160 MHz or 80+80 MHz AP may keep the
width code at 1 and signal the width through a non-zero second centre-frequency
segment; reading the width code alone reports such an AP as 80 MHz.

Supported Wi-Fi SDIO IDs are:

| Family | Manufacturer | Function 1 product |
| --- | --- | --- |
| AIC8801 | `5449` | `0145` |
| AIC8800D80 | `c8a1` | `0082` |
| AIC8800DC/DW/DL | `c8a1` | `c08d` (`c18d` on function 2) |

The vendor SDIO source exposes DC and DW product enums and groups its RF/test
assets as `AIC8800DCDWDL`; there is no separate DL product ID or DL firmware
filename. DL modules therefore use the revision-selected files in
`aic8800DC/`.

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

AIC8800DC/DW/DL WLAN startup uses the vendor v2 TX-power ABI, revision-specific
2.4 GHz TX gain tables, 20/40 MHz RX gain tables, and DC calibration mask. The
driver reads the chip and sub-revision before starting the firmware stack so
the matching request formats are selected correctly. AIC8800DC/DW/DL U02 non-H
and H silicon receive their matching vendor system configuration, Wi-Fi ROM
patch, LDPC/AGC/TX-gain tables, and patch descriptor before the patched ROM is
started. USB and SDIO run the matching DPD calibration helper for both non-H
and H silicon, following the vendor Linux configuration, and treat calibration
failure as fatal. Other DC/DW ROM patch revisions are rejected explicitly until
their matching host tables are ported.

Configuration defaults changed with the DC/DW transport update: firmware
station power save changed from enabled to disabled because sustained traffic
can stop the DC/DW transmit queue and trigger the firmware AC1 assertion; USB
data RX URBs changed from 5 to 4 for descriptor-DMA builds and from 5 to 20 for
non-DDMA builds; USB data TX URBs changed from 2 to 64 to seed firmware air
aggregation without enabling the vendor-disabled USB aggregate wire format; the
previously implicit USB TX aggregation changed to disabled; and the SDIO TX
aggregation wait changed from 1 ms to 0 ms. Board defconfigs that require the
old power or throughput profile must set these options explicitly.

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

Downlink frames for a sleeping SoftAP client remain in the bounded transport
queue. `ME_TRAFFIC_IND` updates the firmware TIM state, `MM_PS_CHANGE_IND`
tracks whether the client is asleep, and `MM_TRAFFIC_REQ_IND` releases the
bounded number of frames requested for a PS-Poll or U-APSD service period. QoS
peers retain their 802.1d TID/access category, including ACM downgrade; non-QoS
and group traffic use TID `0xff` as required by the firmware ABI.

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
160 MHz and 80+80 MHz SoftAP are rejected: `APM_START_REQ` can carry them, but
the beacon builder only emits a VHT operation element for 20/40/80 MHz, so
starting one would advertise a narrower channel than the AP actually occupies.
The advertised band maximum is 80 MHz, so only a direct
`rt_wlan_start_ap_with_channel()` can ask for them.

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
cache contains results from both bands. The confirmation can arrive on the
message endpoint before the last result indication on the data endpoint, so
completion is deferred until the reported result count arrives or a bounded
200 ms drain expires.

The current Apache implementation does not advertise Linux-driver P2P, mesh,
TDLS, remain-on-channel, DFS/CAC, monitor-mode frame delivery, or multiple
channel contexts. Enterprise 802.1X still belongs in an external supplicant.
AIC8800DC/DW/DL U02 non-H and H Wi-Fi ROM patch and DPD calibration are
supported.
DC/DW U01, optional LOFT calibration, and the Bluetooth patch stage are not
yet ported.

The USB receive workers keep their URB queues alive across transient host
controller errors. Protocol, data-integrity, overflow, and endpoint-stall
completions are retried with bounded backoff; a stalled endpoint is cleared
first. After `AIC8800_WIFI_RX_RECOVERY_ERRORS` consecutive failures, the
transport marks the WLAN offload bus failed; the framework then restarts the bus,
firmware stack, and previously enabled WLAN interface. Recovery counters are
included in the endpoint shutdown log.

A short run of failed bulk IN transfers is normal on this device rather than a
fault. During association the firmware retunes and stops answering IN tokens on
both IN endpoints together, typically for a few milliseconds of transaction
errors followed by tens of milliseconds of silence. The retry delay therefore
backs off on the number of re-submissions as well as the consecutive error
count, so a stall costs a handful of transactions instead of one per
millisecond, and runs shorter than `AIC8800_USB_RX_ERRORS_EXPECTED` are logged
at debug level with their matching recovery notice.

That demotion applies only after the endpoint has completed at least one
transfer, tracked by `ever_completed` in the receive worker. Before the first
completion there is no association in progress to explain the failure, so the
error is reported at warning level however short the run is. The distinction
matters because `LOG_D` is compiled out at the default `ULOG_OUTPUT_LVL_I`:
without it, an endpoint that never delivers anything produces a completely
silent log, and an attach failure shows up only as an unexplained firmware
command timeout several seconds later.

The vendor Linux driver
does not retry a failed bulk IN URB at all - it returns the buffer to its pool
and relies on the next successful completion to re-arm the queue - which is
quieter still but has no way out if every queued URB fails. Retrying keeps that
exit while the backoff and log level keep the ordinary case quiet.

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
pool contains 64 requests. This is the largest pipeline supported by the
driver configuration and leaves half of the RT-Smart DWC2 controller's fixed
128-request pool for receive, Bluetooth, and other USB devices. The vendor
driver uses 200 ordinary TX URBs when USB TX aggregation is disabled; the
deeper RT-Smart pipeline similarly lets a cold firmware receive enough records
to establish air aggregation without requiring a preliminary traffic run.

Enable `AIC8800_WIFI_DEBUG_STATS` to collect the optional USB, network, and
receive-reorder diagnostics and export the read-only `aic8800_stat` command.
The command reports common WLAN and firmware rate-control state followed by
USB and SDIO transport counters when those transports are configured. The
option also enables transmit ICMP checksum validation. It defaults off so
production builds avoid the stat-only fields and per-frame accounting.

`AIC8800_WIFI_TX_WAIT_MS` bounds direct USB request backpressure on firmware
families that do not use the host queue. Queued USB and SDIO admission is
non-blocking so a saturated normal-data producer cannot hold the shared bus
lock ahead of management or EAPOL traffic. Reserved pool entries and urgent
queue insertion keep those records available. Ordinary data is posted through
a bounded queue to a dedicated transmit worker, matching the reference
driver's asynchronous submission policy. Firmware control traffic bypasses
the queue. On DC/DW/DL,
EAPOL and management traffic enters at the queue head so the worker applies
the mandatory USB aggregate envelope without adding ordinary-data latency;
unaggregated devices continue to submit it directly.
D80/D80X2 records remain unaggregated because their bundled runtime does not
accept the vendor DC/DW aggregate envelope. Transmit completion, byte,
queue-wait, burst, and error counters are printed when the bus stops if
`AIC8800_WIFI_DEBUG_STATS` is enabled.

The receive workers must run at a higher priority than the transmit worker.
They rearm the bulk IN requests, so if the transmit worker can preempt them a
saturated transmit path stops receive entirely. Keep
`AIC8800_WIFI_RX_THREAD_PRIORITY` ahead of
`AIC8800_WIFI_USB_TX_THREAD_PRIORITY`. With debug statistics enabled, the
endpoint shutdown log includes the receive completion backlog high-water mark.

USB and SDIO receive data marked `flags_need_reord` by the firmware is reordered
per VIF, station, QoS TID, and 12-bit 802.11 sequence number before it reaches
lwIP.
The default window is 64 packets, backed by 128 records and the 50 ms update
timeout used by the AIC reference driver. Common records use inline storage;
large A-MSDUs allocate external storage only while queued. EAPOL and multicast
records bypass reordering as they do in the reference driver. USB receive
records are copied into the assembly buffer before the request is rearmed, so
command responses cannot race reuse of the DMA buffer.

SDIO TX drains ready records without blocking and combines them into the
firmware's 32-frame CMD53 aggregate. Queue admission never waits while holding
the shared bus lock. Four records remain reserved for EAPOL and other
high-priority traffic, while
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

The country table gates channel validity and the maximum power reported to the
host stack, but by default it does not cap the power programmed into the
firmware. The firmware transmits at the minimum of the per-channel value and
the per-rate userconfig targets (20/18/16 dBm), and the vendor Linux driver
fills the per-channel field with a flat 30 dBm so the targets alone decide.
Set
`AIC8800_WIFI_COUNTRY_TX_POWER_LIMIT=y` to enforce the country value as a hard
firmware cap.

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

The `firmware/` directory is a separate repository and is not covered by this
component's Apache-2.0 license. See `firmware/README.md` and the applicable
vendor terms before using or redistributing those files.
