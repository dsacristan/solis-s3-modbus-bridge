/*
 * modbus_proto.h — utilidades PURAS del protocolo Modbus (sin dependencias de Arduino).
 *
 * Se usa desde:
 *   - src/main.cpp              (firmware del stick S3-WIFI-ST)
 *   - tools/host_test_modbus.cpp (test anfitrión con g++, sin hardware)
 *
 * Al no incluir <Arduino.h> se puede compilar en el PC y verificar en serio el
 * CRC16 y el framing MBAP <-> RTU antes de flashear.
 *
 * Convención de buffers: nunca se reserva memoria dinámica aquí; todo son funciones
 * inline sobre buffers que pasa el llamante.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mbp {

// Puertos de escucha por defecto.
static const uint16_t PORT_TCP = 502;   // Modbus TCP estándar (MBAP)
static const uint16_t PORT_RTU = 5020;  // Modbus RTU sobre TCP (tramas crudas)

static const size_t PDU_MAX  = 253;  // PDU máxima del protocolo Modbus
static const size_t MBAP_HDR = 7;    // cabecera MBAP = txn(2)+proto(2)+len(2)+unit(1)

// Códigos de excepción Modbus que usa el puente.
static const uint8_t EX_ILLEGAL_FUNCTION = 0x01;
static const uint8_t EX_ILLEGAL_DATA     = 0x03;
static const uint8_t EX_GATEWAY_TARGET   = 0x0B;  // gateway target failed to respond

// ---------------------------------------------------------------------------
// CRC16 Modbus (polinomio 0xA001 reflejado, init 0xFFFF).
// ---------------------------------------------------------------------------
inline uint16_t crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i];
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
  }
  return crc;
}

// Añade el CRC al final de la parte de datos: frame[len]=bajo, frame[len+1]=alto.
inline void rtuAppendCrc(uint8_t *frame, size_t len) {
  uint16_t c = crc16(frame, len);
  frame[len]     = (uint8_t)(c & 0xFF);
  frame[len + 1] = (uint8_t)(c >> 8);
}

// Valida el CRC de una trama RTU completa (len INCLUYE los 2 bytes de CRC).
inline bool rtuCrcOk(const uint8_t *frame, size_t len) {
  if (len < 4) return false;
  uint16_t calc = crc16(frame, len - 2);
  uint16_t rx   = (uint16_t)frame[len - 2] | ((uint16_t)frame[len - 1] << 8);
  return calc == rx;
}

// ---------------------------------------------------------------------------
// Cabecera MBAP (Modbus TCP)
// ---------------------------------------------------------------------------
struct Mbap {
  uint16_t txn;    // transaction id (se refleja en la respuesta)
  uint16_t proto;  // protocol id (debe ser 0)
  uint16_t len;    // nº de bytes que siguen a este campo = 1 (unit) + PDU
  uint8_t  unit;   // unit id (esclavo)
};

// Parsea los 7 bytes de cabecera MBAP. false si proto!=0 o len absurdo.
inline bool mbapParse(const uint8_t *b, Mbap &h) {
  h.txn   = (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
  h.proto = (uint16_t)(((uint16_t)b[2] << 8) | b[3]);
  h.len   = (uint16_t)(((uint16_t)b[4] << 8) | b[5]);
  h.unit  = b[6];
  return h.proto == 0 && h.len >= 2 && h.len <= (PDU_MAX + 1);
}

// Longitud total de una trama MBAP (cabecera + PDU) = 6 + len.
inline size_t mbapTotalLen(const Mbap &h) {
  return (size_t)6 + h.len;
}

// Escribe una cabecera MBAP de respuesta (unit dado, PDU de pduLen bytes).
inline void mbapBuild(uint8_t *out, const Mbap &req, uint8_t unit, size_t pduLen) {
  out[0] = (uint8_t)(req.txn >> 8);
  out[1] = (uint8_t)(req.txn & 0xFF);
  out[2] = 0;
  out[3] = 0;
  uint16_t len = (uint16_t)(1 + pduLen);
  out[4] = (uint8_t)(len >> 8);
  out[5] = (uint8_t)(len & 0xFF);
  out[6] = unit;
}

// ---------------------------------------------------------------------------
// Conversión respuesta RTU -> respuesta MBAP
// ---------------------------------------------------------------------------

// Construye la respuesta MBAP a partir de la respuesta RTU del inversor.
//   req     -> cabecera MBAP de la petición (7 bytes)
//   rtuResp -> [unit][fc][data...][crc lo][crc hi]  (rtuLen INCLUYE el CRC)
// Devuelve la longitud total escrita en 'out' (cabecera + PDU) o 0 si no es válido.
inline size_t buildMbapFromRtu(const uint8_t *req, const uint8_t *rtuResp, size_t rtuLen,
                               uint8_t *out, size_t outMax) {
  if (rtuLen < 4) return 0;
  Mbap h;
  if (!mbapParse(req, h)) return 0;
  size_t payload = rtuLen - 2;  // unit + PDU (sin CRC)
  if (payload < 1) return 0;
  size_t pduLen = payload - 1;  // PDU sin unit
  if (pduLen < 1 || pduLen > PDU_MAX) return 0;
  size_t total = MBAP_HDR + pduLen;
  if (total > outMax) return 0;
  mbapBuild(out, h, rtuResp[0], pduLen);
  memcpy(out + MBAP_HDR, rtuResp + 1, pduLen);
  return total;
}

// Construye una respuesta de excepción MBAP (PDU de 2 bytes: fc|0x80, code).
inline size_t buildMbapException(const uint8_t *req, uint8_t fc, uint8_t code,
                                 uint8_t *out, size_t outMax) {
  Mbap h;
  if (!mbapParse(req, h)) return 0;
  if (outMax < MBAP_HDR + 2) return 0;
  mbapBuild(out, h, h.unit, 2);
  out[MBAP_HDR]     = (uint8_t)(fc | 0x80);
  out[MBAP_HDR + 1] = code;
  return MBAP_HDR + 2;
}

}  // namespace mbp
