#!/usr/bin/env python3
"""Lector por lotes del inversor a través del puente Modbus TCP (:502, FC4).

Uso:  mbread.py <ip> <unit> [addr:count ...]
Sin argumentos lee el juego de registros de interés para la migración HA.
"""
import socket, struct, sys

def crc16(d):
    c = 0xFFFF
    for b in d:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c & 0xFFFF

def read(ip, unit, addr, count, port=502):
    pdu = bytes([0x04]) + struct.pack(">HH", addr, count)
    mbap = struct.pack(">HHHB", 1, 0, len(pdu) + 1, unit) + pdu
    s = socket.create_connection((ip, port), timeout=6)
    s.sendall(mbap)
    hdr = s.recv(7)
    if len(hdr) < 7:
        s.close(); return None
    ln = struct.unpack(">H", hdr[4:6])[0]
    body = b""
    while len(body) < ln - 1:
        ch = s.recv(ln - 1 - len(body))
        if not ch: break
        body += ch
    s.close()
    if body and body[0] & 0x80:
        return "EXC 0x%02X" % body[1]
    if len(body) < 2:
        return None
    return body[2:2 + body[1]]

def val(raw, scale=1.0, signed=False, dword=False):
    if raw is None or isinstance(raw, str): return raw
    if dword:
        v = struct.unpack(">i" if signed else ">I", raw[:4])[0]
    else:
        v = struct.unpack(">h" if signed else ">H", raw[:2])[0]
    return round(v * scale, 2)

# (nombre, addr, count, escala, signed, dword)
MAP = [
    ("Total energy (kWh)",              33029, 2, 1,    False, True),
    ("Energy today (kWh)",              33035, 1, 0.1,  False, False),
    ("DC voltage 1 (V)",                33049, 1, 0.1,  False, False),
    ("DC power PV? 33055",              33055, 2, 1,    False, True),
    ("Total DC output power (W)",       33057, 2, 1,    False, True),
    ("AC voltage (V)",                  33073, 1, 0.1,  False, False),
    ("AC current?",                     33076, 1, 0.1,  False, False),
    ("Active power AC (W)",             33079, 2, 1,    True,  True),
    ("Inverter temp (C)",               33093, 1, 0.1,  False, False),
    ("Grid frequency (Hz)",             33094, 1, 0.01, False, False),
    ("Battery voltage (V)",             33133, 1, 0.1,  False, False),
    ("Battery current (A)",             33134, 1, 0.1,  True,  False),
    ("Battery direction (0=c,1=d)",     33135, 1, 1,    False, False),
    ("Battery SoC (%)",                 33139, 1, 1,    False, False),
    ("Battery SoH (%)",                 33140, 1, 1,    False, False),
    ("Household load power (W)",        33147, 1, 1,    False, False),
    ("Backup load power (W)",           33148, 1, 1,    False, False),
    ("Battery power (W)",               33149, 2, 1,    True,  True),
    ("Total battery charge (kWh)",      33161, 2, 1,    False, True),
    ("Total battery discharge (kWh)",   33165, 2, 1,    False, True),
    ("METER 33249..33267",              33249, 19, 1,   False, True),
    ("Meter grid energy from (0.01)",   33283, 2, 0.01, False, True),
    ("Meter grid energy to (0.01)",     33285, 2, 0.01, False, True),
]

if __name__ == "__main__":
    ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.254"
    unit = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    print(f"# inversor en {ip} unit={unit} (FC4)")
    for name, addr, cnt, sc, sg, dw in MAP:
        raw = read(ip, unit, addr, cnt)
        if isinstance(raw, bytes) and cnt >= 4 and dw and cnt > 2:
            vals = [val(raw[i:i+4], sc, sg, True) for i in range(0, len(raw) - 3, 2)]
            print(f"{name:32s} @{addr:5d} = {vals}")
        else:
            print(f"{name:32s} @{addr:5d} = {val(raw, sc, sg, dw)}   (raw={raw.hex() if isinstance(raw,bytes) else raw})")
