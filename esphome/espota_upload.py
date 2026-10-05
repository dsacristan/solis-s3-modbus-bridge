#!/usr/bin/env python3
"""Uploader OTA de ESPHome autónomo (solo stdlib de Python).

Sube un `.uf2` al servidor OTA de ESPHome del stick (rtl87xx -> puerto **8892**).
Útil cuando `esphome upload`/`esphome logs` fallan, p. ej. a través de un host
intermedio, o para flashear un firmware ajeno al propio ESPHome (mismo family UF2).

IMPORTANTE: el stick resetea el handshake si se hace a través de un túnel SSH.
Ejecútalo DESDE LA MISMA LAN que el stick.

Protocolo portado de `esphome/espota2.py`. Verificado en hardware.

Uso:
  espota_upload.py <host> <puerto> <firmware.uf2> [password]
  espota_upload.py 192.168.1.254 8892 firmware.uf2 'TU_OTA_PASSWORD'
"""
import sys, socket, gzip, hashlib, secrets, time

OTA_TYPE_UPDATE_APP = 0x00
OTA_VERSION_1_0 = 1
OTA_VERSION_2_0 = 2
MAGIC_BYTES = bytes([0x6C, 0x26, 0xF7, 0x5C, 0x45])

CLIENT_FEATURE_SUPPORTS_COMPRESSION = 0x01
CLIENT_FEATURE_SUPPORTS_SHA256_AUTH = 0x02
CLIENT_FEATURE_SUPPORTS_EXTENDED_PROTOCOL = 0x04
SERVER_FEATURE_SUPPORTS_COMPRESSION = 0x01

RESPONSE_OK = 0x00
RESPONSE_REQUEST_AUTH = 0x01
RESPONSE_REQUEST_SHA256_AUTH = 0x02
RESPONSE_AUTH_OK = 0x41
RESPONSE_UPDATE_PREPARE_OK = 0x42
RESPONSE_BIN_MD5_OK = 0x43
RESPONSE_RECEIVE_OK = 0x44
RESPONSE_UPDATE_END_OK = 0x45
RESPONSE_SUPPORTS_COMPRESSION = 0x46
RESPONSE_CHUNK_OK = 0x47
RESPONSE_FEATURE_FLAGS = 0x48

UPLOAD_BLOCK_SIZE = 8192
UPLOAD_BUFFER_SIZE = UPLOAD_BLOCK_SIZE * 8

ERRORS = {
    0x80: "Invalid magic byte",
    0x81: "Couldn't prepare flash for update (binary too big?)",
    0x82: "Authentication invalid (password?)",
    0x83: "Writing OTA data to flash failed",
    0x84: "Finishing update failed",
    0x85: "Invalid bootstrapping (manual reset needed)",
    0x86: "Wrong current flash config (wrong board)",
    0x87: "ESP does not have the requested flash size (wrong board)",
    0x8A: "No update partition found",
    0x8B: "Application MD5 mismatch",
    0x8E: "Unsupported OTA type",
    0xFF: "Unknown error from device",
}


class OTAError(Exception):
    pass


def recv_exact(sock, n, what):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise OTAError(f"dispositivo cerró la conexión en '{what}' "
                           f"(recibidos {len(data)}/{n} bytes)")
        data += chunk
    return data


def check(data, expect, what):
    if not data:
        raise OTAError(f"sin respuesta del dispositivo en '{what}'")
    dat = data[0]
    if dat in ERRORS:
        raise OTAError(f"error del dispositivo en '{what}': 0x{dat:02X} {ERRORS[dat]}")
    if expect is not None and dat not in expect:
        raise OTAError(f"respuesta inesperada en '{what}': 0x{dat:02X} "
                       f"(esperado {[hex(x) for x in expect]})")
    return dat


def recv_check(sock, n, what, expect):
    return check(recv_exact(sock, n, what), expect, what)


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    host, port, fw = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    password = sys.argv[4] if len(sys.argv) > 4 else None

    contents = open(fw, "rb").read()
    print(f"[*] firmware {fw}: {len(contents)} bytes")

    sock = socket.create_connection((host, port), timeout=20)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"[*] conectado a {host}:{port}")

    sock.sendall(MAGIC_BYTES)
    r = recv_exact(sock, 2, "version")
    check(r, [RESPONSE_OK], "version")
    version = r[1]
    print(f"[*] device OTA version = {version}")

    sock.sendall(bytes([CLIENT_FEATURE_SUPPORTS_COMPRESSION
                        | CLIENT_FEATURE_SUPPORTS_SHA256_AUTH
                        | CLIENT_FEATURE_SUPPORTS_EXTENDED_PROTOCOL]))
    f = recv_exact(sock, 1, "features")[0]
    extended = False
    if f == RESPONSE_FEATURE_FLAGS:
        extended = True
        flags = recv_exact(sock, 1, "feature flags")[0]
        f = flags
        print(f"[*] protocolo extendido, flags=0x{flags:02X}")
    elif f == RESPONSE_SUPPORTS_COMPRESSION:
        f = SERVER_FEATURE_SUPPORTS_COMPRESSION
    else:
        f = 0

    compress = bool(f & SERVER_FEATURE_SUPPORTS_COMPRESSION)
    upload = gzip.compress(contents, 9) if compress else contents
    print(f"[*] compresión={'sí' if compress else 'no'} -> {len(upload)} bytes")

    auth = recv_exact(sock, 1, "auth")[0]
    if auth == RESPONSE_AUTH_OK:
        print("[*] sin autenticación")
    elif auth == RESPONSE_REQUEST_AUTH:  # MD5
        nonce = recv_exact(sock, 32, "md5 nonce").decode()
        cnonce = secrets.token_hex(16)
        sock.sendall(cnonce.encode())
        h = hashlib.md5()
        h.update((password or "").encode()); h.update(nonce.encode()); h.update(cnonce.encode())
        sock.sendall(h.hexdigest().encode())
        recv_check(sock, 1, "auth result", [RESPONSE_AUTH_OK])
        print("[*] auth MD5 OK")
    elif auth == RESPONSE_REQUEST_SHA256_AUTH:  # SHA256
        nonce = recv_exact(sock, 64, "sha256 nonce").decode()
        cnonce = secrets.token_hex(32)
        sock.sendall(cnonce.encode())
        h = hashlib.sha256()
        h.update((password or "").encode()); h.update(nonce.encode()); h.update(cnonce.encode())
        sock.sendall(h.hexdigest().encode())
        recv_check(sock, 1, "auth result", [RESPONSE_AUTH_OK])
        print("[*] auth SHA256 OK")
    else:
        check(bytes([auth]), None, "auth")

    sock.settimeout(90.0)
    if extended:
        sock.sendall(bytes([OTA_TYPE_UPDATE_APP]))

    size = len(upload)
    sock.sendall(size.to_bytes(4, "big"))
    recv_check(sock, 1, "update prepare", [RESPONSE_UPDATE_PREPARE_OK])
    print("[*] update prepare OK")

    sock.sendall(hashlib.md5(upload).hexdigest().encode())
    recv_check(sock, 1, "bin md5", [RESPONSE_BIN_MD5_OK])
    print("[*] md5 aceptado")

    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 0)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, UPLOAD_BUFFER_SIZE)
    t0 = time.perf_counter()
    off = 0
    while off < size:
        chunk = upload[off:off + UPLOAD_BLOCK_SIZE]
        off += len(chunk)
        sock.sendall(chunk)
        if version >= OTA_VERSION_2_0:
            recv_check(sock, 1, "chunk", [RESPONSE_CHUNK_OK])
    dur = time.perf_counter() - t0
    print(f"[*] enviados {size} bytes en {dur:.1f}s ({size/dur/1024:.0f} KB/s)")

    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    recv_check(sock, 1, "receive", [RESPONSE_RECEIVE_OK])
    recv_check(sock, 1, "update end", [RESPONSE_UPDATE_END_OK])
    sock.sendall(bytes([RESPONSE_OK]))
    sock.close()
    print("[+] OTA completado con ÉXITO (el dispositivo se reiniciará)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except OTAError as e:
        print(f"[!] ERROR OTA: {e}")
        sys.exit(1)
    except Exception as e:
        print(f"[!] ERROR: {type(e).__name__}: {e}")
        sys.exit(1)
