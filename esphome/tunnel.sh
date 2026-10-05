#!/bin/bash
# Abre un túnel SSH hacia el stick (para llegar a su web/HTTP desde fuera de su LAN).
#
# Ajusta estas variables a tu entorno (o expórtalas antes de llamar al script):
HA_SSH="${HA_SSH:-usuario@host-que-alcanza-el-stick}"   # p. ej. tu Home Assistant
SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519}"
STICK="${STICK:-192.168.1.254}"
PORT="${1:-80}"
SSHOPT="-i $SSH_KEY -o StrictHostKeyChecking=accept-new -o ConnectTimeout=12 -o BatchMode=yes"

# Cierra un túnel previo de ese puerto (por puerto, sin pkill -f que se autodestruye)
fuser -k "${PORT}/tcp" 2>/dev/null
sleep 1
ssh $SSHOPT -f -N -L ${PORT}:${STICK}:${PORT} "${HA_SSH}"
sleep 2
echo "túnel localhost:${PORT} -> ${STICK}:${PORT} (vía ${HA_SSH}) listo"

# ⚠️ NOTA: la API nativa y la OTA de ESPHome NO funcionan a través de un túnel SSH
# (el stick resetea el handshake). Para OTA/API, ejecuta desde la MISMA LAN.
# El túnel sí sirve para HTTP (dashboard, /api/status, /raw) y para OTA **por web**.
