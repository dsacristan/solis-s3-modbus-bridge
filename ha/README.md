# Home Assistant — leer el inversor por el puente Modbus

`solis_modbus.yaml` es el **paquete de Home Assistant** que lee el inversor Solis S5-EH1P4.6K-L
**localmente**, a través del puente Modbus de este stick, y reproduce las entidades `sensor.solis_*`
que antes servía la integración cloud (para no tocar paneles, plantillas ni el panel de Energía).

## Instalación
1. Copia `solis_modbus.yaml` a `/config/packages/` de tu Home Assistant.
2. En `configuration.yaml`:
   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```
3. Ajusta `host:` si tu stick no está en `192.168.1.254`.
4. Elimina/deshabilita la integración cloud `solis` (el stick ya no reporta a SolisCloud).

## Notas
- El hub lee con **FC4 (input registers)**: el inversor ESINV **no responde a FC3**.
  `port: 502` (Modbus TCP / MBAP); también vale `5020` con `type: rtuovertcp` (RTU crudo).
- `delay: 0.5` y `scan_interval: 30` mantienen el bus RS485 (compartido con evcc) sin saturarlo.
- Registros verificados contra la nube (el detalle está en la cabecera del YAML).
- El maestro de la reserva de batería y `batteryDischargeControl` deben quedar **OFF** por defecto.
