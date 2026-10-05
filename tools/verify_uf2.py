#!/usr/bin/env python3
"""
verify_uf2.py — Verifica un firmware.bin del stick S3 (contenedor UF2 LibreTiny)
y reconstruye la imagen cruda de arranque AmebaZ, sin necesidad de hardware.

Uso:
    python3 tools/verify_uf2.py .pio/build/solis-s3/firmware.bin

Comprueba:
  * tamaño múltiplo de 512
  * cabeceras UF2 (magicStart0/1, magicEnd) y blockNo/numBlocks de cada bloque
  * familyID coherente (0x22E0D6FC = LibreTiny RTL8710B)
  * reconstruye la imagen cruda colocando cada payload en su dirección destino
    y verifica la cabecera de arranque AmebaZ: la cadena ASCII "81958711" en 0x0
  * busca la segunda imagen ("RTKWin") y el nombre del inversor ("Solis")
  * imprime sha256 del .bin y de la imagen cruda
"""
import hashlib
import struct
import sys

UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
FAMILY_RTL8710B = 0x22E0D6FC
AMEBAZ_BOOT_STR = b"81958711"   # cabecera de arranque en 0x0 (ASCII)


def main(path):
    data = open(path, "rb").read()
    if len(data) % 512:
        print(f"ERROR: tamaño {len(data)} no es múltiplo de 512")
        return 1

    nblocks = len(data) // 512
    print(f"Archivo: {path}")
    print(f"Tamaño : {len(data)} bytes = {nblocks} bloques de 512")

    fam = None
    declared = None
    placements = []           # (addr, payload)
    for i in range(nblocks):
        b = data[i * 512:(i + 1) * 512]
        m0, m1, flags, addr, size, blkno, numblk, family = struct.unpack("<8I", b[:32])
        mend = struct.unpack("<I", b[508:512])[0]
        if m0 != UF2_MAGIC0 or m1 != UF2_MAGIC1:
            print(f"  [FALLO] bloque {i}: magicStart incorrecto")
            return 1
        if mend != UF2_MAGIC_END:
            print(f"  [FALLO] bloque {i}: magicEnd incorrecto")
            return 1
        if blkno != i:
            print(f"  [FALLO] bloque {i}: blockNo={blkno} (se esperaba {i})")
            return 1
        if size > 476:
            print(f"  [FALLO] bloque {i}: payloadSize={size} > 476")
            return 1
        if fam is not None and family != fam:
            print(f"  [FALLO] bloque {i}: familyID cambia ({family:#x} != {fam:#x})")
            return 1
        if declared is not None and numblk != declared:
            print(f"  [FALLO] bloque {i}: numBlocks={numblk} (se esperaba {declared})")
            return 1
        fam = family
        declared = numblk
        if size:
            placements.append((addr, b[32:32 + size]))

    print(f"  OK: {nblocks}/{declared} bloques UF2 válidos")
    tag = "  (LibreTiny RTL8710B)" if fam == FAMILY_RTL8710B else "  [AVISO: no es RTL8710B]"
    print(f"  familyID = {fam:#010x}{tag}")

    # Reconstrucción por dirección destino.
    top = max(a + len(p) for a, p in placements)
    image = bytearray(top)
    for addr, payload in placements:
        image[addr:addr + len(payload)] = payload
    print(f"  Imagen cruda: {len(image)} bytes (0x0..{top:#x})")

    ok = True
    if bytes(image[:8]) == AMEBAZ_BOOT_STR:
        print("  Cabecera de arranque AmebaZ '81958711' en 0x0: OK")
    else:
        print(f"  [FALLO] cabecera en 0x0 = {bytes(image[:8])!r} (se esperaba {AMEBAZ_BOOT_STR!r})")
        ok = False

    for tagb in (b"RTKWin", b"Solis"):
        idx = image.find(tagb)
        print(f"  '{tagb.decode()}': {hex(idx) if idx >= 0 else 'AUSENTE'}")

    print(f"  sha256(.bin)  = {hashlib.sha256(data).hexdigest()}")
    print(f"  sha256(cruda) = {hashlib.sha256(bytes(image)).hexdigest()}")
    print("RESULTADO:", "OK" if ok else "FALLO")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else ".pio/build/solis-s3/firmware.bin"))
