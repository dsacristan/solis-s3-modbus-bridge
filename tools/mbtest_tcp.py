#!/usr/bin/env python3
"""Lee un registro Modbus por TCP (puerto 502, MBAP) del puente. FC4 por defecto."""
import socket, struct, sys

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.254"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 502
UNIT = int(sys.argv[3]) if len(sys.argv) > 3 else 1
REG  = int(sys.argv[4]) if len(sys.argv) > 4 else 35000
FC   = int(sys.argv[5]) if len(sys.argv) > 5 else 4
N    = int(sys.argv[6]) if len(sys.argv) > 6 else 1


def read(fc, reg, n):
    pdu = struct.pack(">BHH", fc, reg, n)
    mbap = struct.pack(">HHHB", 1, 0, len(pdu) + 1, UNIT) + pdu
    s = socket.socket(); s.settimeout(6)
    s.connect((HOST, PORT)); s.sendall(mbap)
    r = s.recv(512); s.close()
    return r


print(f"-- TCP {HOST}:{PORT} unit={UNIT} FC{FC} reg={REG} n={N}")
try:
    r = read(FC, REG, N)
except Exception as e:
    print("   ERROR:", type(e).__name__, e); sys.exit(1)
print("   RX:", r.hex(" "))
if len(r) >= 9:
    tid, proto, ln, unit = struct.unpack(">HHHB", r[:7])
    fc = r[7]
    if fc & 0x80:
        print(f"   EXCEPCION FC=0x{fc:02x} code=0x{r[8]:02x}")
    else:
        bc = r[8]
        vals = [struct.unpack(">H", r[9 + 2 * i:11 + 2 * i])[0] for i in range(bc // 2)]
        print(f"   OK: {bc} bytes -> registros {[hex(v) for v in vals]} = {vals}")
else:
    print("   respuesta corta/inesperada")

# Sondeo en ASCII: unit=10 es el datalogger (referencia Solis), y 35000 suele dar 0x0020
