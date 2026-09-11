# -*- coding: utf-8 -*-
"""Minimal WebSocket client to pull /ws/logs from the ESP32 board."""
import socket
import base64
import os
import struct
import time
import sys

HOST = "192.168.66.71"
PORT = 80
PATH = "/ws/logs"
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 4.0

s = socket.create_connection((HOST, PORT), timeout=8)
key = base64.b64encode(os.urandom(16)).decode()
req = (
    "GET %s HTTP/1.1\r\n"
    "Host: %s:%d\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n"
) % (PATH, HOST, PORT, key)
s.sendall(req.encode())

# read handshake response
buf = b""
while b"\r\n\r\n" not in buf:
    chunk = s.recv(4096)
    if not chunk:
        break
    buf += chunk
    if len(buf) > 65536:
        break
head = buf.split(b"\r\n\r\n")[0]
print("=== HANDSHAKE ===")
print(head.decode("latin1", "replace"))
if b" 101 " not in head:
    print("handshake failed")
    sys.exit(1)

s.settimeout(0.5)
start = time.time()
count = 0
while time.time() - start < DURATION:
    try:
        hdr = s.recv(2)
        if len(hdr) < 2:
            continue
        b0, b1 = hdr[0], hdr[1]
        opcode = b0 & 0x0F
        masked = (b1 & 0x80) != 0
        ln = b1 & 0x7F
        if ln == 126:
            ln = struct.unpack(">H", s.recv(2))[0]
        elif ln == 127:
            ln = struct.unpack(">Q", s.recv(8))[0]
        mask = s.recv(4) if masked else b""
        payload = b""
        while len(payload) < ln:
            chunk = s.recv(ln - len(payload))
            if not chunk:
                break
            payload += chunk
        if masked:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        if opcode == 1:  # text
            try:
                sys.stdout.write(payload.decode("utf-8", "replace"))
            except Exception:
                pass
            count += 1
        elif opcode == 8:
            break
    except socket.timeout:
        continue
    except Exception as e:
        break
s.close()
print("\n=== END (frames=%d) ===" % count)
