/*
 * S3-WIFI-ST  ->  FIRMWARE DE DIAGNOSTICO: barrido de GPIO para LOCALIZAR LOS LEDs
 *
 * Para que sirve:
 *   El firmware FASE 1 corre y saluda por consola, pero en la placa de pruebas los
 *   LEDs "power / net / com" no parpadean en PA12/PA05 (los pines que documenta
 *   upstream). Este sketch enciende UN GPIO cada vez, anunciandolo por consola,
 *   para identificar EMPIRICAMENTE que pin enciende que LED.
 *
 * Uso:
 *   pio run -e diag           -> .pio/build/diag/firmware.bin
 *   ltchiptool flash write -d COMx firmware.bin
 *   Abrir el monitor a 9600 8N1 (mismo UART2 = PA30/PA29 que en FASE 1).
 *   Quedarse MIRANDO la placa y ver que LED se enciende cuando la consola
 *   imprime cada ">>> PAxx".
 *
 * Garantias de seguridad:
 *   - NO toca UART2 (PA30/PA29) -> la consola sigue viva.
 *   - NO toca UART0 (PA23/PA18) ni DE/RE (PA19) -> no interfiere el RS485.
 *   - NO toca PA08 (boton reset) ni PA00 (se usa solo para el kick del WDT).
 *   - Da de comer al watchdog hardware (PA00) para que no reinicie el MCU.
 *   - Imprime el uptime: si de repente vuelve a 0, el stick se ha reiniciado.
 */

#include <Arduino.h>

// --- Pines reservados (NO se barren) ---
static const uint8_t PIN_UART2_TX = PIN_PA30;  // consola
static const uint8_t PIN_UART2_RX = PIN_PA29;  // consola
static const uint8_t PIN_RS485_TX = PIN_PA23;  // RS485
static const uint8_t PIN_RS485_RX = PIN_PA18;  // RS485
static const uint8_t PIN_RS485_DE = PIN_PA19;  // DE/RE
static const uint8_t PIN_WDT      = PIN_PA00;  // kick watchdog hardware
static const uint8_t PIN_BOTON    = PIN_PA08;  // boton reset

// --- Pines candidatos a LED (GPIO libres de la variante generic-rtl8710bx-4mb-980k) ---
static const uint8_t CANDIDATOS[] = {
  PIN_PA12, PIN_PA05, PIN_PA06, PIN_PA07, PIN_PA09,
  PIN_PA10, PIN_PA11, PIN_PA14, PIN_PA15, PIN_PA22,
};
static const char* NOMBRES[] = {
  "PA12  (esperado LED COM/naranja segun upstream)",
  "PA05  (esperado LED NET/verde segun upstream)",
  "PA06", "PA07", "PA09", "PA10", "PA11", "PA14", "PA15", "PA22",
};
static const uint8_t N_CAND = sizeof(CANDIDATOS) / sizeof(CANDIDATOS[0]);

static const uint32_t CONSOLE_BAUD = 9600;   // mismo puerto/baud que FASE 1
static const uint32_t WDT_KICK_MS  = 5000;   // << 15 min del watchdog
static const uint32_t ON_MS        = 2000;   // tiempo que el pin queda HIGH
static const uint32_t OFF_MS       = 1000;   // tiempo que el pin queda LOW

static void kickWatchdog() {
  digitalWrite(PIN_WDT, HIGH);
  delay(100);
  digitalWrite(PIN_WDT, LOW);
}

void setup() {
  Serial.begin(CONSOLE_BAUD);
  delay(300);
  Serial.println();
  Serial.println("### S3-WIFI-ST · DIAGNOSTICO DE LEDs (barrido de GPIO) ###");
  Serial.print("MCU: RTL8710BN @ ");
  Serial.print(LT.getCpuFreqMHz());
  Serial.println(" MHz");
  Serial.print("Reservados: UART2=");
  Serial.print(PIN_UART2_TX); Serial.print("/"); Serial.print(PIN_UART2_RX);
  Serial.print(" UART0=");
  Serial.print(PIN_RS485_TX); Serial.print("/"); Serial.print(PIN_RS485_RX);
  Serial.print(" DE/RE="); Serial.print(PIN_RS485_DE);
  Serial.print(" WDT="); Serial.print(PIN_WDT);
  Serial.print(" BOTON="); Serial.println(PIN_BOTON);

  // Watchdog hardware (placa nueva). Inofensivo en la vieja.
  pinMode(PIN_WDT, OUTPUT);
  digitalWrite(PIN_WDT, LOW);
  kickWatchdog();

  Serial.println("Quedate mirando la PLACA. Cada pin = 2 s encendido, 1 s apagado.");
  Serial.println("Apunta: pin <- que LED de los tres (power/net/com) se enciende.");
}

void loop() {
  static uint32_t lastWdt = 0;
  static uint32_t t0 = 0;          // instante de arranque de este sketch

  for (uint8_t i = 0; i < N_CAND; i++) {
    // Kick del watchdog si toca (el timeout real es ~15 min; aqui sobradisimo)
    if (millis() - lastWdt > WDT_KICK_MS) {
      lastWdt = millis();
      kickWatchdog();
    }

    // Anuncio: numero de pin, nombre y uptime (para detectar reinicios)
    Serial.print("\n>>> [");
    Serial.print(i + 1);
    Serial.print("/");
    Serial.print(N_CAND);
    Serial.print("] ");
    Serial.print(NOMBRES[i]);
    Serial.print("  HIGH 2 s  (uptime ");
    Serial.print((millis() - t0) / 1000);
    Serial.println(" s)");

    pinMode(CANDIDATOS[i], OUTPUT);
    digitalWrite(CANDIDATOS[i], HIGH);
    delay(ON_MS);

    digitalWrite(CANDIDATOS[i], LOW);
    Serial.print("    ");
    Serial.print(NOMBRES[i]);
    Serial.println("  LOW 1 s");
    delay(OFF_MS);

    // Dejar el pin como entrada de alta impedancia al terminar
    pinMode(CANDIDATOS[i], INPUT);
  }

  Serial.print("\n=== Fin de vuelta (uptime ");
  Serial.print((millis() - t0) / 1000);
  Serial.println(" s). Repitiendo... ===\n");
  kickWatchdog();
}
