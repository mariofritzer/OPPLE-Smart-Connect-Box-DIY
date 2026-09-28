#!/usr/bin/env python3
"""SkyBridge flashen: erkennt den angeschlossenen Chip und installiert die passende Firmware.

Aufruf:
    python tools/flash.py              (Anschluss wird automatisch gesucht)
    python tools/flash.py --port COM4  (Windows)
    python tools/flash.py --port /dev/ttyUSB0 --no-erase

Voraussetzung: esptool  ->  python -m pip install esptool
"""
import argparse
import os
import re
import subprocess
import sys

CHIPS = {
    "ESP32": "esp32",
    "ESP32-S3": "esp32s3",
    "ESP32-C3": "esp32c3",
    "ESP32-C6": "esp32c6",
    "ESP32-C5": "esp32c5",
}

HERE = os.path.dirname(os.path.abspath(__file__))
FW_DIR = os.path.join(HERE, "..", "docs", "firmware")


def esptool(args):
    cmd = [sys.executable, "-m", "esptool"] + args
    print(">", " ".join(cmd))
    return subprocess.run(cmd, capture_output=False, text=True)


def detect(port_args):
    cmd = [sys.executable, "-m", "esptool"] + port_args + ["flash_id"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = r.stdout + r.stderr
    m = re.search(r"Chip (?:is|type:)\s+(ESP32(?:-[A-Z0-9]+)?)", out)
    if not m:
        print(out)
        sys.exit("Chip nicht erkannt. Ist das Board angesteckt und der Anschluss frei? "
                 "Evtl. BOOT-Taste beim Verbinden gedrueckt halten.")
    chip = m.group(1)
    # klassischer ESP32 meldet sich z. B. als ESP32-D0WD-V3 oder ESP32-PICO-D4
    if re.match(r"ESP32-(D0|D2|U4|S0|PICO)", chip):
        chip = "ESP32"
    return chip


def main():
    ap = argparse.ArgumentParser(description="SkyBridge-Firmware flashen")
    ap.add_argument("--port", help="z. B. COM4 oder /dev/ttyUSB0 (sonst automatisch)")
    ap.add_argument("--no-erase", action="store_true", help="Flash vorher nicht loeschen (Einstellungen bleiben)")
    ap.add_argument("--baud", default="460800")
    a = ap.parse_args()

    try:
        import esptool  # noqa: F401
    except ImportError:
        sys.exit("esptool fehlt. Installieren mit:  python -m pip install esptool")

    port_args = ["--port", a.port] if a.port else []
    chip = detect(port_args)
    target = CHIPS.get(chip)
    if not target:
        sys.exit(f"Chip {chip} wird nicht unterstuetzt (kein WLAN+Bluetooth oder keine Firmware).")
    fw = os.path.normpath(os.path.join(FW_DIR, f"skybridge-{target}.bin"))
    if not os.path.exists(fw):
        sys.exit(f"Firmware nicht gefunden: {fw}")
    print(f"\nErkannt: {chip}  ->  {os.path.basename(fw)}\n")

    base = ["--chip", target] + port_args + ["--baud", a.baud]
    if not a.no_erase:
        if esptool(base + ["erase_flash"]).returncode != 0:
            sys.exit("Loeschen fehlgeschlagen.")
    if esptool(base + ["write_flash", "0x0", fw]).returncode != 0:
        sys.exit("Flashen fehlgeschlagen.")
    print("\nFertig! Board kurz ab- und wieder anstecken, dann mit dem WLAN 'SkyBridge-Setup' "
          "(Passwort 'skybridge') verbinden und http://192.168.4.1 oeffnen.")


if __name__ == "__main__":
    main()
