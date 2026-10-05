Confirming **ESPHome 2026.1.5 works here too, on an ESINV (hybrid with battery)** model, not only INV:

* Board: Solis S3 WiFi stick (EMW3080 / RTL8710BN), LibreTiny pinned to **1.10.0**, ArduinoCore-API #RingBufferFix.
* Inverter: **S5-EH1P4.6K-L** (hybrid), 9600 8N1, unit 1, **FC4** (`register_type: read`), `send_wait_time: 300ms`.
* With **2026.6.5** every Modbus sensor stayed `unknown`; after downgrading to **2026.1.5** they report live values (AC voltage, battery SoC/SoH, battery power, energy today, etc.).

Three observations that may save others time:

1. On 2026.6.5 (and probably the whole 2026.2–2026.6 range) the native API **and** the OTA server **reset every connection** (TCP accept then RST). So once you OTA up to that range you cannot OTA back off it — recovery needs serial / UART boot mode. Might be worth adding to #76.
2. The **current master yaml targets 2026.9 and will not compile on 2026.1.5**: two lambdas in `solis-modbus-esinv.yaml` use `modbus::helpers::word_from_hex_str()`, which does not exist in 2026.1.5. Changing them to `esphome::modbus_controller::word_from_hex_str()` compiles and works.
3. Related to (1): with the master yaml the WDT kick (PA00) only fires when `api.connected`, so while the API is broken the external watchdog resets the board ~every 15 min. Decoupling it with an independent `interval: 30s` avoids that.

FYI, OTA/API also fail when tunnelled through an SSH port-forward (jump host): the device resets the handshake. From the same LAN it works.
