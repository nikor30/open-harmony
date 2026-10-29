# Harmony 900 (first contact)

A second remote, a Harmony 900, read with concordance. Nothing written.

```
Model:             Logitech Harmony 900
USB:               046d:c11f, "Belcarra Generic MDLM-BLAN"
Firmware Version:  7.5 (type 0)
Hardware Version:  0.1.15
External Flash:    2 MiB, 01:49 AMD Am29LV160BB
Architecture:      15
Protocol:          15
```

concordance's `SupportedModels.md` lists arch 15 / 900 as: config update
working, learn IR working, firmware update not working. The "Config Flash
Used" line of `concordance -i` shows a nonsense percentage for this remote.

## Connecting on Linux

Unlike the Harmony One (HID), the 900 is a USB network device: the kernel's
`zaurus` driver creates an Ethernet interface, the remote asks for an
address by DHCP, and concordance then talks to it at 169.254.1.2 (TCP port
3074, plus HTTP on port 80).

concordance ships a udev rule and `start_concordance_dhcpd.sh` (dnsmasq)
for this. Without them, on a desktop with NetworkManager and a firewall:

```bash
IF=enp9s0u1u4u4                                 # see `ip -br link`
nmcli device set $IF managed no                 # NM otherwise runs DHCP and drops the address
ip addr add 169.254.1.1/16 dev $IF
tools/usbnet-dhcp.py $IF 120                    # answers the remote's DHCP with 169.254.1.2
ping -c 2 169.254.1.2
local/bin/concordance -i -v
```

`tools/usbnet-dhcp.py` uses a raw packet socket, so it works even when the
host firewall drops incoming DHCP (UFW does by default). All of this is
undone when the remote is unplugged (the interface disappears).
