# Solis S3-WIFI-ST → puente Modbus TCP/RTU local

Convierte el **datalogger WiFi original** de un inversor Solis (stick **S3-WIFI-ST**, módulo MXCHIP
EMW3080 = **Realtek RTL8710BN**) en un **puente Modbus local**: lee el inversor por RS485 y lo expone
por red como **Modbus TCP (:502)** y **RTU crudo sobre TCP (:5020)**, con panel web y OTA por WiFi.

**Objetivo:** que **evcc** (y/o Home Assistant) lean el inversor **sin depender de SolisCloud**, con
datos en tiempo real, para **cargar el coche con el excedente solar**.

---

## ⚠️ Estado: FUNCIONA (verificado en hardware)

Este repositorio nació de semanas de depuración. El puente **lee el inversor** y sirve Modbus TCP.
Varias trampas nos costaron mucho tiempo — **si vas a reutilizar este código, léelas antes** (están
detalladas en [`docs/hallazgos.md`](docs/hallazgos.md)):

1. **`platform = libretiny@1.10.0` es OBLIGATORIO.** Con LibreTiny **1.13.0 el RX del UART0 no recibe
   nada** (el TX sale perfecto, pero cero bytes entrantes). Es un bug del driver del core.
2. Si usas el firmware **ESPHome de referencia** en su lugar: las versiones **2026.2–2026.6 compilan
   pero NO leen Modbus** (y rompen la API y la OTA). Usa **2026.1.x**.
3. **El keepalive TCP de lwIP por defecto es de 2 horas.** En builds solo-WiFi el override del SDK
   (`TCP_KEEPIDLE_DEFAULT=10 s`) queda compilado fuera, así que un cliente que muere sin cerrar la
   conexión ocupa su *slot* ~2 h. El firmware lo endurece vía `build_flags` (keepalive corto + pool de
   sockets mayor) y añade **evicción por inactividad**; detalle en [`docs/hallazgos.md`](docs/hallazgos.md).

Y otra, de operación: **cambiar de firmware borra la config WiFi del puente** (ambos firmwares
usan la partición `kvs`), así que el stick arranca en modo AP y hay que reconectar la WiFi.

---

## Hardware

| Elemento | Detalle |
|---|---|
| Stick | Solis **S3-WIFI-ST** (EMW3080 / RTL8710BN, 8 MB flash, WiFi) |
| Inversor probado | Solis **S5-EH1P4.6K-L** (híbrido con batería, familia **ESINV**) |
| Enlace | **RS485 · 9600 8N1 · unit id 1 · FC4** (por el *COM Port* Exceedconn de 4 pines) |
| Programador (1ª vez) | Adaptador **USB-serie 3,3 V** (FT232RL u similar) + `ltchiptool` |

**Pinout del stick** (confirmado contra hardware):

| Pin | Función |
|---|---|
| `PA23` | RS485 **TX** (DI del transceptor) |
| `PA18` | RS485 **RX** (RO del transceptor) |
| `PA19` | **DE/RE** del transceptor (van unidos) |
| `PA30` / `PA29` | Consola/LOG (UART2, 9600) |
| `PA12` | LED **COM** (naranja) |
| `PA05` | LED **NET** (verde) |
| `PA00` | *kick* del **watchdog hardware** (imprescindible en placas nuevas) |
| `PA08` | Botón (>10 s = modo configuración) |

> ⛔ **PA6–PA11 son pines de la FLASH SPI (XIP).** Tocar `PA6` (SPIC_CS) provoca un **Hard Fault
> instantáneo**. No usarlos nunca como GPIO.

Más detalle: [`docs/hardware.md`](docs/hardware.md).

---

## Compilar y flashear

### 1) Primera vez (por serie, UART boot mode)

```bash
# Requisitos: Python 3.12 + PlatformIO (penv).
# La plataforma libretiny@1.10.0 necesita el parche de compatibilidad con Python 3.12:
python3 scripts/patch-libretiny-libs-queue.py \
    ~/.platformio/platforms/libretiny@1.10.0/builder/utils/libs-queue.py

export HOME=$HOME
pio run -e solis-s3          # -> .pio/build/solis-s3/firmware.uf2
```

Para grabar el stick la primera vez hay que poner el MCU en **UART boot mode** (basta con un jumper;
no hay que soldar. Referencia: https://github.com/hn/ginlong-solis/issues/48#issuecomment-2371866988):

```bash
# 1. BACKUP OBLIGATORIO del firmware original (8 MiB exactos):
ltchiptool flash read -d /dev/ttyUSB0 RTL8710B backup-original.bin
# 2. Grabar:
ltchiptool flash write -d /dev/ttyUSB0 .pio/build/solis-s3/firmware.uf2
```

> Sin `-f`/`-s`: el `.uf2` ya lleva las direcciones de destino.

### 2) Actualizaciones (por WiFi, sin cable)

El firmware trae **OTA por web**: abre `http://<IP-del-stick>/update`, elige el `.uf2` y sube.
(Reversible: para volver a ESPHome, sube ahí su `.uf2`.)

### 3) Configurar la WiFi

Al arrancar sin configuración válida, el stick levanta un **AP abierto** `SolisS3-Setup`
(`http://192.168.4.1`). Conéctate con el móvil, elige tu red y guarda. El stick reinicia y entra en
tu WiFi (por DHCP). **No hay credenciales embebidas en el firmware.**

---

## Uso

| Endpoint | Para qué |
|---|---|
| `GET /` | Panel HTML con estado y diagnóstico RS485 |
| `GET /api/status` | JSON de estado (versión, WiFi, contadores Modbus, `evictions`, `idle_ms`, `rs485.rx_raw_bytes`) |
| `GET /raw?hex=<hex>&crc=1&hold=<ms>` | **Diagnóstico**: envía una trama RS485 y devuelve `{tx,rx,n,crc_ok}` |
| `GET /update` · `POST /update` | OTA por web (subir `.uf2`) |

**Modbus:**

| Puerto | Protocolo | Para |
|---|---|---|
| `502` | Modbus **TCP** (MBAP) | evcc (`modbus: tcpip`), HA (`type: tcp`) |
| `5020` | Modbus **RTU crudo** sobre TCP | evcc (`rs485tcpip`), HA (`type: rtuovertcp`) |

⚠️ El inversor S5-EH1P es **ESINV**: sus datos (`3xxxx`) se leen con **FC4** (`read: input`), **no**
FC3. Ejemplo de comprobación:

```bash
# FC4, unit 1, registro 35000 (tipo de inversor; responde 0x2030)
python3 tools/mbtest_tcp.py <IP> 502 1 35000 4 1
```

Configuración de referencia para **evcc**:

```yaml
meters:
  - name: pv
    type: template
    template: solis
    modbus: tcpip
    host: <IP-del-stick>
    port: 502
    id: 1
    # ESINV -> función 04 (input registers)
```

---

## Troubleshooting

| Síntoma | Causa / solución |
|---|---|
| Modbus TCP responde **excepción 0x0B** (`rz=0`) | El inversor no contesta. Comprueba con `/raw`; si `n=0`, revisa que **`platform = libretiny@1.10.0`** (con 1.13.0 el RX no funciona) y el cableado A/B |
| TX correcto pero **cero bytes RX** | LibreTiny **1.13.0** (usar 1.10.0) |
| El stick **desaparece de la red** tras cambiar de firmware | Su WiFi vive en `kvs` y el otro firmware la pisó → reconectar el AP `SolisS3-Setup` |
| El stick **no arranca / se reinicia cada ~15 min** | El *watchdog* hardware (PA00) no se está alimentando |
| `Hard Fault` al arrancar tu código | Has tocado un pin de FLASH (`PA6`–`PA11`) |

---

## Estructura del repositorio

```
src/            firmware del puente (PlatformIO / Arduino-LibreTiny)
  main.cpp      firmware completo (WiFi, web, Modbus, OTA, diagnóstico)
  modbus_proto.h  helpers Modbus PUROS (host-testeables): MBAP, CRC-16/RTU
  diag.cpp, diag_btn.cpp  firmwares de diagnóstico (entornos `diag` / `diag-btn`)
tools/          utilidades de host: test Modbus (g++), verify_uf2.py, mbtest_tcp.py, raw_sweep.sh
esphome/        YAML del firmware ESPHome de REFERENCIA (comparativa / alternativa)
ha/             paquete de Home Assistant (`modbus`, FC4 · :502) para leer el puente
docs/           documentación del proyecto (hallazgos, hardware, protocolo, referencias)
dist/           binarios compilados (.uf2 / .bin) + SHA256SUMS
```

---

## Créditos y referencias

Este proyecto se apoya **especialmente** en el trabajo de **Hajo Noerenberg**:

> **https://github.com/hn/ginlong-solis** — *Ginlong Solis inverter → ESPHome*. De aquí salen el pinout
> del stick S3, la definición del módulo EMW3080, los mapas de registros INV/ESINV, la referencia del
> *watchdog* PA00, el botón PA08, y el firmware ESPHome de referencia con el que depuramos. **Sin ese
> proyecto esto no habría sido posible.**

Lista completa de referencias, herramientas y agradecimientos: [`docs/referencias.md`](docs/referencias.md).

## Autoría

Proyecto desarrollado por **David** en colaboración con **Hermes** (agente de Nous Research) y
**DeepSeek** (modelo de lenguaje), que participaron en la depuración del firmware, del protocolo Modbus
y de la integración de Home Assistant.

## Licencia

**GPL-3.0** (ver [`LICENSE`](LICENSE)), en coherencia con el proyecto del que deriva.

## Aviso

Modificar el datalogger implica **abrir el stick y flashearlo**. Haz **copia de seguridad del firmware
original (8 MiB) ANTES** de tocar nada: es tu única vía de vuelta. Úsalo bajo tu responsabilidad.
