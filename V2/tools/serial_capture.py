#!/usr/bin/env python3
"""Robust serial logger for the BlockBoy (ESP32-S3 USB-CDC).
Automatically reconnects when the port drops on an app switch/reset
(USB-CDC re-enumerates). Logs everything to a file.
"""
import sys, time, glob
import serial
import serial.tools.list_ports as lp

LOG = sys.argv[1] if len(sys.argv) > 1 else "serial.log"
PREF = sys.argv[2] if len(sys.argv) > 2 else "COM94"

def find_port():
    # Try the preferred port first, otherwise any USB-serial-like port.
    names = [p.device for p in lp.comports()]
    if PREF in names:
        return PREF
    for p in lp.comports():
        d = (p.description or "").lower()
        if "usb" in d or "serial" in d or "cdc" in d or "jtag" in d:
            return p.device
    return names[0] if names else None

def main():
    log = open(LOG, "a", buffering=1, encoding="utf-8", errors="replace")
    log.write(f"\n=== capture gestart {time.strftime('%H:%M:%S')} ===\n")
    while True:
        port = find_port()
        if not port:
            time.sleep(0.5); continue
        try:
            ser = serial.Serial(port, 115200, timeout=1)
        except Exception:
            time.sleep(0.4); continue
        log.write(f"=== verbonden op {port} {time.strftime('%H:%M:%S')} ===\n")
        buf = b""
        try:
            while True:
                data = ser.read(256)
                if data:
                    buf += data
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        log.write(line.decode("utf-8", "replace").rstrip("\r") + "\n")
        except Exception:
            try: ser.close()
            except Exception: pass
            log.write(f"=== verbinding verloren {time.strftime('%H:%M:%S')} ===\n")
            time.sleep(0.4)

if __name__ == "__main__":
    main()
