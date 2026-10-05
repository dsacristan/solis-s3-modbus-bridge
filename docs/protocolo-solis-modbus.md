# Protocolo Modbus del inversor Solis (familia ESINV)

Lo que hay que saber para leer un inversor **híbrido con batería** (familia **ESINV**, p. ej.
**S5-EH1P4.6K-L**). El error clásico es tratar de leerlo como un INV estándar.

## Parámetros del enlace

| Parámetro | Valor |
|---|---|
| Baud | **9600** |
| Formato | **8N1** (8 bits, sin paridad, 1 stop) |
| *Slave address* | **1** (⚠️ el `rs485ComAddr: 101` que ves en SolisCloud **no** sirve) |
| Puerto físico | **COM Port** del inversor (4 pines: VCC, GND, A, B) |
| Tiempo de respuesta | ~300 ms tras el último byte de la petición |

> La configuración del inversor (9600 · none · 8 · 1 · device 01) se puede ver en su menú, sección
> *internal communication*.

## Dos mapas de registros (¡y dos funciones distintas!)

| Mapa | Familia | Direcciones | Función Modbus | Offset |
|---|---|---|---|---|
| **INV** | estándar (p. ej. RHI) | `3xxx` (2999–3071) | **FC3** (holding) | **−1** |
| **ESINV** | **híbrida con batería** | `3xxxx` (33000–33165, 35000, 4xxxx) | **FC4** (input) | **sin offset** |
| EPM | gestor de exportación | `36xxx` | — | — |

**Regla práctica:**

- Un **S5-EH1P es ESINV** → sus datos de operación (`33049` tensión DC, `33057` potencia DC total,
  `33139` SoC de batería, `35000` tipo de inversor…) se leen con **FC4 (input registers, `0x04`)**.
- Mandar **FC3** a un registro ESINV → el inversor **no responde** (silencio). Esto nos costó el
  "silencio total".
- El **registro `35000`** ("inverter type definition") existe en los dos mapas, es de lectura y
  devuelve el tipo: en nuestro S5-EH1P respondió **`0x2030`**. Es la forma de autodetectar el mapa.

## Funciones Modbus (según el protocolo del fabricante)

| Función | Uso |
|---|---|
| `02H` | leer estado / faltas (registros `1xxxx`) |
| `03H` | leer *holding* (`4xxxx`) |
| **`04H`** | **leer datos de operación (`3xxxx`)** ← lo que usamos |
| `06H` / `10H` | escribir |

## Trama y CRC

- **RTU**: `[unit][función][datos…][CRC_LOW][CRC_HIGH]`.
- **CRC-16/MODBUS**: polinomio `0xA001` (reflejado, init `0xFFFF`). Valor de comprobación del
  estándar para `"123456789"` = **`0x4B37`**. En el cable va **byte bajo primero**.
  - Verificado en este proyecto: `01 04 88 B8 00 01` → CRC **`0x4F9A`** → se envía `… 9a 4f`.
- **TCP (MBAP)**: cabecera de 7 bytes `[trans_id][proto=0][len][unit]` + PDU.

## Cómo lo expone el puente

El puente **no filtra la función**: reenvía la PDU tal cual al RS485. Así sirve a cualquier cliente
(evcc, HA, `mbpoll`…) sin recompilar, y con la función que el cliente elija:

| Puerto | Protocolo | Config en el cliente |
|---|---|---|
| **502** | Modbus TCP (MBAP) | evcc `modbus: tcpip` · HA `type: tcp` |
| **5020** | RTU crudo sobre TCP | evcc `rs485tcpip` · HA `type: rtuovertcp` |

> **El cliente debe usar FC4** (`read: input`) para los datos ESINV. Si usa FC3, el inversor guarda
> silencio y el puente devuelve la **excepción `0x0B`** (*gateway target failed to respond*).

### Diagnóstico rápido

```bash
# Modbus TCP, FC4, unit 1, registro 35000  ->  debe responder 0x2030
python3 tools/mbtest_tcp.py <IP-del-stick> 502 1 35000 4 1

# Ver los bytes crudos TX/RX por RS485 (sin reflashear)
curl "http://<IP-del-stick>/raw?hex=010488B80001&crc=1"
# -> {"tx":"010488b800019a4f","rx":"0104022030a0e4","n":7,"crc_ok":true}
```

| Lectura | Significado |
|---|---|
| `n = 0` en `/raw` | No llega ni un byte: revisa el **RX del core** (LibreTiny 1.10.0), el cableado A/B o el baud |
| `crc_ok = false` con `n > 0` | Llegan bytes pero corruptos: polaridad A/B o baud |
| Excepción `0x0B` en :502 | El inversor no contesta a esa función/registro (¿FC3 en vez de FC4?) |
