/*
 * S3-WIFI-ST  ->  DIAGNOSTICO / MONITOR DEL BOTON DE RESET  (v3, limpio)
 * ----------------------------------------------------------------------------
 * RESULTADO DE LA INVESTIGACION (v1 y v2 con Hard Fault):
 *
 *   El boton ES PA08, CONFIRMADO empiricamente (v2 detecto 3 pulsaciones con
 *   duracion correcta: 228 / 568 / 497 ms, todas clasificadas como CORTO).
 *
 *   El Hard Fault NO era un bug nuestro ni del flash: era el BARRIDO de pines.
 *   En el RTL8710B, segun el propio SDK de Realtek
 *   (component/soc/realtek/8711b/fwlib/ram_lib/rtl8710b_pinmapcfg.c):
 *       {_PA_6, PINMUX_FUNCTION_SPIF, ...}, //SPIC_CS
 *       {_PA_7, PINMUX_FUNCTION_SPIF, ...}, //SPIC_DATA1
 *       {_PA_8, PINMUX_FUNCTION_SPIF, ...}, //SPIC_DATA2
 *       {_PA_9, PINMUX_FUNCTION_SPIF, ...}, //SPIC_DATA0
 *       {_PA_10,PINMUX_FUNCTION_SPIF, ...}, //SPIC_CLK
 *       {_PA_11,PINMUX_FUNCTION_SPIF, ...}, //SPIC_DATA3
 *   -> PA6..PA11 son los pines del FLASH SPI. El firmware se ejecuta EN EL SITIO
 *      (XIP) desde esa flash (seccion .xip_image2.text). Reconfigurar PA6
 *      (SPIC_CS = chip-select de la flash) como GPIO le quita el CS a la flash:
 *      la CPU no puede leer mas instrucciones y saltan instrucciones basura
 *      -> CFSR=0x10000 (UNDEFINSTR) + HFSR=0x40000000 (FORCED).
 *
 *   Por eso la v2 imprimia "S0: pin PA6 -> poniendo INPUT_PULLUP..." y moria
 *   justo despues, en el pinMode(PA6)/delay: el primer pin del barrido era
 *   precisamente el chip-select de la flash.
 *
 * PINES RESERVADOS EN ESTE CHIP (NO USAR como GPIO):
 *   PA6  (SPIC_CS)   <- CRITICO, mata el firmware
 *   PA7  (SPIC_DATA1)
 *   PA9  (SPIC_DATA0)
 *   PA10 (SPIC_CLK)
 *   PA11 (SPIC_DATA3)
 *   PA8  (SPIC_DATA2) -> nominalmente flash, pero en ESTA placa esta libre
 *                         (es el boton de reset; el flash parece usar solo 1-2
 *                         lineas de datos). Confirmado en hardware.
 *
 * Esta v3 es SOLO el monitor del boton PA08 (sin barrido), con watchdog y LEDs.
 * El boton queda asi identificado y utilizable para la FASE 2/3 (p.ej.
 * pulsacion corta = reiniciar el servidor Modbus, >=5 s = reset de fabrica).
 */

#include <Arduino.h>

// --- Pines (SOLO los que usamos; nada de la zona flash PA6..PA11) ---
static const uint8_t PIN_WDT     = PIN_PA00;   // kick watchdog hardware
static const uint8_t PIN_LED_COM = PIN_PA12;   // LED naranja (COM)
static const uint8_t PIN_LED_NET = PIN_PA05;   // LED verde (NET)
static const uint8_t PIN_BOTON   = PIN_PA08;   // boton de reset (confirmado)

// --- Parametros ---
static const uint32_t CONSOLE_BAUD = 9600;
static const uint32_t WDT_KICK_MS  = 5000;   // << 15 min del watchdog hardware
static const uint32_t DEBOUNCE_MS  = 30;
static const uint32_t T_LARGO_MS   = 1000;   // >= 1 s  -> LARGO
static const uint32_t T_RESET_MS   = 5000;   // >= 5 s  -> gesto reset fabrica

static void kickWdt() {
  digitalWrite(PIN_WDT, HIGH);
  delay(100);
  digitalWrite(PIN_WDT, LOW);
}

// ---------- SETUP ----------
void setup() {
  Serial.begin(CONSOLE_BAUD);
  delay(300);

  Serial.println();
  Serial.println("### S3-WIFI-ST - MONITOR DEL BOTON DE RESET (v3) ###");
  Serial.flush();
  Serial.println("MCU: RTL8710BN @ 125 MHz");
  Serial.flush();
  Serial.println("Boton: PA8 (INPUT_PULLUP, activo-LOW). NO se tocan PA6..PA11 (flash).");
  Serial.flush();

  // Watchdog hardware (inofensivo en placas viejas)
  pinMode(PIN_WDT, OUTPUT);
  digitalWrite(PIN_WDT, LOW);
  kickWdt();
  Serial.println("P1: watchdog PA00 kick OK");
  Serial.flush();

  // LEDs
  pinMode(PIN_LED_COM, OUTPUT);
  digitalWrite(PIN_LED_COM, LOW);
  pinMode(PIN_LED_NET, OUTPUT);
  digitalWrite(PIN_LED_NET, LOW);
  Serial.println("P2: LEDs PA12(naranja)/PA05(verde) OK");
  Serial.flush();

  // Boton
  pinMode(PIN_BOTON, INPUT_PULLUP);
  Serial.print("P3: boton PA8, leido = ");
  Serial.println(digitalRead(PIN_BOTON));
  Serial.flush();

  Serial.println("--- LISTO. Pulsa el boton (corto/largo/>=5 s). ---");
  Serial.flush();
}

// ---------- LOOP ----------
void loop() {
  static uint32_t tWdt = 0, tBeat = 0, tCom = 0;
  static int      lastBtn = HIGH;
  static uint32_t tLastChange = 0, tPressStart = 0;
  static uint16_t nPress = 0;

  // Watchdog
  if (millis() - tWdt > WDT_KICK_MS) {
    tWdt = millis();
    kickWdt();
  }

  // Latido LED verde (~1 Hz)
  if (millis() - tBeat > 500) {
    tBeat = millis();
    digitalWrite(PIN_LED_NET, !digitalRead(PIN_LED_NET));
  }

  // Apagar destello naranja
  if (tCom && millis() - tCom > 150) {
    digitalWrite(PIN_LED_COM, LOW);
    tCom = 0;
  }

  // --- Boton PA08 (anti-rebote) ---
  int b = digitalRead(PIN_BOTON);
  if (b != lastBtn && millis() - tLastChange > DEBOUNCE_MS) {
    tLastChange = millis();
    lastBtn = b;
    if (b == LOW) {
      tPressStart = millis();
      nPress++;
      Serial.print("[t=");
      Serial.print(millis());
      Serial.print(" ms] PULSADO (#");
      Serial.print(nPress);
      Serial.println(")");
      digitalWrite(PIN_LED_COM, HIGH);
      tCom = millis();
    } else {
      uint32_t dur = millis() - tPressStart;
      Serial.print("[t=");
      Serial.print(millis());
      Serial.print(" ms] SOLTADO dur=");
      Serial.print(dur);
      Serial.print(" ms -> ");
      if (dur < T_LARGO_MS)        Serial.println("CORTO");
      else if (dur < T_RESET_MS)   Serial.println("LARGO");
      else                         Serial.println("MUY LARGO (gesto reset fabrica)");
    }
    Serial.flush();
  }
}
