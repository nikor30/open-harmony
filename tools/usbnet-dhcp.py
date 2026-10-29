#!/usr/bin/env python3
"""Minimal DHCP responder for Harmony 900/1000/1100 remotes (usbnet).

Usage: usbnet-dhcp.py <interface> [seconds]

These remotes appear as a USB network device and ask for an address by
DHCP; concordance then talks to them at 169.254.1.2 (port 3074). This
answers DISCOVER/REQUEST on the given interface only, always with
169.254.1.2, server 169.254.1.1. It uses a raw packet socket, so it works
behind a host firewall that drops incoming DHCP. Needs root and
169.254.1.1/16 on the interface.
"""
import socket
import struct
import sys
import time

SERVER = socket.inet_aton("169.254.1.1")
CLIENT = socket.inet_aton("169.254.1.2")
MASK = socket.inet_aton("255.255.0.0")
LEASE = 24 * 3600


def csum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack(f"!{len(data) // 2}H", data))
    s = (s >> 16) + (s & 0xFFFF)
    s += s >> 16
    return ~s & 0xFFFF


def dhcp_reply(req, msg_type):
    xid, flags, chaddr = req[4:8], req[10:12], req[28:44]
    body = struct.pack("!BBBB", 2, 1, 6, 0) + xid + b"\0\0" + flags
    body += b"\0" * 4 + CLIENT + SERVER + b"\0" * 4 + chaddr + b"\0" * 192
    body += b"\x63\x82\x53\x63"
    body += bytes([53, 1, msg_type]) + bytes([54, 4]) + SERVER
    body += bytes([51, 4]) + struct.pack("!I", LEASE) + bytes([1, 4]) + MASK
    body += b"\xff"
    return body


def frame(src_mac, dst_mac, payload):
    udp = struct.pack("!HHHH", 67, 68, 8 + len(payload), 0) + payload
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0,
                     SERVER, b"\xff\xff\xff\xff")
    ip = ip[:10] + struct.pack("!H", csum(ip)) + ip[12:]
    return dst_mac + src_mac + b"\x08\x00" + ip + udp


def options(dhcp):
    opts, i = {}, 240
    while i < len(dhcp) and dhcp[i] != 0xFF:
        if dhcp[i] == 0:
            i += 1
            continue
        opts[dhcp[i]] = dhcp[i + 2:i + 2 + dhcp[i + 1]]
        i += 2 + dhcp[i + 1]
    return opts


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__.strip())
    iface = sys.argv[1]
    until = time.time() + (float(sys.argv[2]) if len(sys.argv) == 3 else 120)
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0800))
    s.bind((iface, 0))
    s.settimeout(1)
    my_mac = s.getsockname()[4]
    while time.time() < until:
        try:
            f, addr = s.recvfrom(2048)
        except socket.timeout:
            continue
        if addr[2] == socket.PACKET_OUTGOING or f[23] != 17:
            continue
        ihl = (f[14] & 0xF) * 4
        sport, dport = struct.unpack("!HH", f[14 + ihl:18 + ihl])
        if (sport, dport) != (68, 67):
            continue
        dhcp = f[14 + ihl + 8:]
        mtype = options(dhcp).get(53, b"\0")[0]
        reply = {1: 2, 3: 5}.get(mtype)  # DISCOVER -> OFFER, REQUEST -> ACK
        if not reply:
            continue
        s.send(frame(my_mac, f[6:12], dhcp_reply(dhcp, reply)))
        print(f"{'OFFER' if reply == 2 else 'ACK'} 169.254.1.2 to {f[6:12].hex(':')}")
        if reply == 5:
            return


if __name__ == "__main__":
    main()
