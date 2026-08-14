# lwIP 2.2.1 RT-Thread port

This directory is based on the upstream `STABLE-2_2_1_RELEASE` and adds the
RT-Thread port required by the SAL socket and netdev frameworks.

## lwIP 2.2.x changes handled by this port

- `src/core/ipv4/acd.c` is built so DHCP uses lwIP 2.2's RFC 5227 address
  conflict detection implementation.
- The port uses lwIP 2.2's `MEM_CUSTOM_ALLOCATOR` interface for the RT-Thread
  aligned heap instead of replacing lwIP's memory implementation.
- PPP serial output uses the new const-qualified `sio_write` signature.
- The new `tcpip_callback_wait()` core API is available through the normal
  `tcpip.c` build, with no RT-Thread-specific override.
- Netdev state is synchronized using `LWIP_NETIF_EXT_STATUS_CALLBACK`, including
  IPv4 address, gateway, netmask, administrative state, link state, and IPv6
  address changes.
- The upstream global `dns_setserver()` API is preserved. The RT-Thread
  `dns_setserver_for_netif()` and `dns_getserver_for_netif()` extensions keep
  DHCPv4, DHCPv6, RDNSS, PPP, and manually configured DNS synchronized per
  netdev, while only the default interface updates lwIP's global resolver.
- The SCons source list follows 2.2.1's new core source requirements. New app
  layouts such as the combined TFTP client/server and split mDNS sources are
  retained in the imported upstream tree. The MQTT API also retains 2.2.1's
  length-qualified binary Will message support. These application modules can
  be enabled by future RT-Thread application configuration.

## RT-Thread integration files

- `SConscript`: selects the lwIP 2.2.1 sources and include paths.
- `src/lwipopts.h` and `src/lwippools.h`: map RT-Thread configuration to lwIP.
- `src/arch`: implements the lwIP OS abstraction on RT-Thread.
- `src/netif/ethernetif.c` and `src/include/netif/ethernetif.h`: bridge RT-Thread
  Ethernet devices to lwIP and netdev.
- `src/apps/ping/ping.c`: provides the ping implementation expected by netdev.
- `src/api/sockets.c` and `src/include/lwip/priv/sockets_priv.h`: expose the
  socket state and RT-Thread wait queue needed by SAL POSIX poll support.
- The socket layer exposes lwIP's fixed `TCP_SND_BUF` capacity through
  `SO_SNDBUF` and reports `TCP_MAXSEG`. This lets POSIX applications such as
  iperf3 inspect the connection without claiming per-socket buffer resizing.
