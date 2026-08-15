# lwIP IPv4 NAT

This component provides stateful IPv4 network address and port translation for
TCP, UDP, and ICMP echo traffic. It is intended for routing clients on an
internal interface, such as the Wi-Fi AP interface, through lwIP's default
uplink, such as the Wi-Fi STA interface.

Enable `LWIP_USING_NAT` in Kconfig. When the RT-Thread Wi-Fi AP and DHCP server
are enabled, NAT is enabled automatically while the AP is running. The STA (or
another routed interface) must have an address, gateway, and default route.

For a different internal interface, enable and disable translation from a
non-lwIP thread with:

```c
#include <ipv4_nat.h>

ip_nat_set_enabled(internal_netif, 1);
ip_nat_set_enabled(internal_netif, 0);
```

The session table size, translated port range, and protocol timeouts are
configurable under `LWIP_USING_NAT`. IPv4 fragments, port forwarding, and ICMP
error payload translation are not supported.

Use the `nat` MSH command to display enabled internal interfaces and active
session counts. NAT sessions are forwarding state and do not appear in
`netstat`, which only lists local TCP and UDP protocol control blocks.
