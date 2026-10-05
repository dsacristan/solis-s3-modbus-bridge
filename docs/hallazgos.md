# Hallazgos: qué fallaba y por qué (bitácora de la depuración)

Resumen de semanas de depuración. **Lo más valioso de este repositorio** son las dos trampas y la
metodología que permitió encontrarlas.

---

## Las dos causas raíz

El síntoma era siempre el mismo: **el inversor no respondía** por RS485 (silencios, `rx = 0`),
aunque el datalogger **original** sí leía. Probamos bauds, unit-ids, pines, polaridad A/B, CRC,
tiempos de DE/RE… todo. La causa no era nada de eso.

### 1. Versión de ESPHome (para el firmware de referencia)

| ESPHome | ¿Compila? | ¿Lee Modbus? | API/OTA |
|---|---|---|---|
| 2025.7–2025.11.2 | ❌ | — | — |
| **2026.1.x → 2026.1.5** | ✅ | ✅ **SÍ (verificado en HW)** | ✅ |
| 2026.2.x – 2026.6.x (**2026.6.5**) | ✅ | ❌ **no lee** | ❌ **resetean la conexión** |
| 2026.3.x | ❌ | — | — |
| 2026.5+ | — | — | pierde WiFi |
| 2026.9.0 | reportado ✅ | reportado ✅ | ⚠️ aún no en PyPI |

Con **2026.6.5** el firmware arrancaba, HA conectaba… y **todos los sensores Modbus salían
`unknown`**; además su API y su servidor OTA **cerraban cada conexión**. Con **2026.1.5** empezó a
leer datos reales (AC 241,6 V, SoC, potencia de batería, energía…). Fuente: **issue #72** de
`hn/ginlong-solis`.

> ⚠️ Trampa: si OTA-eas a la franja rota (2026.2–2026.6), su servidor OTA también está roto → **no
> puedes volver por OTA**; sólo por serie.

### 2. Versión de LibreTiny (para el puente propio) — **el bug inédito**

El puente propio (Arduino/LibreTiny) **también** estaba mudo, y aquí la causa era distinta:

> **LibreTiny 1.13.0 rompe el RX del UART0** en el RTL8710BN. El **TX funciona perfectamente**
> (las tramas salían con CRC correcto), pero el UART **no recibe ni un byte**.

Con **`platform = libretiny@1.10.0`** (la que usa el build ESPHome que sí leía):

```
/raw FC4/35000 -> TX: 01 04 88 b8 00 01 9a 4f   RX: 01 04 02 20 30 a0 e4   crc_ok=true
:502  FC4/35000 -> 0x2030 · FC4/33139 -> 0x0050 (SoC) · FC4/33049 -> 0x02ED (749)
```

Detalle útil: en 1.13.0 `Serial::begin()` **sí** acepta pines (4 argumentos); en 1.10.0 **no**
(sólo `begin(baud, config)` → usa los del *variant*: UART0 = RX PA18 / TX PA23). Pero incluso
llamando con 2 argumentos, con 1.13.0 el RX seguía mudo → **es el driver del core, no los pines**.

> 🛠️ Al instalar `libretiny@1.10.0` hay que aplicarle `scripts/patch-libretiny-libs-queue.py`
> (si no, el build falla con el bug de `@dataclass`/SCons de Python 3.12+).

### 3. (Operativa) Cambiar de firmware borra la WiFi del puente

Ambos firmwares guardan sus datos en la **misma partición `kvs`**. Si pasas de ESPHome al puente (o
al revés), el puente arranca sin configuración válida → **modo AP `SolisS3-Setup`** y hay que
volver a darle la WiFi. *El puente, además, sirve `/update` en modo AP*, así que se puede reflashear
sin cable desde su propia red.

---

## Pistas falsas (y por qué lo eran)

| Hipótesis | Veredicto |
|---|---|
| **Baud** incorrecto (4800/9600/19200/38400/57600) | ❌ El inversor va a **9600**. Barridos completos, todo silencio. |
| **Unit id** distinto (barrido 1–20, 100/101/247) | ❌ Es **1**. (El `rs485ComAddr: 101` de SolisCloud era engañoso.) |
| **FC3 vs FC4** | ⚠️ Parcialmente cierto y **necesario**: el ESINV usa **FC4** (`read: input`). Pero no bastaba. |
| **Polaridad A/B** invertida | ❌ Descartado: el reposo era *mark* y el RO daba 3,297 V. |
| **CRC mal calculado** | ❌ **Verificado byte a byte**: `01 04 88 b8 00 01` → CRC `9a 4f` = `0x4F9A` (correcto). |
| **DE/RE mal temporizado** | ❌ Se confundió `send_wait_time: 300ms` de ESPHome con el *hold* del DE. **No lo es**: es el *gap mínimo entre envíos* (`modbus.cpp:44`); el DE se conmuta sólo alrededor de la escritura. Cambiarlo no arregló nada. |
| **Pin RX equivocado** (PA29 vs PA18) | ❌ Fue una hipótesis plausible, pero **la causa estaba en el core**, no en el pin. |
| **Transceptor averiado** | ❌ El **CA-IF4805HS** está bien (DE//RE unidos, RO = 3,297 V, A−B oscila al transmitir). |
| **Inversor "sólo habla AliOS"** | ❌ **Falso.** Fue la conclusión errónea de una sesión anterior; se corrigió con los datos. |

---

## Metodología que sí funcionó

1. **Test de host sin hardware.** `tools/host_test_modbus.cpp` (g++) valida MBAP, CRC-16/Modbus y la
   conversión completa MBAP→RTU→MBAP contra un esclavo simulado. 17 asserts en verde.
2. **Endpoint `/raw` de diagnóstico en vivo.** Sin reflashear: enviar una trama arbitraria por RS485
   y devolver **los bytes TX y RX reales** + `crc_ok`. Con esto se descartó el CRC *de verdad* y se
   vio que el TX salía perfecto mientras el RX estaba a cero.
3. **Comparar con un firmware de referencia que funciona.** El ESPHome de `hn/ginlong-solis` sobre
   el **mismo** hardware sirvió de "oráculo": si él lee y nosotros no, la diferencia está en nuestro
   *software*. Y al separar las diferencias, apareció la versión de LibreTiny.
4. **Verificación del artefacto antes de flashear.** `tools/verify_uf2.py` reconstruye la imagen
   desde el UF2 y comprueba cabecera, familia y firmas.
5. **OTA por web** para iterar sin cable (y `/update` también en modo AP, para no quedarse bloqueado).

## Hallazgos de hardware menores pero críticos

- **PA6–PA11 son la FLASH SPI (XIP)**. Tocar `PA6` (SPIC_CS) = **Hard Fault instantáneo** (la CPU
  ejecuta desde esa flash). Costó una tarde diagnosticarlo con `addr2line`/`objdump` sobre el ELF.
- **Watchdog hardware en `PA00`**: las placas S3 desde finales de 2022 llevan un IC *watchdog*
  conectado a `CHIP_EN` que **resetea el MCU cada ~15 min** si no se le da de comer. Se alimenta con
  un pulso HIGH de ~100 ms. (En placas antiguas `PA00` no se usa; pulsarlo es inofensivo.)
- **`PA29` no es el RX del RS485**: es el **RX de la consola** (UART2). El RS485 es **UART0**
  (`PA23` TX / `PA18` RX) con **`PA19` = DE/RE**.
- **OTA/API de ESPHome por túnel SSH**: **no funciona** (el stick resetea el handshake). Hay que
  hacerlo **desde la misma LAN**.
