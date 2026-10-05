#!/bin/bash
# Compila el firmware ESPHome de referencia y lo sube por OTA al stick.
#
# Ajusta estas variables a tu entorno (o expórtalas):
HA_SSH="${HA_SSH:-usuario@host-que-alcanza-el-stick}"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519}"
STICK="${STICK:-192.168.1.254}"
VENV="${1:-esphome-env}"          # venv con ESPHome instalado
SSHOPT="-i $SSH_KEY -o StrictHostKeyChecking=accept-new -o ConnectTimeout=12 -o BatchMode=yes"

echo "=== OTA (mejor desde la misma LAN que el stick) ==="
../$VENV/bin/esphome upload solis-esphome-emw3080.yaml --device "${STICK}" 2>&1 | tail -20

# Alternativa si `esphome upload` falla: usa espota_upload.py sobre el maestro del repo.
