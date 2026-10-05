#!/usr/bin/env python3
"""Descubrimiento: busca en el mapa ESINV los registros que corresponden a los
totales conocidos de la nube (para no adivinar).

  total_energy_purchased = 4437.0 kWh  (x100 = 443700)
  total_on_grid_energy   = 8317.0 kWh  (x100 = 831700)
  energy_total           = 24756 kWh   (x1  = 24756)
  total_energy_used      = 21323 kWh   (x1 / x10 / x100)
"""
import socket, struct

IP, UNIT, PORT = "192.168.1.254", 1, 502

def read(addr, count):
    pdu = bytes([0x04]) + struct.pack(">HH", addr, count)
    s = socket.create_connection((IP, PORT), timeout=6)
    s.sendall(struct.pack(">HHHB", 1, 0, len(pdu) + 1, UNIT) + pdu)
    hdr = s.recv(7)
    if len(hdr) < 7: s.close(); return None
    ln = struct.unpack(">H", hdr[4:6])[0]
    body = b""
    while len(body) < ln - 1:
        c = s.recv(ln - 1 - len(body))
        if not c: break
        body += c
    s.close()
    if body and body[0] & 0x80: return None
    return body[2:2 + body[1]]

# Descarga el rango ESINV de datos como uint16
data = {}
for base in range(33020, 33300, 20):
    raw = read(base, 20)
    if raw:
        for i in range(0, len(raw), 2):
            data[base + i // 2] = struct.unpack(">H", raw[i:i+2])[0]

TARGETS = {
    "purchased x100=443700": 443700, "on_grid x100=831700": 831700,
    "used x1=21323": 21323, "used x10=213230": 213230, "used x100=2132300": 2132300,
    "gen x1=24756": 24756, "gen x10=247560": 247560,
}
print("=== coincidencias uint32 (pares de registros) ===")
for a in sorted(data):
    if a + 1 not in data: continue
    v = (data[a] << 16) | data[a + 1]
    for name, t in TARGETS.items():
        if abs(v - t) <= max(3, t * 0.002):
            print(f"  @{a}  uint32={v}   <- {name}")
print("=== coincidencias uint16 ===")
for a in sorted(data):
    for name, t in TARGETS.items():
        if t < 65536 and abs(data[a] - t) <= 2:
            print(f"  @{a}  uint16={data[a]}   <- {name}")
print()
print("=== bloque METER 33249..33272 (uint16) ===")
for a in range(33249, 33273):
    if a in data: print(f"  @{a} = {data[a]:6d}  0x{data[a]:04x}")
print()
print("=== bloque 33280..33299 (uint16) ===")
for a in range(33280, 33300):
    if a in data: print(f"  @{a} = {data[a]:6d}  0x{data[a]:04x}")
print()
print("=== bloque 33126..33172 (uint16) ===")
for a in range(33126, 33172):
    if a in data: print(f"  @{a} = {data[a]:6d}  0x{data[a]:04x}")
