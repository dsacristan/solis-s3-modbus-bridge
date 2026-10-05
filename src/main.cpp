/*
 * S3-WIFI-ST  ->  servidor Modbus local para inversor Solis S5-EH1P4.6K-L
 *
 * FASE 1.5 + FASE 3 (este archivo):
 *   - WiFi STA con DHCP (config guardada en flash, partición kvs).
 *   - Servidor HTTP en :80:
 *       GET /            -> dashboard HTML (modo normal) o portal config (modo AP)
 *       GET /api/status  -> JSON para HA / evcc (solo modo normal)
 *   - MODO CONFIGURACIÓN (FASE 3): manteniendo el botón PA08 >10 s, el stick
 *     reinicia y crea un AP abierto "SolisS3-Setup" (192.168.4.1) con portal
 *     cautivo: escaneo de redes + formulario SSID/pass + guardar en flash.
 *
 *   - LED NET verde  = estado WiFi (fijo=conectado, parpadeo=sin red / modo config)
 *     LED COM naranja = destello en cada petición HTTP o trama Modbus
 *   - Kick del watchdog hardware (PA00) y UART RS485 (UART0) conservados.
 *
 * FASE 2 (este archivo): PUENTE MODBUS.
 *   - :502  -> servidor Modbus TCP estándar (cabecera MBAP). Es el que usan
 *              evcc (`modbus: tcpip`) y la integración `modbus` de HA (`type: tcp`).
 *   - :5020 -> Modbus RTU sobre TCP (tramas crudas, sin MBAP). Para clientes
 *              tipo `rs485tcpip` (evcc fase A) o HA `type: rtuovertcp`.
 *   Ambos reenvían la petición por RS485 al inversor y devuelven su respuesta.
 *   El manejo de sockets es NO bloqueante (accept + sondeo de `available()`),
 *   así el bucle sigue atendiendo HTTP, la reconexión WiFi y el watchdog.
 *
 * ── MAPA REAL DE UARTs (verificado en el core LibreTiny de ESTE build) ─────────
 *   `Serial`  = UART2 = PA30 TX / PA29 RX  (log/descarga; el mismo de ltchiptool)
 *   `Serial0` = UART0 = PA23 TX / PA18 RX  (RS485 inversor)
 *
 * ⚠️ PA6..PA11 = FLASH SPI (XIP): NO usar como GPIO (Hard Fault instantáneo).
 * Nombres de pin: PIN_PAxx (NO "PAxx").
 */

#include <Arduino.h>
#include <Update.h>   // OTA por web (LibreTiny: envuelve lt_ota_* / uf2ota)
#include <WiFi.h>        // trae WiFiServer + WiFiClient (LwIP)
#include <WebServer.h>

#include "modbus_proto.h"  // CRC16 + MBAP <-> RTU (host-testeable, sin Arduino)

// Reset fiable del MCU. `lt_reboot()` en realtek-ambz cae en la versión débil
// "watchdog way" (poco fiable). El SDK sí expone sys_reset() (SCB->AIRCR SYSRESETREQ),
// el mismo que usa lt_reboot_download_mode(). Verificado en sys_api.c del SDK.
extern "C" void sys_reset(void);
static inline void doReboot() {
  sys_reset();
  while (1) {}
}

// --- Pinout verificado del stick S3 (macros LibreTiny) ---
static const uint8_t PIN_RS485_TX = PIN_PA23;  // UART0 TX (DI)
static const uint8_t PIN_RS485_RX = PIN_PA18;  // UART0 RX (RO) — pinout real (= ESPHome)
static const uint8_t PIN_RS485_DE = PIN_PA19;
static const uint8_t PIN_LED_COM  = PIN_PA12;  // LED naranja (actividad HTTP)
static const uint8_t PIN_LED_WIFI = PIN_PA05;  // LED verde   (estado WiFi)
static const uint8_t PIN_WDT      = PIN_PA00;  // kick del watchdog hardware
static const uint8_t PIN_BOTON    = PIN_PA08;  // botón reset (activo-LOW)

// --- Sondeo de pines GPIO para localizar el RO del transceptor RS485 ---
// El RO conduce HIGH (3,3 V) en reposo (bus en mark). Leemos los pines libres
// como INPUT y los exponemos en el dashboard: el que lea 1 es el RO.
static const uint8_t PROBE_PINS[] = { PIN_PA00, PIN_PA05, PIN_PA08, PIN_PA12, PIN_PA14, PIN_PA15, PIN_PA18, PIN_PA19, PIN_PA22, PIN_PA23, PIN_PA29, PIN_PA30 };
static const char  *PROBE_NAMES[] = { "PA00", "PA05", "PA08", "PA12", "PA14", "PA15", "PA18", "PA19", "PA22", "PA23", "PA29", "PA30" };
#define PROBE_COUNT 12
static int g_probeValues[PROBE_COUNT];

// Sondea todos los pines GPIO accesibles como INPUT (sin pull-up) y guarda su
// nivel lógico. Debe ejecutarse ANTES de configurar las UARTs (que reasignan pines).
static void doProbe() {
  for (uint8_t i = 0; i < PROBE_COUNT; i++) pinMode(PROBE_PINS[i], INPUT);
  delay(10);
  for (uint8_t i = 0; i < PROBE_COUNT; i++) g_probeValues[i] = digitalRead(PROBE_PINS[i]);
}

// --- Red de trabajo por defecto (solo como fallback; lo normal es leer flash) ---
static const uint16_t HTTP_PORT = 80;

// --- Modo configuración (AP) ---
static const char *AP_SSID = "SolisS3-Setup";
static const IPAddress AP_IP(192, 168, 4, 1);

// --- Parámetros del enlace al inversor ---
static uint32_t g_rs485Baud = 9600;   // baud del RS485 al inversor (cambiable desde el dashboard)
static const uint32_t DEBUG_BAUD    = 9600;
static uint8_t  g_modbusUnit  = 0x01; // unit/device Modbus del inversor (cambiable)
static const uint32_t WDT_KICK_MS   = 5000;
static const uint32_t RECONNECT_MS  = 30000;
static const uint32_t LONG_PRESS_MS = 10000;  // >10 s -> modo configuración

// --- Puente Modbus (FASE 2) ---
static const uint16_t MODBUS_TCP_PORT = mbp::PORT_TCP;  // 502  (Modbus TCP / MBAP)
static const uint16_t MODBUS_RTU_PORT = mbp::PORT_RTU;  // 5020 (Modbus RTU crudo)
static const uint32_t RS485_FIRST_BYTE_MS = 1000;  // espera al 1er byte (Solis responde ~300ms+)
static const uint32_t RS485_INTERBYTE_MS  = 15;    // fin de trama RS485: gap > 15 ms
static const uint32_t RTU_SILENCE_MS      = 15;    // delimita tramas en el puerto RTU crudo
static const uint32_t MODBUS_IDLE_MS      = 3000;  // descarta tramas parciales tras este silencio
static const uint32_t MODBUS_ALIVE_MS     = 30000; // ventana para considerar "vivo" el puente
static const size_t   MODBUS_BUF_SZ       = mbp::MBAP_HDR + mbp::PDU_MAX + 1;  // 261 B

// --- Config persistente en flash (partición kvs, FLASH_KVS_OFFSET) ---
#define CFG_MAGIC    0x534F4C33UL  // "SOL3"
#define CFG_VERSION  1
#define CFG_FLAG_FORCE_AP 0x01     // bit0: entrar en AP en el próximo arranque

struct __attribute__((packed)) Config {
  uint32_t magic;
  uint32_t version;
  uint32_t flags;
  char ssid[33];
  char pass[65];
  uint32_t crc;
};

// --- Estado ---
enum Mode { MODE_STA, MODE_AP };
static Mode g_mode = MODE_STA;

// --- Versión del firmware (visible en dashboard + /api/status) ---
#define FIRMWARE_VERSION "v18"
#define FIRMWARE_BUILD   (__DATE__ " " __TIME__)

// --- Servidor HTTP + contadores ---
static WebServer server(HTTP_PORT);
static uint32_t g_httpRequests = 0;
static uint32_t g_comLedUntil  = 0;

// --- Puente Modbus (FASE 2): dos servidores + un cliente por puerto ---
// Un único cliente activo por puerto (evcc abre una sola conexión física y
// serializa sus lecturas). Acceso al RS485 siempre en exclusiva (bucle único).
static WiFiServer modbusTcp(MODBUS_TCP_PORT);
static WiFiServer modbusRtu(MODBUS_RTU_PORT);
static WiFiClient g_mbapClient;
static WiFiClient g_rtuClient;
static uint8_t  g_mbapBuf[MODBUS_BUF_SZ];
static size_t   g_mbapLen  = 0;
static uint32_t g_mbapLast = 0;
static uint8_t  g_rtuBuf[MODBUS_BUF_SZ];
static size_t   g_rtuLen   = 0;
static uint32_t g_rtuLast  = 0;
// Contadores (visibles en el dashboard y en /api/status).
static uint32_t g_mbapOk = 0, g_mbapErr = 0, g_mbapDrop = 0;
static uint32_t g_rtuOk  = 0, g_rtuErr  = 0;
static uint32_t g_crcErrors = 0;
static uint32_t g_lastOkMs  = 0;  // millis() de la última trama válida del inversor (0 = nunca)
static uint32_t g_rxRawBytes = 0; // bytes crudos recibidos por RS485 (diagnóstico: >0 => el RX oye algo)

// --- Resultados de escaneo WiFi (modo config) ---
#define MAX_NETWORKS 20
static String netSsid[MAX_NETWORKS];
static int8_t  netRssi[MAX_NETWORKS];
static bool    netSecure[MAX_NETWORKS];
static int     netCount = 0;

// ---------------------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------------------

static void kickWatchdog() {
  digitalWrite(PIN_WDT, HIGH);
  delay(100);
  digitalWrite(PIN_WDT, LOW);
}

static void flashCom() {
  digitalWrite(PIN_LED_COM, HIGH);
  g_comLedUntil = millis() + 120;
}

static String ipToStr(IPAddress ip) {
  String s;
  s.reserve(16);
  s += String(ip[0]); s += F("."); s += String(ip[1]); s += F(".");
  s += String(ip[2]); s += F("."); s += String(ip[3]);
  return s;
}

// CRC32 estándar (polinomio 0xEDB88320), bit a bit.
static uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++)
      crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
}

static uint32_t computeCrc(const Config &cfg) {
  return crc32((const uint8_t *)&cfg, sizeof(Config) - sizeof(cfg.crc));
}

// Lee la config de flash y valida magic+version+crc. Devuelve false si no es válida.
static bool loadConfig(Config &cfg) {
  memset(&cfg, 0, sizeof(cfg));
  lt_flash_read(FLASH_KVS_OFFSET, (uint8_t *)&cfg, sizeof(cfg));
  if (cfg.magic != CFG_MAGIC || cfg.version != CFG_VERSION)
    return false;
  return computeCrc(cfg) == cfg.crc;
}

// Guarda la config en flash (borra un bloque de 4 KB y escribe).
static void saveConfig(const Config &cfg) {
  Config c = cfg;
  c.crc = computeCrc(c);
  lt_flash_erase(FLASH_KVS_OFFSET, 4096);
  lt_flash_write(FLASH_KVS_OFFSET, (const uint8_t *)&c, sizeof(c));
}

// Escapa & < > " ' para incrustar en HTML de forma segura.
static String htmlEscape(const String &s) {
  String r;
  r.reserve(s.length() + 8);
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') r += F("&amp;");
    else if (c == '<') r += F("&lt;");
    else if (c == '>') r += F("&gt;");
    else if (c == '"') r += F("&quot;");
    else if (c == '\'') r += F("&#39;");
    else r += c;
  }
  return r;
}

// ---------------------------------------------------------------------------
// Handlers modo normal (STA): dashboard + JSON
// ---------------------------------------------------------------------------

static String buildDashboard() {
  String h;
  h.reserve(1500);
  bool wifiOk = (WiFi.status() == WL_CONNECTED);

  h += F("<!DOCTYPE html><html lang='es'><head><meta charset='utf-8'>");
  h += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  h += F("<title>S3-WIFI-ST</title><style>");
  h += F("body{font-family:system-ui,sans-serif;max-width:640px;margin:2rem auto;padding:0 1rem;background:#111;color:#e0e0e0}");
  h += F("h1{font-size:1.3rem;border-bottom:2px solid #2a2a2a;padding-bottom:.4rem}");
  h += F(".ok{color:#2ecc71}.bad{color:#e74c3c}");
  h += F("table{width:100%;border-collapse:collapse;margin-top:1rem}");
  h += F("td,th{padding:.45rem .5rem;border-bottom:1px solid #2a2a2a;text-align:left;font-size:.95rem}");
  h += F("td:first-child{color:#888;width:45%}a{color:#5dade2}");
  h += F("</style></head><body>");

  h += F("<h1>Solis S3-WIFI-ST</h1>");
  h += F("<p>Estado WiFi: ");
  if (wifiOk) h += F("<span class='ok'>CONECTADO</span>");
  else        h += F("<span class='bad'>SIN RED</span>");
  h += F("</p>");

  h += F("<table>");
  h += F("<tr><td>Versión</td><td>"); h += FIRMWARE_VERSION; h += F(" · "); h += FIRMWARE_BUILD; h += F("</td></tr>");
  h += F("<tr><td>SSID</td><td>");
  if (wifiOk) h += WiFi.SSID(); else h += F("—");
  h += F("</td></tr>");
  h += F("<tr><td>Dirección IP</td><td>");
  if (wifiOk) h += ipToStr(WiFi.localIP()); else h += F("—");
  h += F("</td></tr>");
  h += F("<tr><td>Puerto web</td><td>");
  h += String(HTTP_PORT);
  h += F("</td></tr>");
  if (wifiOk) {
    h += F("<tr><td>Máscara</td><td>"); h += ipToStr(WiFi.subnetMask()); h += F("</td></tr>");
    h += F("<tr><td>Gateway</td><td>"); h += ipToStr(WiFi.gatewayIP()); h += F("</td></tr>");
    h += F("<tr><td>RSSI</td><td>"); h += String(WiFi.RSSI()); h += F(" dBm</td></tr>");
  }
  h += F("<tr><td>MAC</td><td>"); h += WiFi.macAddress(); h += F("</td></tr>");
  h += F("<tr><td>Uptime</td><td>"); h += String(millis() / 1000); h += F(" s</td></tr>");
  h += F("<tr><td>HTTP servidas</td><td>"); h += String(g_httpRequests); h += F("</td></tr>");
  h += F("<tr><td>RS485 (UART0)</td><td>"); h += String(g_rs485Baud); h += F(" 8N1 · unit "); h += String(g_modbusUnit); h += F("</td></tr>");
  h += F("<tr><td>Probe GPIO</td><td>");
  for (uint8_t i = 0; i < PROBE_COUNT; i++) {
    h += PROBE_NAMES[i]; h += F("="); h += String(g_probeValues[i]);
    if (i + 1 < PROBE_COUNT) h += F(" · ");
  }
  h += F("</td></tr>");
  h += F("<tr><td>Modbus TCP :502</td><td>");
  h += String(g_mbapOk); h += F(" ok · "); h += String(g_mbapErr); h += F(" err");
  if (g_mbapClient) h += F(" · cliente activo");
  h += F("</td></tr>");
  h += F("<tr><td>Modbus RTU :5020</td><td>");
  h += String(g_rtuOk); h += F(" ok · "); h += String(g_rtuErr); h += F(" err");
  if (g_rtuClient) h += F(" · cliente activo");
  h += F("</td></tr>");
  h += F("<tr><td>Errores CRC RS485</td><td>"); h += String(g_crcErrors); h += F("</td></tr>");
  h += F("<tr><td>Bytes RX RS485 (crudos)</td><td>"); h += String(g_rxRawBytes); h += F("</td></tr>");
  h += F("<tr><td>Última trama Modbus</td><td>");
  if (g_lastOkMs) {
    h += F("hace "); h += String((millis() - g_lastOkMs) / 1000); h += F(" s");
  } else {
    h += F("— (aún sin respuesta)");
  }
  h += F("</td></tr>");
  h += F("</table>");

  h += F("<p><a href='/api/status'>/api/status</a> (JSON)</p>");
  h += F("<form method='get' action='/setrs485' style='margin-top:.8rem'>");
  h += F("<label style='font-size:.85rem;color:#aaa'>Baud RS485: <select name='baud' style='background:#1c1c1c;color:#e0e0e0;border:1px solid #333'>");
  static const uint32_t bauds[] = {4800, 9600, 19200, 38400, 57600, 115200};
  for (uint8_t i = 0; i < 6; i++) {
    h += F("<option value='"); h += String(bauds[i]); h += F("'");
    if (bauds[i] == g_rs485Baud) h += F(" selected");
    h += F(">"); h += String(bauds[i]); h += F("</option>");
  }
  h += F("</select></label> ");
  h += F("<label style='font-size:.85rem;color:#aaa'>Unit: <input name='unit' type='number' min='1' max='247' value='");
  h += String(g_modbusUnit);
  h += F("' style='width:4em;background:#1c1c1c;color:#e0e0e0;border:1px solid #333'></label> ");
  h += F("<button type='submit' style='background:#2ecc71;color:#0a0a0a;border:0;border-radius:4px;padding:.3rem .7rem'>Aplicar</button>");
  h += F("</form>");
  h += F("<p style='color:#888;font-size:.85rem'>Modo configuración: mantén el botón 10 s.</p>");
  h += F("</body></html>");
  return h;
}

static String buildStatusJson() {
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  bool modbusAlive = (g_lastOkMs != 0) && ((millis() - g_lastOkMs) < MODBUS_ALIVE_MS);
  String j;
  j.reserve(500);
  j += F("{\"version\":\""); j += FIRMWARE_VERSION;
  j += F("\",\"build\":\""); j += FIRMWARE_BUILD;
  j += F("\",\"uptime_s\":"); j += String(millis() / 1000);
  j += F(",\"wifi\":{\"status\":");
  if (wifiOk) j += F("\"connected\""); else j += F("\"disconnected\"");
  j += F(",\"ssid\":\"");
  if (wifiOk) j += WiFi.SSID();
  j += F("\",\"ip\":\"");
  if (wifiOk) j += ipToStr(WiFi.localIP());
  j += F("\",\"rssi_dbm\":");
  if (wifiOk) j += String(WiFi.RSSI()); else j += F("null");
  j += F(",\"mac\":\""); j += WiFi.macAddress(); j += F("\"}");
  j += F(",\"http_requests\":"); j += String(g_httpRequests);
  j += F(",\"modbus_tcp\":{\"port\":502,\"ok\":"); j += String(g_mbapOk);
  j += F(",\"err\":"); j += String(g_mbapErr);
  j += F(",\"drop\":"); j += String(g_mbapDrop);
  j += F(",\"client\":"); j += (g_mbapClient ? F("true") : F("false"));
  j += F("},\"modbus_rtu\":{\"port\":5020,\"ok\":"); j += String(g_rtuOk);
  j += F(",\"err\":"); j += String(g_rtuErr);
  j += F(",\"client\":"); j += (g_rtuClient ? F("true") : F("false"));
  j += F("},\"modbus\":{\"alive\":"); j += (modbusAlive ? F("true") : F("false"));
  j += F(",\"last_ok_ms_ago\":");
  if (g_lastOkMs) j += String((millis() - g_lastOkMs)); else j += F("null");
  j += F("},\"crc_errors\":"); j += String(g_crcErrors);
  j += F(",\"rs485\":{\"baud\":"); j += String(g_rs485Baud); j += F(",\"unit\":"); j += String(g_modbusUnit);
  j += F(",\"rx_raw_bytes\":"); j += String(g_rxRawBytes); j += F("}");
  j += F(",\"probe\":{");
  for (uint8_t i = 0; i < PROBE_COUNT; i++) {
    if (i) j += F(",");
    j += F("\""); j += PROBE_NAMES[i]; j += F("\":"); j += String(g_probeValues[i]);
  }
  j += F("},");
  j += F("}}");
  return j;
}

static void handleRoot() {
  g_httpRequests++; flashCom();
  server.send(200, F("text/html"), buildDashboard());
}

static void handleStatus() {
  g_httpRequests++; flashCom();
  server.send(200, F("application/json"), buildStatusJson());
}

static void handleSetRs485() {  g_httpRequests++; flashCom();  if (server.hasArg("baud")) {
    uint32_t b = (uint32_t)server.arg("baud").toInt();
    if (b == 4800 || b == 9600 || b == 19200 || b == 38400 || b == 57600 || b == 115200) {
      g_rs485Baud = b;
      Serial0.begin(g_rs485Baud, SERIAL_8N1);
      Serial.print("Baud RS485 -> ");
      Serial.println(g_rs485Baud);
    }
  }
  if (server.hasArg("unit")) {
    int u = server.arg("unit").toInt();
    if (u >= 1 && u <= 247) g_modbusUnit = (uint8_t)u;
  }
  server.sendHeader("Location", "/", true);
  server.send(302, F("text/plain"), "");
}

// ---------------------------------------------------------------------------
// Actualización de firmware por web (reversible, sin cable).
// Se sube a POST /update el MISMO .uf2 que se flashea por serie.
// ---------------------------------------------------------------------------
static const char UPDATE_HTML[] =
  "<!doctype html><html><head><meta charset='utf-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>Actualizar firmware</title></head>"
  "<body style='font-family:sans-serif;max-width:620px;margin:2rem auto;padding:0 1rem'>"
  "<h2>Actualizar firmware del stick</h2>"
  "<p>Selecciona el fichero <b>.uf2</b> y pulsa Subir. No cortes la corriente.</p>"
  "<form method='POST' action='/update' enctype='multipart/form-data'>"
  "<input type='file' name='firmware' accept='.uf2,.bin' required><br><br>"
  "<button type='submit'>Subir y reiniciar</button></form>"
  "<p style='color:#888'>Firmware actual: " FIRMWARE_VERSION
  " &middot; <a href='/'>volver al panel</a></p></body></html>";

static void handleUpdatePage() {
  g_httpRequests++; flashCom();
  server.send(200, F("text/html"), UPDATE_HTML);
}

static void handleUpdateUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    Serial.print("OTA web: inicio '");
    Serial.print(up.filename);
    Serial.println("'");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.print("OTA web OK: ");
      Serial.print(Update.size());
      Serial.println(" bytes");
    } else {
      Update.printError(Serial);
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    Serial.println("OTA web: abortada");
  }
}

static void handleUpdateDone() {
  g_httpRequests++; flashCom();
  bool ok = !Update.hasError();
  String msg;
  msg += F("Firmware ");
  if (ok) msg += F("ACTUALIZADO"); else msg += F("ERROR");
  msg += F(": ");
  msg += Update.errorString();
  server.sendHeader("Connection", "close");
  server.send(200, F("text/plain"), msg);
  if (ok) { delay(1000); doReboot(); }
}

// ---------------------------------------------------------------------------
// Diagnóstico RS485 en vivo (LAN): GET /raw?hex=<hex>&crc=1&hold=<ms>
// Envía una trama arbitraria y devuelve los bytes TX y RX (hex) en JSON.
// ---------------------------------------------------------------------------
static void handleRaw() {
  g_httpRequests++; flashCom();
  String hexs = server.hasArg("hex") ? server.arg("hex") : String("");
  bool addCrc = server.hasArg("crc") && server.arg("crc") == "1";
  uint32_t hold = server.hasArg("hold") ? (uint32_t)server.arg("hold").toInt() : 0;

  static uint8_t tx[128];
  size_t n = 0;
  for (unsigned i = 0; i + 1 < hexs.length() && n < sizeof(tx) - 2; i += 2) {
    char b[3] = { hexs[i], hexs[i + 1], 0 };
    tx[n++] = (uint8_t)strtol(b, nullptr, 16);
  }
  if (addCrc) { mbp::rtuAppendCrc(tx, n); n += 2; }

  while (Serial0.available()) Serial0.read();
  delayMicroseconds(4000);
  digitalWrite(PIN_RS485_DE, HIGH);
  Serial0.write(tx, n);
  Serial0.flush();
  if (hold) delay(hold);
  digitalWrite(PIN_RS485_DE, LOW);

  static uint8_t rx[256];
  size_t rn = 0;
  uint32_t t0 = millis();
  while (rn == 0 && millis() - t0 < RS485_FIRST_BYTE_MS) {
    if (Serial0.available()) rx[rn++] = (uint8_t)Serial0.read(); else delay(1);
  }
  if (rn) {
    uint32_t tLast = millis();
    while (rn < sizeof(rx)) {
      if (Serial0.available()) { rx[rn++] = (uint8_t)Serial0.read(); tLast = millis(); }
      else if (millis() - tLast > RS485_INTERBYTE_MS) break;
      else delay(1);
    }
  }

  String j = F("{\"tx\":\"");
  for (size_t i = 0; i < n; i++) { if (tx[i] < 16) j += "0"; j += String(tx[i], HEX); }
  j += F("\",\"rx\":\"");
  for (size_t i = 0; i < rn; i++) { if (rx[i] < 16) j += "0"; j += String(rx[i], HEX); }
  j += F("\",\"n\":");
  j += String((unsigned)rn);
  j += F(",\"crc_ok\":");
  j += (rn >= 4 && mbp::rtuCrcOk(rx, rn)) ? F("true") : F("false");
  j += F("}");
  server.send(200, F("application/json"), j);
}

static void handleNotFound() {
  g_httpRequests++; flashCom();
  server.send(404, F("text/plain"), F("404 - no encontrado"));
}

// ---------------------------------------------------------------------------
// Modo configuración (AP): portal cautivo con escaneo de redes
// ---------------------------------------------------------------------------

// Escanea redes en modo STA y cachea los resultados. Debe llamarse ANTES de
// levantar el AP (escanear en AP puro no es fiable en este chip).
static void scanAndCache() {
  netCount = 0;
  WiFi.mode(WIFI_STA);
  delay(200);
  int n = WiFi.scanNetworks(false, false, false, 300, 0);
  if (n > 0) {
    if (n > MAX_NETWORKS) n = MAX_NETWORKS;
    for (int i = 0; i < n; i++) {
      netSsid[i]   = WiFi.SSID(i);
      netRssi[i]   = (int8_t)WiFi.RSSI(i);
      netSecure[i] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    netCount = n;
  }
  WiFi.scanDelete();
}

static String buildPortal() {
  String h;
  h.reserve(2600);

  h += F("<!DOCTYPE html><html lang='es'><head><meta charset='utf-8'>");
  h += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  h += F("<title>S3-WIFI-ST · Configuración</title><style>");
  h += F("body{font-family:system-ui,sans-serif;max-width:560px;margin:2rem auto;padding:0 1rem;background:#111;color:#e0e0e0}");
  h += F("h1{font-size:1.25rem;border-bottom:2px solid #2a2a2a;padding-bottom:.4rem}");
  h += F("label{display:block;margin-top:1rem;font-size:.9rem;color:#aaa}");
  h += F("select,input{width:100%;box-sizing:border-box;padding:.55rem;margin-top:.3rem;background:#1c1c1c;color:#e0e0e0;border:1px solid #333;border-radius:6px;font-size:1rem}");
  h += F("button{width:100%;margin-top:1.4rem;padding:.7rem;background:#2ecc71;color:#0a0a0a;border:0;border-radius:6px;font-size:1rem;font-weight:bold;cursor:pointer}");
  h += F(".hint{font-size:.8rem;color:#777;margin-top:.3rem}");
  h += F("</style></head><body>");

  h += F("<h1>S3-WIFI-ST · Configuración</h1>");
  h += F("<p style='font-size:.9rem;color:#bbb'>Conecta el stick a tu red WiFi de trabajo.</p>");

  h += F("<form method='post' action='/save'>");
  h += F("<label for='netsel'>Red detectada (opcional)</label>");
  h += F("<select id='netsel' onchange=\"document.getElementById('ssid').value=this.value\">");
  h += F("<option value=''>— elige una red o escríbela —</option>");
  for (int i = 0; i < netCount; i++) {
    h += F("<option value='"); h += htmlEscape(netSsid[i]); h += F("'>");
    h += htmlEscape(netSsid[i]);
    h += F(" ("); h += String(netRssi[i]); h += F(" dBm");
    if (netSecure[i]) h += F(", protegida");
    h += F("</option>");
  }
  h += F("</select>");

  h += F("<label for='ssid'>Nombre de red (SSID)</label>");
  h += F("<input id='ssid' name='ssid' maxlength='32' required>");

  h += F("<label for='pass'>Contraseña</label>");
  h += F("<input id='pass' name='pass' type='password' maxlength='64' placeholder='Déjala vacía si la red es abierta'>");
  h += F("<div class='hint'>Si la red es WPA/WPA2, la contraseña debe tener 8–64 caracteres.</div>");

  h += F("<button type='submit'>Guardar y reiniciar</button>");
  h += F("</form>");

  if (netCount == 0) {
    h += F("<p style='color:#e67e22;font-size:.85rem'>No se detectaron redes. Escribe el SSID manualmente.</p>");
  }

  h += F("</body></html>");
  return h;
}

static void handlePortal() {
  g_httpRequests++; flashCom();
  server.send(200, F("text/html"), buildPortal());
}

static void handleSave() {
  g_httpRequests++; flashCom();

  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();

  if (ssid.length() == 0 || ssid.length() > 32) {
    server.send(400, F("text/html"), F("SSID inválido (1–32 caracteres). <a href='/'>Volver</a>"));
    return;
  }
  if (pass.length() > 64 || (pass.length() > 0 && pass.length() < 8)) {
    server.send(400, F("text/html"), F("Contraseña inválida (8–64 caracteres, o vacía si es abierta). <a href='/'>Volver</a>"));
    return;
  }

  Config cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic   = CFG_MAGIC;
  cfg.version = CFG_VERSION;
  cfg.flags   = 0;  // próximo arranque en modo normal (STA)
  strncpy(cfg.ssid, ssid.c_str(), 32);
  cfg.ssid[32] = 0;
  strncpy(cfg.pass, pass.c_str(), 64);
  cfg.pass[64] = 0;
  saveConfig(cfg);

  Serial.print("Config guardada: SSID=");
  Serial.print(cfg.ssid);
  Serial.print(" pass_len=");
  Serial.println(pass.length());

  String resp;
  resp += F("<!DOCTYPE html><html lang='es'><head><meta charset='utf-8'>");
  resp += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  resp += F("<title>Guardado</title><style>body{font-family:system-ui,sans-serif;background:#111;color:#e0e0e0;text-align:center;padding-top:4rem}.ok{color:#2ecc71;font-size:2rem}</style></head><body>");
  resp += F("<div class='ok'>✓</div><h2>Configuración guardada</h2>");
  resp += F("<p>El stick se reinicia y se conectará a <strong>");
  resp += htmlEscape(ssid);
  resp += F("</strong>.</p></body></html>");

  server.send(200, F("text/html"), resp);
  delay(1000);     // dar tiempo a que el navegador reciba la respuesta
  doReboot();      // reset fiable (sys_reset) -> arranca en modo STA
}

// ---------------------------------------------------------------------------
// FASE 2 — puente Modbus: enlace RS485 + servicios de socket
// ---------------------------------------------------------------------------

// Envía 'frame' (ya con CRC) por el RS485, conmutando el transceptor (PA19).
static void rs485Send(const uint8_t *frame, size_t len) {
  while (Serial0.available()) Serial0.read();  // descarta basura previa
  // Modbus RTU exige ≥3,5 caracteres de silencio antes de una trama (≈3,65 ms a 9600).
  delayMicroseconds(4000);
  digitalWrite(PIN_RS485_DE, HIGH);            // transmitir
  Serial0.write(frame, len);                   // write() bloquea byte a byte según el FIFO
  Serial0.flush();                             // drena el FIFO del UART (TX complete)
  // ⚠️ DE y /RE van UNIDOS en el CA-IF4805HS: hay que bajar DE INMEDIATAMENTE
  // tras el flush para habilitar el receptor durante la ventana de respuesta.
  // El `send_wait_time: 300ms` de ESPHome NO es el hold del DE (es el gap mínimo
  // entre envíos, modbus.cpp:44); mantenerlo aquí apagaba el receptor y dejaba
  // el driver ocupando el bus justo cuando el Solis contesta (~300 ms).
  digitalWrite(PIN_RS485_DE, LOW);             // recibir
}

// Lee una trama RTU del inversor hasta un silencio inter-byte o timeout.
// Devuelve el nº de bytes leídos (0 si no respondió).
static size_t rs485Recv(uint8_t *buf, size_t maxLen) {
  size_t n = 0;
  uint32_t t0 = millis();
  while (n == 0 && (millis() - t0) < RS485_FIRST_BYTE_MS) {
    if (Serial0.available()) buf[n++] = (uint8_t)Serial0.read();
    else delay(1);
  }
  if (n == 0) return 0;

  uint32_t tLast = millis();
  while (n < maxLen) {
    if (Serial0.available()) {
      buf[n++] = (uint8_t)Serial0.read();
      tLast = millis();
    } else if (millis() - tLast > RS485_INTERBYTE_MS) {
      break;                                   // fin de trama (3,5 caracteres)
    } else {
      delay(1);
    }
  }
  g_rxRawBytes += n;  // diagnóstico: cuenta bytes crudos (aunque luego fallen CRC)
  return n;
}


// Petición Modbus completa: envía unit+PDU por RS485 y devuelve la respuesta RTU
// cruda (con CRC) en 'resp'. Devuelve el nº de bytes o 0 si no hubo respuesta válida.
static size_t rs485Request(uint8_t unit, const uint8_t *pdu, size_t pduLen,
                           uint8_t *resp, size_t respMax) {
  if (pduLen < 1 || pduLen > mbp::PDU_MAX) return 0;

  static uint8_t req[mbp::PDU_MAX + 3];
  static uint8_t tmp[MODBUS_BUF_SZ + 2];

  req[0] = unit;
  memcpy(req + 1, pdu, pduLen);
  mbp::rtuAppendCrc(req, 1 + pduLen);
  rs485Send(req, 1 + pduLen + 2);

  size_t n = rs485Recv(tmp, sizeof(tmp));
  if (n < 4) return 0;
  if (!mbp::rtuCrcOk(tmp, n)) { g_crcErrors++; return 0; }
  if (tmp[0] != unit) return 0;                // unit distinto -> ignorar

  size_t copy = (n < respMax) ? n : respMax;
  memcpy(resp, tmp, copy);
  return copy;
}

// Procesa una trama MBAP completa y responde por el socket.
static void processMbapFrame(const uint8_t *frame, size_t frameLen) {
  mbp::Mbap h;
  if (!mbp::mbapParse(frame, h)) return;

  const uint8_t *pdu = frame + mbp::MBAP_HDR;
  size_t pduLen = frameLen - mbp::MBAP_HDR;

  // unit 0 o 255 se tratan como "el inversor" (unit por defecto).
  uint8_t unit = h.unit;
  if (unit == 0 || unit == 0xFF) unit = g_modbusUnit;

  static uint8_t respRtu[MODBUS_BUF_SZ];
  static uint8_t out[MODBUS_BUF_SZ];

  size_t rn = (pduLen >= 1) ? rs485Request(unit, pdu, pduLen, respRtu, sizeof(respRtu)) : 0;

  size_t outLen;
  if (rn >= 4) {
    g_lastOkMs = millis();
    outLen = mbp::buildMbapFromRtu(frame, respRtu, rn, out, sizeof(out));
    if (outLen) {
      g_mbapOk++;
    } else {
      g_mbapErr++;
      outLen = mbp::buildMbapException(frame, pdu[0], mbp::EX_GATEWAY_TARGET, out, sizeof(out));
    }
  } else {
    // el inversor no respondió -> excepción 0x0B (gateway target failed to respond)
    uint8_t fc = (pduLen >= 1) ? pdu[0] : 0;
    g_mbapErr++;
    outLen = mbp::buildMbapException(frame, fc, mbp::EX_GATEWAY_TARGET, out, sizeof(out));
  }
  if (outLen) g_mbapClient.write(out, outLen);
  flashCom();
}

// Atiende el servidor Modbus TCP (:502). No bloqueante.
static void modbusTcpService() {
  if (!g_mbapClient) {
    g_mbapClient = modbusTcp.available();
    if (!g_mbapClient) return;
    g_mbapLen  = 0;
    g_mbapLast = millis();
    Serial.print("Modbus TCP: cliente ");
    Serial.println(g_mbapClient.remoteIP());
  }
  if (!g_mbapClient.connected()) {
    g_mbapClient.stop();
    g_mbapLen = 0;
    return;
  }

  // Acumular los bytes disponibles.
  while (g_mbapClient.available() > 0) {
    if (g_mbapLen >= sizeof(g_mbapBuf)) {  // desbordamiento -> descartar
      g_mbapLen = 0;
      g_mbapDrop++;
      break;
    }
    int b = g_mbapClient.read();
    if (b < 0) break;
    g_mbapBuf[g_mbapLen++] = (uint8_t)b;
    g_mbapLast = millis();
  }
  if (g_mbapLen > 0 && (millis() - g_mbapLast) > MODBUS_IDLE_MS) g_mbapLen = 0;

  // Procesar cuantas tramas completas haya en el buffer.
  while (g_mbapLen >= mbp::MBAP_HDR) {
    mbp::Mbap h;
    if (!mbp::mbapParse(g_mbapBuf, h)) { g_mbapLen = 0; g_mbapDrop++; break; }
    size_t total = mbp::mbapTotalLen(h);
    if (total > sizeof(g_mbapBuf)) { g_mbapLen = 0; g_mbapDrop++; break; }
    if (g_mbapLen < total) break;          // aún incompleta
    processMbapFrame(g_mbapBuf, total);
    if (g_mbapLen > total) memmove(g_mbapBuf, g_mbapBuf + total, g_mbapLen - total);
    g_mbapLen -= total;
  }
}

// Atiende el servidor Modbus RTU sobre TCP (:5020). Delimita por silencio.
static void modbusRtuService() {
  if (!g_rtuClient) {
    g_rtuClient = modbusRtu.available();
    if (!g_rtuClient) return;
    g_rtuLen  = 0;
    g_rtuLast = millis();
    Serial.print("Modbus RTU: cliente ");
    Serial.println(g_rtuClient.remoteIP());
  }
  if (!g_rtuClient.connected()) {
    g_rtuClient.stop();
    g_rtuLen = 0;
    return;
  }

  while (g_rtuClient.available() > 0) {
    if (g_rtuLen >= sizeof(g_rtuBuf)) { g_rtuLen = 0; break; }
    int b = g_rtuClient.read();
    if (b < 0) break;
    g_rtuBuf[g_rtuLen++] = (uint8_t)b;
    g_rtuLast = millis();
  }

  if (g_rtuLen == 0) return;
  uint32_t gap = millis() - g_rtuLast;

  if (gap > MODBUS_IDLE_MS) { g_rtuLen = 0; return; }  // basura parcial
  if (gap < RTU_SILENCE_MS) return;                    // aún puede llegar más

  // Trama completa: validar CRC, reenviar tal cual por RS485 y devolver respuesta.
  if (g_rtuLen >= 4 && mbp::rtuCrcOk(g_rtuBuf, g_rtuLen)) {
    static uint8_t resp[MODBUS_BUF_SZ + 2];
    rs485Send(g_rtuBuf, g_rtuLen);
    size_t rn = rs485Recv(resp, sizeof(resp));
    if (rn >= 4 && mbp::rtuCrcOk(resp, rn)) {
      g_lastOkMs = millis();
      g_rtuClient.write(resp, rn);
      g_rtuOk++;
    } else {
      g_rtuErr++;
    }
    flashCom();
  } else {
    g_crcErrors++;
    g_rtuErr++;
  }
  g_rtuLen = 0;
}

// ---------------------------------------------------------------------------
// setup() y loop()
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(DEBUG_BAUD);
  delay(300);
  Serial.println();
  Serial.println("=== Solis S3-WIFI-ST · puente Modbus (FASE 1.5 + 2 + 3 · web + Modbus + config) ===");
  Serial.print("Firmware "); Serial.print(FIRMWARE_VERSION); Serial.print(" · build "); Serial.println(FIRMWARE_BUILD);
  Serial.print("MCU: RTL8710BN @ ");
  Serial.print(LT.getCpuFreqMHz());
  Serial.println(" MHz");

  // Sondeo de pines GPIO ANTES de configurar UARTs (para localizar el RO).
  doProbe();

  // RS485 + LEDs + watchdog + botón
  // UART0 completo (TX PA23 / RX PA18) + DE/RE en PA19 — igual que el ESPHome
  // de referencia que SÍ lee el inversor.
  // Sin pines explícitos: usa los del variant UART0 (RX=PA18 / TX=PA23),
  // EXACTAMENTE la llamada que hace ESPHome (this->serial_->begin(baud, cfg)).
  Serial0.begin(g_rs485Baud, SERIAL_8N1);
  pinMode(PIN_LED_COM, OUTPUT);  digitalWrite(PIN_LED_COM, LOW);
  pinMode(PIN_LED_WIFI, OUTPUT); digitalWrite(PIN_LED_WIFI, LOW);
  pinMode(PIN_WDT, OUTPUT);      digitalWrite(PIN_WDT, LOW);
  kickWatchdog();
  pinMode(PIN_RS485_DE, OUTPUT); digitalWrite(PIN_RS485_DE, LOW);
  pinMode(PIN_BOTON, INPUT_PULLUP);

  // (búsqueda de UART retirada: pinout ya fijado y verificado con ESPHome)
  // --- Decidir el modo ---
  Config cfg;
  bool cfgValid = loadConfig(cfg);

  bool forceAp = cfgValid && (cfg.flags & CFG_FLAG_FORCE_AP);

  // Consumir el flag FORCE_AP de inmediato: si el usuario reinicia físicamente
  // SIN guardar, no debe quedarse atascado en AP para siempre.
  if (forceAp) {
    cfg.flags &= ~CFG_FLAG_FORCE_AP;
    saveConfig(cfg);
  }

  if (forceAp || !cfgValid) {
    // Modo configuración (AP)
    g_mode = MODE_AP;
    Serial.println("Modo: CONFIGURACIÓN (AP). " + String(forceAp ? "Botón 10s." : "Sin config válida."));

    scanAndCache();
    Serial.print("Escaneo: ");
    Serial.print(netCount);
    Serial.println(" redes.");

    // ⚠️ Tras escanear estamos en modo STA. Si llamamos softAP() directamente,
    // enableAP(true) hace mode(STA^AP) = APSTA (AP+STA simultáneo), que está ROTO
    // en este chip (verificado en WiFiGeneric.cpp: "adding AP mode doesn't seem to
    // work fine: STA -> AP+STA") y el AP no emite bien. Forzamos AP PURO primero.
    WiFi.mode(WIFI_AP);

    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
    if (WiFi.softAP(AP_SSID, NULL)) {
      Serial.print("AP abierto: ");
      Serial.print(AP_SSID);
      Serial.print(" @ ");
      Serial.println(ipToStr(WiFi.softAPIP()));
    } else {
      Serial.println("ERROR: no se pudo crear el AP.");
    }

    server.on("/", handlePortal);
    server.on("/save", handleSave);
    // /update también en modo AP: permite reflashear por web aunque el stick
    // no tenga WiFi válida (recuperación sin cable).
    server.on("/update", HTTP_GET, handleUpdatePage);
    server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
    server.onNotFound(handlePortal);
    server.begin();

    Serial.print("Portal de configuración en http://");
    Serial.print(ipToStr(AP_IP));
    Serial.println("/");
  } else {
    // Modo normal (STA)
    g_mode = MODE_STA;
    Serial.println("Modo: NORMAL (STA).");

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    const char *ssid = cfg.ssid;
    const char *pass = (cfg.pass[0] == '\0') ? NULL : cfg.pass;

    Serial.print("Conectando a \"");
    Serial.print(ssid);
    Serial.println("\"...");

    WiFi.begin(ssid, pass);

    uint32_t t0 = millis(), tWdt = millis();
    while ((WiFi.status() != WL_CONNECTED || (uint32_t)WiFi.localIP() == 0) && millis() - t0 < 15000) {
      if (millis() - tWdt > 5000) { tWdt = millis(); kickWatchdog(); }
      delay(500);
    }

    if (WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0) {
      Serial.print("WiFi OK · IP: "); Serial.println(WiFi.localIP());
      Serial.print("RSSI: "); Serial.println(WiFi.RSSI());
    } else {
      Serial.println("WiFi: NO conectado aún (reintentaré en loop).");
    }

    server.on("/", handleRoot);
    server.on("/api/status", handleStatus);
    server.on("/setrs485", handleSetRs485);
    server.on("/raw", handleRaw);
    server.on("/update", HTTP_GET, handleUpdatePage);
    server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
    server.onNotFound(handleNotFound);
    server.begin();

    Serial.print("Servidor web en http://");
    Serial.print(ipToStr(WiFi.localIP()));
    Serial.print(":");
    Serial.println(HTTP_PORT);

    // --- FASE 2: arrancar los dos servidores del puente Modbus ---
    // (después de WiFi.begin(): LwIP ya está inicializado).
    if (modbusTcp.begin()) {
      Serial.print("Modbus TCP (MBAP) en :"); Serial.println(MODBUS_TCP_PORT);
    } else {
      Serial.println("ERROR: no se pudo abrir el puerto 502.");
    }
    if (modbusRtu.begin()) {
      Serial.print("Modbus RTU (crudo) en :"); Serial.println(MODBUS_RTU_PORT);
    } else {
      Serial.println("ERROR: no se pudo abrir el puerto 5020.");
    }
  }

  Serial.println("FASE 1.5+2+3 OK.");
}

void loop() {
  // Kick del watchdog hardware (siempre).
  static uint32_t tWdt = 0;
  if (millis() - tWdt > WDT_KICK_MS) {
    tWdt = millis();
    kickWatchdog();
  }

  if (g_mode == MODE_STA) {
    // --- Reintento de WiFi si se cayó ---
    static uint32_t tReconnect = 0;
    if (WiFi.status() != WL_CONNECTED && millis() - tReconnect > RECONNECT_MS) {
      tReconnect = millis();
      Serial.println("WiFi caída — reintentando...");
      WiFi.reconnect();
    }

    // --- Botón >10 s -> modo configuración ---
    static uint32_t pressedSince = 0;
    if (digitalRead(PIN_BOTON) == LOW) {
      if (pressedSince == 0) {
        pressedSince = millis();
      } else if (millis() - pressedSince > LONG_PRESS_MS) {
        pressedSince = 0;  // evita re-disparo
        Serial.println("Botón >10 s -> entro en modo configuración.");
        Config cfg;
        if (!loadConfig(cfg)) {
          memset(&cfg, 0, sizeof(cfg));
          cfg.magic = CFG_MAGIC; cfg.version = CFG_VERSION;
        }
        cfg.flags |= CFG_FLAG_FORCE_AP;
        saveConfig(cfg);
        digitalWrite(PIN_LED_COM, HIGH);
        delay(500);
        doReboot();
      }
    } else {
      pressedSince = 0;
    }

    // --- LED NET verde: fijo si conectado, parpadeo si no ---
    static uint32_t tNet = 0;
    if (millis() - tNet > 500) {
      tNet = millis();
      if (WiFi.status() == WL_CONNECTED) digitalWrite(PIN_LED_WIFI, HIGH);
      else digitalWrite(PIN_LED_WIFI, !digitalRead(PIN_LED_WIFI));
    }

    // --- FASE 2: puente Modbus (no bloqueante salvo el diálogo RS485) ---
    modbusTcpService();
    modbusRtuService();
  } else {
    // --- Modo AP: LED verde parpadeo lento y claro para indicar "config" ---
    // Uso variable de estado (no digitalRead, poco fiable en pin OUTPUT) y
    // 700 ms on / 700 ms off -> ~0,7 Hz, claramente visible.
    static uint32_t tNet = 0;
    static bool     ledState = false;
    if (millis() - tNet > 700) {
      tNet = millis();
      ledState = !ledState;
      digitalWrite(PIN_LED_WIFI, ledState ? HIGH : LOW);
    }
  }

  // Atender clientes HTTP.
  server.handleClient();

  // Ceder al hilo TCP de LwIP (aviso explícito en LwIPServer.cpp del core).
  delay(1);

  // LED COM: apagar al vencer el destello.
  if (g_comLedUntil && millis() > g_comLedUntil) {
    digitalWrite(PIN_LED_COM, LOW);
    g_comLedUntil = 0;
  }
}
