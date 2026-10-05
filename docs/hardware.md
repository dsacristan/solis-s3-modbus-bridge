# Hardware: el stick S3-WIFI-ST (EMW3080 / RTL8710BN)

El stick que Solis vende como datalogger ("S3-WIFI-ST") lleva dentro un módulo **MXCHIP EMW3080**
que es, en realidad, un **Realtek RTL8710BN** (AmebaZ) con **8 MB de flash** (aunque el perfil de
placa de LibreTiny asuma 4 MB) y **WiFi**. Su firmware original es **AliOS-Things** y habla con
SolisCloud por MQTT de Alibaba.

---

## Mapa de pines (confirmado en hardware)

| Pin | Función | Notas |
|---|---|---|
| `PA23` | **UART0 TX** → DI del transceptor RS485 | `Serial0` |
| `PA18` | **UART0 RX** ← RO del transceptor RS485 | `Serial0` |
| `PA19` | **DE/RE** del transceptor | DE y /RE van **unidos** → un solo pin |
| `PA30` | **UART2 TX** (consola / LOG) | `Serial` (¡el mismo que usa `ltchiptool`!) |
| `PA29` | **UART2 RX** (consola / LOG) | `Serial`; **no** es el RS485 |
| `PA12` | LED **COM** (naranja) | actividad HTTP |
| `PA05` | LED **NET** (verde) | estado WiFi |
| `PA00` | ***kick* del watchdog hardware** | pulso HIGH ~100 ms; ver abajo |
| `PA08` | **Botón** | INPUT_PULLUP, activo-LOW; >10 s = modo configuración |
| LED `POWER` | — | fijo a VCC (siempre encendido) |

### ⛔ Pines PROHIBIDOS (FLASH SPI / XIP)

| Pin | Función | |
|---|---|---|
| `PA6` | **SPIC_CS** | **CRÍTICO**: tocarlo = *Hard Fault* instantáneo |
| `PA7` | SPIC_DATA1 | reservado |
| `PA8` | SPIC_DATA2 | reservado *en el chip*… pero **libre en esta placa** (es el botón) |
| `PA9` | SPIC_DATA0 | reservado |
| `PA10` | SPIC_CLK | reservado |
| `PA11` | SPIC_DATA3 | reservado |

> El firmware se ejecuta **en el sitio (XIP)** desde la flash SPI. Reconfigurar `PA6` como GPIO le
> quita el *chip-select* a la flash → la CPU lee basura → `UNDEFINSTR`. **No es un bug del sketch: es
> física del chip.**

Utilizables sin problema: `PA0, PA5, PA12, PA14, PA15, PA18, PA19, PA22, PA23, PA29, PA30`.

---

## Las dos UART

| UART | Pines | Objeto | Uso | Baud |
|---|---|---|---|---|
| **UART2** | `PA30` TX / `PA29` RX | **`Serial`** | Consola / LOG / flasheo | 9600 (app) · 115200 (ROM) · 1.5 M (descarga) |
| **UART0** | `PA23` TX / `PA18` RX + `PA19` DE | **`Serial0`** | **RS485 del inversor** (Modbus RTU) | **9600 8N1** |

> En este chip `Serial` **es UART2** (`LT_UART_DEFAULT_PORT = 2`) — curioso, pero es así. El banner
> de arranque sale por `Serial`, y por eso aparece en la consola de `ltchiptool`.

---

## Transceptor RS485 y conector

- **CA-IF4805HS** (Chipanalog), RS485 *half-duplex*, SOP-8/SOIC-8: `1=RO`, `2=/RE`, `3=DE`, `4=DI`,
  `6=A`, `7=B`, `8=VCC`.
- **DE y /RE están unidos** (continuidad verificada) → `PA19` controla transmisión y recepción a la
  vez. **Consecuencia**: mientras `PA19` está HIGH, el receptor está **apagado**. Hay que bajar `PA19`
  **inmediatamente** tras vaciar el FIFO de TX para poder oír la respuesta.
- Con el bus en reposo, `RO = 3,297 V` → receptor habilitado y bus en *mark* (polaridad correcta).
- Conector del inversor = **COM Port** (Exceedconn de 4 pines): **1 = VCC 5 V**, **2 = GND**,
  **3 = RS485 A**, **4 = RS485 B**.

> Matiz importante: `PA19` **no** se usa para un `send_wait_time` ni similar. El inversor responde
> ~300 ms después; el DE debe estar ya en LOW para escucharlo.

---

## Watchdog hardware (`PA00`) — imprescindible

Las placas S3 fabricadas **desde finales de 2022** llevan un IC *watchdog* externo conectado a
`CHIP_EN`. Si no se le "da de comer", **resetea el MCU cada ~15 minutos**. Se alimenta con un
**pulso HIGH de ~100 ms en `PA00`** (cada pocos segundos o minutos).

- En placas **antiguas** (2021) `PA00` no se usa → pulsarlo es inofensivo.
- **Síntoma si falta**: el stick "reinicia" solo cada ~15 min (el banner de arranque se repite).
- En este firmware el *kick* se hace **cada 5 s**, **independiente** de que haya cliente conectado
  (lección del issue #85 de `ginlong-solis`: no atarlo a `api.connected`, o si HA se cae el stick
  entra en bucle de reinicios).

---

## Alimentación y flasheo

- Alimentación: **5 V** (el propio inversor se los da por el *COM Port*); si lo pruebas fuera,
  usa una fuente de **≥ 500 mA**.
- **UART boot mode** (solo la primera vez, para flashear): poner el MCU en modo descarga
  (p. ej. **pull TX low durante el arranque**) y usar un adaptador **USB-serie a 3,3 V**.
- ⚠️ **Nunca** alimentes el stick desde el pin 3,3 V de un FT232RL (~50 mA): no da.
- Se graba con **`ltchiptool flash write -d <puerto> firmware.uf2`** (el `.uf2` ya lleva las
  direcciones; no se pasa `-f`/`-s`).
- **Antes de tocar nada:** `ltchiptool flash read -d <puerto> RTL8710B backup.bin` (**8 MiB**).
