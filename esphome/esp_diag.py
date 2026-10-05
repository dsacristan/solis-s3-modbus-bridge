#!/usr/bin/env python3
"""Diagnóstico directo del ESPHome del stick vía API nativa (aioesphomeapi).
Compatible con aioesphomeapi antigua (2026.1.5) y moderna.
Uso:  esp_diag.py <host> <psk|-> [segundos]
"""
import asyncio, sys, time, inspect
from aioesphomeapi import APIClient, LogLevel

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PSK_ARG = sys.argv[2] if len(sys.argv) > 2 else "-"
DUR = int(sys.argv[3]) if len(sys.argv) > 3 else 25
PSK = None if PSK_ARG in ("-", "", "none") else PSK_ARG


def make_client():
    """Intenta con client_info; si no existe el kwarg, sin él."""
    kw = {}
    try:
        sig = inspect.signature(APIClient.__init__)
        if "client_info" in sig.parameters:
            kw["client_info"] = "pikita-diag"
    except Exception:
        pass
    try:
        return APIClient(HOST, 6053, None, noise_psk=PSK, **kw)
    except TypeError:
        return APIClient(HOST, 6053, None, noise_psk=PSK)


async def run():
    stop = asyncio.Event()

    async def on_stop(expected_disconnect: bool):
        print(f"### on_stop(expected_disconnect={expected_disconnect})", flush=True)
        stop.set()

    def on_log(msg):
        try:
            text = msg.message.decode("utf-8", "replace").rstrip("\n")
        except Exception:
            text = str(getattr(msg, "message", msg))
        print(f"[LOG] {text}", flush=True)

    def on_state(state):
        try:
            print(f"[STATE] key={getattr(state,'key','?')} = {getattr(state,'state',state)}", flush=True)
        except Exception:
            print(f"[STATE] {state}", flush=True)

    cli = make_client()
    try:
        await cli.connect(on_stop=on_stop)
    except Exception as e:
        print(f"### connect FALLÓ psk={'si' if PSK else 'no'}: {type(e).__name__}: {e}", flush=True)
        return 2
    print(f"### CONECTADO (psk={'si' if PSK else 'no'})", flush=True)

    try:
        info = await cli.device_info()
        print("### device_info:", info, flush=True)
    except Exception as e:
        print("### device_info ERR:", e, flush=True)

    try:
        ents, svcs = await cli.list_entities_services()
        print(f"### entidades: {len(ents)}", flush=True)
        for e in ents:
            nm = getattr(e, "name", "?")
            if any(k in nm.lower() for k in ("dc_voltage", "voltage_1", "type_definition", "battery_state_of_charge", "energy_today", "temperature")):
                print(f"    - {nm}", flush=True)
    except Exception as e:
        print("### list_entities ERR:", e, flush=True)

    try:
        cli.subscribe_states(on_state)
    except Exception as e:
        print("### subscribe_states ERR:", e, flush=True)

    try:
        cli.subscribe_logs(on_log, log_level=LogLevel.LOG_LEVEL_DEBUG)
        print("### suscrito a logs (DEBUG)", flush=True)
    except Exception as e:
        print("### subscribe_logs ERR:", e, flush=True)

    t0 = time.time()
    while not stop.is_set() and (time.time() - t0) < DUR:
        await asyncio.sleep(0.5)

    try:
        await cli.disconnect()
    except Exception:
        pass
    print("### fin", flush=True)
    return 0


if __name__ == "__main__":
    try:
        rc = asyncio.run(run())
    except KeyboardInterrupt:
        rc = 0
    sys.exit(rc or 0)
