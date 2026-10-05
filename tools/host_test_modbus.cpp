/*
 * host_test_modbus.cpp — test ANFITRIÓN (g++, sin hardware) de la lógica Modbus
 * del puente: CRC16, parseo/construcción de MBAP y conversión RTU -> MBAP.
 *
 * Compilar/ejecutar (desde la raíz del proyecto):
 *     g++ -std=c++17 -O2 -Wall -Wextra -Isrc tools/host_test_modbus.cpp -o /tmp/host_test_modbus \
 *       && /tmp/host_test_modbus
 *
 * No sustituye a la prueba en hardware (RS485, tiempos, LwIP), pero garantiza que el
 * framing y el CRC son correctos ANTES de flashear.
 */
#include "modbus_proto.h"

#include <cstdio>
#include <cstring>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg)                        \
  do {                                          \
    if (cond) {                                 \
      g_pass++;                                 \
    } else {                                    \
      g_fail++;                                 \
      printf("  [FALLO] %s (linea %d)\n", msg, __LINE__); \
    }                                           \
  } while (0)

static void dump(const char *tag, const uint8_t *d, size_t n) {
  printf("  %-22s:", tag);
  for (size_t i = 0; i < n; i++) printf(" %02X", d[i]);
  printf("   (%zu bytes)\n", n);
}

// ---------------------------------------------------------------------------
// [1] CRC16 Modbus
// ---------------------------------------------------------------------------
static void testCrc() {
  printf("[1] CRC16 Modbus\n");

  // Vector autoritativo de la especificacion (catalogo CRC-16/MODBUS).
  const char *s = "123456789";
  uint16_t c = mbp::crc16((const uint8_t *)s, 9);
  printf("  crc16(\"123456789\") = 0x%04X  (esperado 0x4B37)\n", c);
  CHECK(c == 0x4B37, "vector CRC-16/MODBUS de '123456789'");

  // Trama de peticion RTU real: 01 03 81 EF 00 02 + CRC (lo,hi)
  uint8_t f[] = {0x01, 0x03, 0x81, 0xEF, 0x00, 0x02};
  uint16_t cf = mbp::crc16(f, sizeof(f));
  uint8_t frame[8];
  memcpy(frame, f, 6);
  mbp::rtuAppendCrc(frame, 6);
  dump("trama RTU con CRC", frame, 8);
  CHECK(frame[6] == (cf & 0xFF) && frame[7] == (cf >> 8), "orden del CRC (byte bajo, byte alto)");
  CHECK(mbp::rtuCrcOk(frame, 8), "rtuCrcOk acepta la trama valida");

  frame[3] ^= 0xFF;
  CHECK(!mbp::rtuCrcOk(frame, 8), "rtuCrcOk rechaza trama corrupta");
}

// ---------------------------------------------------------------------------
// [2] Parseo y construccion de cabecera MBAP
// ---------------------------------------------------------------------------
static void testMbap() {
  printf("[2] Cabecera MBAP\n");

  const uint8_t req[] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x01,
                         0x03, 0x81, 0xEF, 0x00, 0x02};
  mbp::Mbap h;
  CHECK(mbp::mbapParse(req, h), "mbapParse acepta la peticion");
  CHECK(h.txn == 1, "txn id");
  CHECK(h.proto == 0, "proto id == 0");
  CHECK(h.len == 6, "len == 6");
  CHECK(h.unit == 1, "unit id");
  CHECK(mbp::mbapTotalLen(h) == 12, "longitud total de la trama == 12");

  uint8_t bad[] = {0x00, 0x01, 0x00, 0x09, 0x00, 0x06, 0x01};
  CHECK(!mbp::mbapParse(bad, h), "mbapParse rechaza proto id != 0");

  uint8_t out[16];
  mbp::mbapBuild(out, h, 0x11, 5);
  const uint8_t exp[] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x11};
  CHECK(memcmp(out, exp, sizeof(exp)) == 0, "mbapBuild escribe la cabecera correcta");
}

// ---------------------------------------------------------------------------
// Esclavo Modbus RTU simulado (para probar la conversion de ida y vuelta).
//   FC3 -> 4 bytes fijos 0x1234 0xABCD
//   FC6 -> eco de direccion+valor
//   otra -> excepcion 0x01 (funcion ilegal)
// ---------------------------------------------------------------------------
static size_t simSlave(const uint8_t *req, size_t reqLen, uint8_t *resp) {
  if (reqLen < 4 || !mbp::rtuCrcOk(req, reqLen)) return 0;
  uint8_t unit = req[0], fc = req[1];
  size_t n = 0;
  resp[n++] = unit;
  if (fc == 0x03) {
    resp[n++] = fc;
    resp[n++] = 4;
    resp[n++] = 0x12; resp[n++] = 0x34; resp[n++] = 0xAB; resp[n++] = 0xCD;
  } else if (fc == 0x06) {
    resp[n++] = fc;
    resp[n++] = req[2]; resp[n++] = req[3];
    resp[n++] = req[4]; resp[n++] = req[5];
  } else {
    resp[n++] = fc | 0x80;
    resp[n++] = 0x01;
  }
  mbp::rtuAppendCrc(resp, n);
  return n + 2;
}

// ---------------------------------------------------------------------------
// [3] Conversion completa MBAP -> RTU -> MBAP
// ---------------------------------------------------------------------------
static void testBridge() {
  printf("[3] Puente MBAP <-> RTU (FC3)\n");

  // Peticion: evcc lee 33263 (0x81EF) potencia de red, 2 registros, unit 1.
  const uint8_t req[] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x01,
                         0x03, 0x81, 0xEF, 0x00, 0x02};
  const size_t reqLen = sizeof(req);

  // Construir la trama RTU equivalente (unit + PDU + CRC) y pasarla al esclavo simulado.
  uint8_t rtureq[mbp::PDU_MAX + 3];
  rtureq[0] = req[6];
  memcpy(rtureq + 1, req + 7, reqLen - 7);
  mbp::rtuAppendCrc(rtureq, 1 + (reqLen - 7));
  size_t rqLen = 1 + (reqLen - 7) + 2;
  dump("peticion RTU", rtureq, rqLen);

  uint8_t rturesp[mbp::PDU_MAX + 3];
  size_t rn = simSlave(rtureq, rqLen, rturesp);
  dump("respuesta RTU", rturesp, rn);
  CHECK(rn == 9, "respuesta RTU de 9 bytes (unit+fc+bytecount+4dato+crc2)");

  uint8_t out[mbp::MBAP_HDR + mbp::PDU_MAX + 1];
  size_t on = mbp::buildMbapFromRtu(req, rturesp, rn, out, sizeof(out));
  dump("respuesta MBAP", out, on);

  // PDU = 03 04 12 34 AB CD (6 bytes) -> MBAP len = 1 + 6 = 7 -> trama total 13 bytes.
  const uint8_t exp[] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x07, 0x01,
                         0x03, 0x04, 0x12, 0x34, 0xAB, 0xCD};
  CHECK(on == sizeof(exp), "longitud de la respuesta MBAP (13)");
  CHECK(on == sizeof(exp) && memcmp(out, exp, on) == 0, "bytes exactos de la respuesta MBAP");
}

// ---------------------------------------------------------------------------
// [4] FC6 (escritura de un registro) y excepcion
// ---------------------------------------------------------------------------
static void testFc6AndException() {
  printf("[4] FC6 y excepcion\n");

  // FC6: escribir 0x4322=1 en el registro 43110 (0xA866) -> peticion MBAP
  const uint8_t req[] = {0x00, 0x2A, 0x00, 0x00, 0x00, 0x06, 0x01,
                         0x06, 0xA8, 0x66, 0x00, 0x01};
  uint8_t rtureq[mbp::PDU_MAX + 3];
  rtureq[0] = req[6];
  memcpy(rtureq + 1, req + 7, 5);
  mbp::rtuAppendCrc(rtureq, 6);

  uint8_t rturesp[mbp::PDU_MAX + 3];
  size_t rn = simSlave(rtureq, 8, rturesp);
  uint8_t out[mbp::MBAP_HDR + mbp::PDU_MAX + 1];
  size_t on = mbp::buildMbapFromRtu(req, rturesp, rn, out, sizeof(out));
  dump("FC6 respuesta MBAP", out, on);
  const uint8_t exp[] = {0x00, 0x2A, 0x00, 0x00, 0x00, 0x06, 0x01,
                         0x06, 0xA8, 0x66, 0x00, 0x01};
  CHECK(on == sizeof(exp) && memcmp(out, exp, on) == 0, "eco FC6 correcto");

  // Cuando el inversor no responde -> excepcion 0x0B con el mismo txn/unit.
  uint8_t ex[mbp::MBAP_HDR + 4];
  size_t xn = mbp::buildMbapException(req, 0x06, mbp::EX_GATEWAY_TARGET, ex, sizeof(ex));
  dump("excepcion MBAP", ex, xn);
  const uint8_t expe[] = {0x00, 0x2A, 0x00, 0x00, 0x00, 0x03, 0x01, 0x86, 0x0B};
  CHECK(xn == sizeof(expe) && memcmp(ex, expe, xn) == 0, "excepcion 0x0B exacta");
}

int main() {
  printf("=== Test anfitrion de la logica Modbus (sin hardware) ===\n");
  testCrc();
  testMbap();
  testBridge();
  testFc6AndException();
  printf("\n=== Resultado: %d OK, %d FALLOS ===\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
