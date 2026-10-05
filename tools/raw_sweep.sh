#!/bin/bash
# Barrido rápido por el endpoint /raw del puente: prueba varios unit-id y funciones
# y muestra los bytes TX/RX reales. Útil para diagnosticar el enlace RS485.
#
# Uso: raw_sweep.sh [IP-del-stick]     (por defecto 192.168.1.254)
IP="${1:-192.168.1.254}"
B="http://$IP/raw"

echo "--- barrido de unit (FC4 reg 35000) ---"
for u in 01 02 03 65 f7 00 ff; do
  printf 'unit=%s -> ' "$u"
  curl -s --max-time 10 "$B?hex=${u}0488B80001&crc=1"; echo
done

echo "--- otras funciones (unit 01) ---"
printf 'FC3 reg 3000 -> '; curl -s --max-time 10 "$B?hex=01030BB80001&crc=1"; echo
printf 'FC4 reg 3000 -> '; curl -s --max-time 10 "$B?hex=01040BB80001&crc=1"; echo
printf 'FC4 reg 0    -> '; curl -s --max-time 10 "$B?hex=010400000001&crc=1"; echo
printf 'FC1 reg 0    -> '; curl -s --max-time 10 "$B?hex=010100000001&crc=1"; echo

echo "--- solo escuchar 1 s (sin TX) ---"
printf 'listen       -> '; curl -s --max-time 12 "$B?hex="; echo
