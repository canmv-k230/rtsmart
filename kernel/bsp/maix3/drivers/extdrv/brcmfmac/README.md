# RT-Smart brcmfmac SDIO driver

This component is an RT-Smart-native port of the Linux 6.6.36 `brcmfmac`
FullMAC driver from Canaan's K230 Linux SDK commit
`9ea28c5521c20b3f1e97af7d69f58c8aeafe84e5`. It does not build Linux kernel
objects and does not depend on `cfg80211`, `net_device`, or `sk_buff`.

The port keeps the Linux driver boundaries:

- `brcmfmac_sdio.c` implements the SDIO backplane, firmware download, and
  SDPCM framing.
- `brcmfmac_proto.c` implements the BCDC command/data protocol and firmware
  event decoding.
- `brcmfmac_wifi.c` translates Linux brcmfmac FullMAC behavior to
  `rt_wlan_offload_ops` and reports decoded events to RT-Thread.

The existing `cyw43xx` directory is not compiled or linked by this component.
It remains available as a board-specific reference. The packaged BCM43430A1
firmware and default K016-CW43 NVRAM come from the K230 Linux SDK board overlay;
`nvram_ap6212.txt` is an alternate calibration, not an SDIO-ID match.

SDIO identities and firmware filenames live in one mapping table. The first
packaged chip is `02d0:a9a6` (BCM43430A1); adding a NIC consists
of adding its Linux firmware/NVRAM files and a mapping entry. ChipCommon EROM
discovery determines cores and RAM layout at runtime, so the transport is not
hard-coded to BCM43430 addresses. Firmware files are installed under
`/bin/firmware/brcmfmac` by default.

After SDIO discovery returns, a dedicated initialization worker registers the
station and AP devices with RT-Thread WLAN management, following the AIC8800
auto-start path. The AP interface remains idle until it is started. Deferring
this step avoids waiting for firmware responses while card discovery holds the
MMC/SDIO host lock.

The SDIO transport follows Linux brcmfmac's function-specific block sizing:
function 1 uses 64-byte blocks, while function 2 normally uses 512-byte blocks
(256 or 128 bytes for device IDs that require it). Receive buffers are
cache-line aligned, SDPCM next-frame lengths provide read-ahead, and SDIO core
revision 12 or newer configures firmware-to-host RX glom alignment. This avoids
per-packet DMA bounce copies and reduces CMD53 traffic under load.

Ethernet transmit is decoupled from the lwIP caller through a bounded packet
pool and worker queue. When firmware accepts the Linux `bus:rxglom` iovar, the
worker emits the corresponding extended SDPCM headers and combines up to
`BRCMFMAC_TX_GLOM_FRAMES` packets into one CMD53 transfer. The default aggregate
limit is Linux brcmfmac's 32 frames, and the 256-packet RT-Smart queue is large
enough to absorb short firmware-credit stalls without the multi-megabyte cost
of Linux's 2048-packet queue. Control commands
remain synchronous and reserve firmware credits so scan/connect/AP operations
cannot be starved by a saturated data queue. The direct lwIP transmit path uses a bounded wait when the transport queue is
full, providing the backpressure that Linux gets from its netdev queue. If that
wait expires, the packet is consumed as a link-layer drop: returning a temporary
error makes RT-Thread UDP senders retry the same frame in a tight loop and starve
the TX worker.
Flow-control windows are consumed from normal frames and the RX glom outer
superframe; glom subframes are validated without replacing that window, as in
Linux. The transport registers both Broadcom function interrupt bits and
wakes the receive worker from either one. This adapts Linux brcmfmac's
function 1 handler plus function 2 dummy-handler topology to RT-Thread's
per-function interrupt dispatcher. The 10 ms watchdog follows Linux's default
interrupt mode and performs software timeout handling without idle SDIO status
reads. TX credit waits explicitly request a status scan and are woken by
credit-window updates in received SDPCM headers; the transport does not inject
`SMB_DEV_INT` mailbox interrupts, matching Linux brcmfmac.
`BRCMFMAC_TX_CREDIT_TIMEOUT_MS` is a diagnostic warning interval; an exhausted
data window remains queued, as it does in Linux, and does not reset firmware.
Command timeouts and real SDIO failures still fail the operation or invoke
transport recovery. Function 2 is read only
after firmware reports `FRAME_IND`, matching Linux brcmfmac and avoiding FIFO
underflow from speculative reads. The transport also handles
the SDPCM priority mask, global mailbox flow control, host-mailbox
acknowledgement, and F2 receive resynchronization with
firmware retransmission for failed control/event reads. Failed function-2 writes run the Linux abort/terminate sequence. Control
frames are retried with the same SDPCM sequence number. Failed data aggregates
are not replayed because firmware may have accepted any number of complete
subframes before the host reports the error. Unlike Linux's success-only
sequence accounting, this port treats an exhausted write recovery as an
ambiguous transport fault and restarts firmware, so host and firmware cannot
remain permanently one sequence apart. The card remains at its negotiated
SDIO high-speed rate; transfer failures are recovered at the function and host
controller layers instead of changing clock and timing state. Invalid firmware
credit windows use Linux's bounded two-credit recovery and rate-limited logging.
The stop log reports transfers, frames, aggregates, queue high-water mark,
drops, credit stalls, retries, and errors for diagnosis. A successfully started
AP is cached in the driver and replayed after a real transport recovery so the
AP-start event restores the WLAN management, DHCP, and NAT state. AP startup
selects `apsta=1` before creating the virtual AP BSS, even when no station is
active yet. BCM43430 firmware cannot reliably convert a running primary AP BSS
back into the station role, so keeping BSS 0 as the station interface makes
AP-first and STA-first concurrent operation use the same firmware state. Before
a concurrent station join, the virtual AP BSS is temporarily stopped so BSS 0
can authenticate without competing for the single radio channel. After the join,
the AP resumes on the associated station channel; a failed join restores its old
channel. This matches bcmdhd's single-channel APSTA policy for BCM43430. Local class-3
deauthentication responses for clients that have
not associated are not reported as station departures, avoiding false
management events and log storms.

NVRAM is hardware data, not a file that can be generated from the Broadcom
chip ID. Use the module or board vendor's calibrated NVRAM for each distinct
module and RF design, and select it in the firmware mapping table. Units with
the same module and board layout can share those calibration values, but every
unit still needs a unique MAC address from module OTP/CIS or manufacturing
provisioning. When firmware exposes the known NVRAM template address, the driver
uses the Broadcom CIS MAC tuple first, then a stable locally administered address
derived from the K230 SoC UID; a boot-local fallback is used only if neither is
available. The BCM43430A1 mapping defaults to `nvram.txt`, matching the
firmware/NVRAM pair supplied by the K230 Linux SDK and validated on CanMV.
The default country revision is 0, matching Linux brcmfmac BCM43430 ISO 3166
fallback and the K230 SDK bcmdhd configuration.
`nvram_ap6212.txt` is retained for boards whose module and RF layout match the
AP6212A V1.0.2 calibration. Select the correct file with
`BRCMFMAC_BCM43430A1_NVRAM`; neither file is interchangeable with calibration
for an unrelated module or board.

Source code derived from Linux brcmfmac is ISC licensed. RT-Smart integration
code in this component is also distributed under the ISC license. Firmware is
redistributed under the vendor terms included as
`firmware/LICENCE.broadcom_bcm43xx` and is not covered by the ISC license.
