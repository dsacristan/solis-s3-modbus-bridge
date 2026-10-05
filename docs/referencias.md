# Referencias, créditos y agradecimientos

## ⭐ Proyecto del que más hemos sacado (crédito principal)

### [hn/ginlong-solis](https://github.com/hn/ginlong-solis) — *Ginlong Solis inverter → ESPHome*
por **Hajo Noerenberg** (`hn`), licencia **GPL-3.0**.

Sin este proyecto **no habríamos podido hacer nada de esto**. De él hemos tomado y aprendido:

- **El pinout completo del stick S3-WIFI-ST** y la identificación del módulo (MXCHIP EMW3080 =
  Realtek RTL8710BN, 8 MB de flash) — `solis-esphome-emw3080.yaml`.
- **Los mapas de registros Modbus**: `solis-modbus-inv.yaml` (familia INV, `3xxx`, FC3) y
  `solis-modbus-esinv.yaml` (familia **ESINV/híbrida**, `3xxxx`, **FC4**) — la clave para leer
  nuestro S5-EH1P.
- **El *watchdog* hardware en `PA00`** (placas S3 desde finales de 2022) y el botón `PA08`.
- **Los LEDs** `PA12` (COM) / `PA05` (NET).
- El **firmware ESPHome de referencia** (`esphome/`) con el que comparamos y con el que localizamos
  las dos causas raíz de nuestro problema (versiones de ESPHome y de LibreTiny).
- Sus **issues**, que fueron el mapa del tesoro:
  - **#72** — *"ESPhome 2026.3.0 wont compile"* + comentarios de `TjepkemaTechniek` y `zejulio`
    → ahí está documentado que **2026.2.x compila pero no lee datos** y que **2026.1.5 funciona**.
  - **#76** — *"READ THIS! ESPHome / LibreTiny Compile / ModBus / UART / WiFi problems"*
    → la recomendación de **pinar LibreTiny 1.10.0** en el YAML.
  - **#79** — *"ESPHome Device Builder 2026.5+ causes Solis S3 stick to lose WiFi"* (reportado por
    `ube80`).
  - **#85** — *"ESPHome 2026.9.0: INV YAML migration and successful RTL8710BN upgrade"*
    → reporte de éxito con 2026.9 y la mejora del *watchdog* (desacoplarlo de `api.connected`);
    esa idea es la que aplicamos para que el stick no se reinicie si HA se desconecta.
  - **#13** — el *watchdog* hardware en `PA00`.
  - **#48 / #91** — modo *UART boot* y el tamaño real de flash (8 MB, no 4 MB).
  - **#83** — "Lost all entities to my two Solis inverters" (síntoma equivalente al nuestro).

Publicamos allí nuestros hallazgos (ver `upstream/comment-72.md` y `upstream/comment-76.md`), en
particular el bug de **LibreTiny 1.13.0 rompiendo el RX del UART0**, que no estaba reportado.

---

## Cadenas de herramientas

| Herramienta | Uso | Enlace |
|---|---|---|
| **LibreTiny** | plataforma Arduino para RTL8710BN | https://github.com/libretiny-eu/libretiny |
| **PlatformIO** | build system | https://platformio.org |
| **ltchiptool** | flasheo serie / UF2 del RTL8710B | https://github.com/libretiny-eu/ltchiptool |
| **ArduinoCore-API (fork `hn`)** | core `Serial` con `RingBuffer` arreglado (libretiny#154) | https://github.com/hn/ArduinoCore-API (`#RingBufferFix`) |
| **ESPHome** | firmware de referencia alternativo | https://esphome.io |
| Realtek **AmebaZ SDK** | SDK del chip (dentro de LibreTiny) | — |

## Sistemas que consumen el puente

| Sistema | Uso | Enlace |
|---|---|---|
| **evcc** | gestión de carga solar (objetivo del proyecto) | https://evcc.io |
| **Home Assistant** | domótica; lectura vía integración `modbus` | https://www.home-assistant.io |

## Documentación y datasheets

- **Protocolo Modbus del inversor Solis ESINV** (híbrido): *"RS485_MODBUS Communication Protocol —
  ESINV-33000ID Hybrid Inverter"* (PDF del fabricante). De aquí salen las funciones (`02H`, `03H`,
  **`04H`**, `06H`, `10H`), el enlace **9600 8N1**, el *slave address* y el mapa `3xxxx` sin offset.
- **CA-IF4805HS** (Chipanalog) — transceptor **RS485 half-duplex** del stick, SOIC-8
  (pin 1 = RO, 2 = /RE, 3 = DE, 4 = DI, 6 = A, 7 = B). En la placa, **DE y /RE van unidos**.
- **MXCHIP EMW3080** / **Realtek RTL8710BN** — módulo y SoC; mapa de pines y mux de la FLASH SPI.
- **CRC-16/MODBUS** — valor de referencia de `"123456789"` = **`0x4B37`**; en el cable se envía
  **byte bajo primero**.

## Agradecimientos

- **Hajo Noerenberg (`hn`)** por `ginlong-solis`, su documentación y su firmware de referencia.
- **`TjepkemaTechniek`, `zejulio`, `ube80`, `NH-Networks`** y demás usuarios de los issues de
  `ginlong-solis` por compartir qué versiones funcionan y cuáles no: eso nos ahorró días.
- **A quien cedió el hardware de pruebas**, por las mediciones con el polímetro (transceptor, A/B, RO)
  y por la paciencia durante la depuración.

## Nota sobre licencias

`hn/ginlong-solis` es **GPL-3.0**, y este proyecto también, en coherencia (usamos su YAML de ESPHome
de referencia y derivamos de su conocimiento del hardware). Los ficheros de `esphome/` conservan su
cabecera original cuando procede.
