# ESP-Hosted-FG/NG WLAN offload driver

This driver connects ESP-Hosted-FG or ESP-Hosted-NG firmware to RT-Smart's
`rt_wlan_offload_radio` interface. It is separate from the older
`esp_hosted_mcu` RPC driver.

The driver targets WLAN offload API version 3, including dynamically assigned
control-device names, explicit channel-width metadata, current security-mode
reporting, and the NG external-authenticator contract.

The design has three independent layers:

- `esp_hosted_wifi.c` registers one WLAN offload radio and owns lifecycle,
  capability, command, and firmware state.
- `esp_hosted_fg.c` and `esp_hosted_ng.c` implement their respective wire
  protocols and translate them to WLAN offload operations and events.
- `esp_hosted_transport_spi.c` and `esp_hosted_transport_sdio.c` implement
  `rt_wlan_offload_bus`; protocol code has no K230 bus dependencies.

FG performs connection and WPA key management in ESP firmware. When enabled,
its `/dev/wlanctlN` control device supports libwlan_offload diagnostics and
offloaded operations but does not advertise an external supplicant. NG exposes
authentication, association, EAPOL, key, management-frame, and AP-station
operations on the same control ABI. It supports the framework's
embedded WPA/WPA2 client and open/WPA2 SoftAP paths, while retaining the
userspace supplicant interface for broader security support.

FG can emit station-disconnect events while it applies new credentials and
internally retries association. While a host connect request is pending, those
events are treated as retry progress rather than a terminal failure. A
successful station-connected event completes the request; otherwise a
driver-side deadline completes it with a timeout before the RT-Thread WLAN
management deadline expires.

## Configuration

Enable `RT_USING_ESP_HOSTED_WIFI`, then select exactly one firmware
personality and one transport. SPI requires handshake and data-ready GPIOs and
uses fixed 1600-byte full-duplex transfers. SDIO supports the ESP device IDs
`6666:2222`, `6666:3333`, `0092:6666`, and `0092:7777`; configure the one used
by the target firmware because the local RT-Thread SDIO matcher accepts one ID
per driver registration.

GPIO polarity, SPI mode, checksum selection, and the chosen ESP firmware must
match. The driver refuses to start if the boot capability event does not
advertise the selected transport.

`ESP_HOSTED_WIFI_AUTO_START` is enabled by default. Transport registration
schedules a dedicated probe worker, which enables the station interface,
attaches lwIP, and creates `w0` without requiring `wlan_offload.elf probe`. The
worker waits at most `ESP_HOSTED_WIFI_BOOT_TIMEOUT_MS` for the firmware boot
event, so a missing ESP cannot delay RT-Smart startup. A failed automatic probe
leaves `/dev/wlanctlN` registered so the link can still be retried manually.

The K230 SPI path uses 32-bit controller frames for each fixed 1600-byte
full-duplex exchange. Only the first protocol frame is parsed because the ESP
does not initialize the remaining SPI padding. Handshake and data-ready GPIOs
wake the worker. Malformed and empty receive cycles are rate-limited so an
asserted data-ready signal cannot cause an unbounded dummy-transfer loop.

Enabling an interface through `/dev/wlanctlN` also uses RT-Thread WLAN
management, so manual and automatic startup share the same `w0`, lwIP,
`ifconfig`, and `wifi` shell state.

## Source and license provenance

Wire layouts and generated FG protobuf files come from ESP-Hosted commit
`5acd9ba0eaf186cc340b8dc2e7a12993a4162b93`. Those inputs are offered under
`GPL-2.0-only OR Apache-2.0`; this port uses the Apache-2.0 option. The bundled
protobuf-c runtime is BSD-2-Clause. The K230 and RT-Thread implementation in
this directory is original Apache-2.0 code. No GPL-only Linux host-driver
implementation is copied into this component.
