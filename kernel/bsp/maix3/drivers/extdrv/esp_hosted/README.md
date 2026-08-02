# ESP-Hosted-MCU RT-Smart port

This driver exposes an ESP-Hosted-MCU coprocessor through the RT-Thread WLAN
framework. It registers the station and SoftAP devices and passes their Ethernet
frames to lwIP. Optionally it also registers the coprocessor's Bluetooth
controller as an RT-Smart H:4 character device.

## Port layout

The component follows the upstream ESP-Hosted-MCU `common/` and `host/`
split. Only the RT-Smart wrapper and K230 transport are platform-specific:

- `common/proto` contains the upstream `.proto` file and its unmodified
  protobuf-c generated sources.
- `common/protobuf-c` contains the upstream protobuf-c runtime.
- `host/drivers/rpc` follows the upstream RPC `core/`, `slaveif/`, and `wrap/`
  layout. The compiled files use the RT-Smart compatibility interfaces instead
  of ESP-IDF host types, `g_h`, and the upstream virtual serial driver.
- `host/port/rtsmart/esp_hosted_rpc_compat.c` is the RPC platform boundary. It
  supplies allocation and serial-interface submission using RT-Thread and the
  selected K230 transport backend.
- `host/port/rtsmart/esp_hosted.c` maps typed ESP-Hosted operations and events to RT-Thread WLAN
  and Bluetooth lifecycle APIs. It contains no protobuf field layouts or RPC
  message IDs.
- `host/drivers/rpc/core/rpc_core.c` uses the generated `Rpc` envelope
  directly and owns UID correlation, serial TLV fragmentation, synchronous
  calls, and event dispatch.
- `host/drivers/rpc/wrap/rpc_wrap.c` implements the upstream RPC wrapper
  API with generated protobuf-c messages. `esp_hosted_rpc_api.c` contains the
  smaller RT-Thread WLAN/Bluetooth adapter API.
- `host/drivers/rpc/wrap/esp_hosted_rpc_api.c` builds generated schema messages for the Wi-Fi and
  Bluetooth commands used by the RT-Thread adapter.
- `host/drivers/rpc/slaveif/rpc_slave_if.c` provides generated
  protobuf-c message access to
  every request, response, and event that has a payload definition in the
  upstream schema.
- `host/port/rtsmart/esp_hosted_control.c` owns the private
  capability/configuration TLV protocol.
- `host/drivers/transport/esp_hosted_transport.c` owns common frame encoding, checksum validation,
  flow control, priority TX queues, and RX delivery. RPC/control traffic is
  serviced first, Bluetooth HCI second, and WLAN data last. WLAN flow control
  does not block HCI traffic.
- `host/port/rtsmart/esp_hosted_hci.c` adapts ESP-Hosted interface 4 to the generic RT-Thread
  Bluetooth HCI device API.
- `host/drivers/transport/esp_hosted_transport_spi.c` owns K230 SPI bus, FPIOA, GPIO IRQ, and reset
  setup shared by both SPI protocols.
- `host/drivers/transport/esp_hosted_transport_spi_fd.c` implements the fixed 1600-byte full-duplex
  protocol.
- `host/drivers/transport/esp_hosted_transport_spi_hd.c` implements the ESP SPI half-duplex register
  and DMA protocol.

An SDIO port should implement another `eh_transport_ops` backend and leave the
adapter and RPC layers unchanged. SDIO is not selectable until such a backend
exists.

## Kconfig

Enable `RT_USING_ESP_HOSTED`, then select one transport:

- `ESP_HOSTED_TRANSPORT_SPI_FD`: standard full-duplex SPI with MOSI, MISO,
  handshake, and data-ready signals. SPI mode 0 is not supported by the slave.
- `ESP_HOSTED_TRANSPORT_SPI_HD`: half-duplex ESP SPI-HD with data-ready and
  either two or four bidirectional data lines.

SPI-HD 1-line mode is intentionally not supported. K230 uses the hardware QSPI
controller for SPI-HD, so select `ESP_HOSTED_SPI_HD_WIDTH_2` or
`ESP_HOSTED_SPI_HD_WIDTH_4`. A 4-line host starts in 2-line mode and changes to
4-line mode only after the coprocessor capability packet advertises 4-line
support. A 2-line host can communicate with a coprocessor configured for either
2 or 4 lines. Select SPI Mode 0, 1, 2, or 3 to match the coprocessor; Mode 0 is
available only for SPI-HD.

For 4-line mode, set `ESP_HOSTED_SPI_D2_PIN` and
`ESP_HOSTED_SPI_D3_PIN` in addition to D0 and D1. Full-duplex SPI requires both
handshake and data-ready GPIOs. SPI-HD has no handshake signal and uses only
data-ready; SPI-HD can optionally disable data-ready and poll the slave status
register instead. When data-ready is enabled, select Active High or Active Low
to match the ESP-Hosted-MCU `ESP_SPI_HD_DATAREADY_GPIO_CONFIG` setting. The
full-duplex handshake GPIO has a separate Active High or Active Low choice.

Pins which support `-1` use it as follows:

- SPI signal `-1` keeps the board's existing FPIOA assignment.
- CS `-1` selects the SPI controller's hardware CS.
- SPI-HD data-ready `-1` enables transport polling. Full-duplex handshake and
  data-ready GPIOs cannot be disabled.
- Reset `-1` means the host cannot reset the coprocessor; both processors must
  then be reset together.

`ESP_HOSTED_RESET_PULSE_MS` controls how long the reset input is asserted.
For SPI-HD, `ESP_HOSTED_SPI_HD_RESET_SETTLE_MS` prevents stale shared registers
from the previous session being accepted while an ESP software restart is still
in progress. It does not replace the ready-register or RPC-event checks.
During startup, the SPI-HD worker also verifies the ready and control registers and
automatically reopens the data path if a delayed ESP restart clears them.
After reset, full-duplex waits for the handshake GPIO to cycle inactive and
then active. SPI-HD waits for the slave-ready register to read `0xee`, which is
the prerequisite for opening its data path. The RPC layer is enabled only after
the coprocessor's fresh `Event_ESPInit` is also received; this event distinguishes
a new coprocessor session from stale SPI-HD register state. These checks run in
the ESP-Hosted worker thread and therefore do not delay the rest of RT-Smart
initialization.

The SPI mode, checksum setting, bus width, signal polarity, and physical wiring
must match the ESP-Hosted-MCU slave configuration.

## Bluetooth architecture

Enable `ESP_HOSTED_BLE` to select `RT_USING_BT_HCI` and register `hci0` (or the
name selected by `ESP_HOSTED_BT_HCI_DEVICE_NAME`). The device is a non-blocking
H:4 byte stream: writes contain one complete H:4 command/ACL packet and reads
return controller event/ACL bytes. The driver validates packet boundaries and
drops a complete RX packet when the configured ring buffer has insufficient
space, so a truncated HCI packet is never exposed to the host stack.

RT-Smart's HCI layer is deliberately not a new GAP/GATT implementation. It is a
controller abstraction which can also be used by future USB, UART, or SDIO
controller drivers. CanMV can enable `ENABLE_BLUETOOTH` to run MicroPython's
existing NimBLE host stack over `/dev/hci0`; NimBLE supplies GAP, GATT, L2CAP,
SMP, central, and peripheral roles.

Opening the HCI device sends ESP-Hosted `FeatureControl` RPCs to initialize and
enable the remote Bluetooth controller. Closing the device disables and
deinitializes it without releasing controller memory, so it can be opened
again. This lifecycle is required by ESP-Hosted-MCU 2.5.2 and later, where the
Bluetooth controller no longer starts automatically.

ESP-Hosted has asymmetric HCI framing. Host-to-coprocessor packets place the
H:4 type in transport header byte 11 and exclude it from the payload. Packets
from the coprocessor contain the H:4 type as the first payload byte. Keep this
conversion in the ESP-Hosted adapter rather than exposing it to generic HCI
consumers.

## Complete RPC access

`esp_hosted_rpc.h` is the public generic protocol API. RPC IDs come directly
from the generated `RpcId` enum, so the RT-Smart port does not maintain a
second numeric ID list. The schema-specific services used by the RT-Thread
adapter are isolated in the RT-Smart RPC port directory. The high-level
`esp_wifi_remote_*`, coprocessor, MAC, Bluetooth, heartbeat, and OTA wrappers
retain the upstream API names and forward through the same generated schema.

For complete typed access, include `esp_hosted_rpc_schema.h`. It includes the
generated protobuf-c types from the upstream `esp_hosted_rpc.proto` and supports
all 112 request/response pairs and all 21 events that have actual message
definitions. Use `rt_esp_hosted_rpc_call_message()` with the generated request
type and cast the returned message to the corresponding generated response
type. The descriptor lookup APIs can be used for runtime discovery.

The `RpcId` enum also reserves 26 request IDs for ESP-IDF operations which have
no request or response payload in the current upstream `Rpc` oneof. Those IDs
cannot be serialized by either the official host or this port; descriptor
lookup returns `NULL` and a message call returns `-RT_ENOSYS` instead of sending
an invalid request.

The generated `esp_hosted_rpc.pb-c.[ch]` and bundled protobuf-c runtime come
from the sibling ESP-Hosted-MCU checkout under `common/proto` and
`common/protobuf-c`. They should be refreshed whenever that schema changes.

For example, reading maximum transmit power is:

```c
RpcReqWifiGetMaxTxPower request = RPC__REQ__WIFI_GET_MAX_TX_POWER__INIT;
ProtobufCMessage *message;

if (rt_esp_hosted_rpc_call_message(
        RPC_ID__Req_WifiGetMaxTxPower, &request.base,
        &message, 0) == RT_EOK)
{
    RpcRespWifiGetMaxTxPower *response =
        (RpcRespWifiGetMaxTxPower *)message;

    if (response->resp == 0)
    {
        /* response->power contains the result. */
    }
    rt_esp_hosted_rpc_free_message(message);
}
```

Serialized access remains available for callers that already own encoded
inner messages. Other ESP-Hosted facilities, including OTA, enterprise authentication,
DPP, TWT, GPIO expansion, memory monitoring, and custom RPC, use these generic
entry points:

- `rt_esp_hosted_rpc_call()` returns the serialized response submessage.
- `rt_esp_hosted_rpc_call_status()` handles responses containing only the
  standard `resp` status member.
- `rt_esp_hosted_rpc_set_event_callback()` observes all RPC events on a
  dedicated thread, after internal WLAN event processing.

Requests and responses passed to the generic API are the inner protobuf
submessages. The API constructs and validates the outer `Rpc` envelope, UID,
response ID, transport TLVs, serialization, and timeout through protobuf-c.
Prefer the typed message API for new code because it verifies the generated
request and response descriptors before sending.

RPC availability still depends on the coprocessor chip, ESP-IDF version, and
ESP-Hosted-MCU build options. An ID present in the shared schema can therefore
return the coprocessor's unsupported-message status.

## SPI-HD protocol notes

SPI-HD uses an 8-bit one-line command, an 8-bit address on two or four lines,
and eight dummy clocks on every command. Payload data uses a dual/quad data
phase. Commands use the protocol masks `0x50` for dual SPI and `0xa0` for
quad SPI. Register access, DMA completion commands, cumulative byte counters,
and flow-control interrupt bits follow ESP-Hosted-MCU 2.12.x.

The transport worker polls the ready register with a delay until a coprocessor
is present, so enabling the driver without connected hardware does not block
the RT-Smart initialization path.

For throughput testing, 5 MHz is an initial validation frequency rather than a
performance setting. Quad SPI at 5 MHz has a 20 Mbit/s raw wire-rate ceiling
before command, register, transport, Ethernet, TCP, and Wi-Fi overhead. Raise
`ESP_HOSTED_SPI_MAX_HZ` in steps (for example 10, 20, then 40 MHz) and verify
signal integrity at each step. Espressif's published throughput figures use a
higher SPI clock and cannot be reproduced at 5 MHz.

## Basic verification

Successful initialization prints the selected transport, pin mapping,
coprocessor firmware version, and `transport ready`. Then verify:

```text
wifi scan
wifi join <ssid> <password>
ifconfig
ping <gateway-address>
wifi ap <ssid> <password>
wifi list_sta
```

With `ESP_HOSTED_BLE` enabled, `/dev/hci0` must also be present and startup must
report `BLE controller transport available`. If CanMV NimBLE is enabled,
`bluetooth.BLE().active(True)` verifies HCI Reset and controller initialization;
then use `gap_scan` and `gap_advertise` to verify central and peripheral roles.

For SPI-HD, also check that `SPI-HD data path open` reports valid 1600-byte
buffers and that the negotiated line count matches the wiring.
