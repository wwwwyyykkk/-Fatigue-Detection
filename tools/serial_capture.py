# -*- coding: utf-8 -*-
"""Capture ESP32-S3 boot log via pyserial + trigger reset."""
import serial, time, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM16"
OUT  = sys.argv[2] if len(sys.argv) > 2 else "D:/Fatigue-detection/tools/bootlog.txt"
BAUD = 115200

ser = serial.Serial(PORT, BAUD, timeout=1)

# Trigger hard reset: EN (RTS) low then high.
# IMPORTANT: keep DTR (GPIO0) HIGH so the chip does NOT enter download mode.
ser.setDTR(False)
time.sleep(0.1)
ser.setRTS(True)    # EN pulled low
time.sleep(0.1)
ser.setRTS(False)   # EN released -> chip boots from flash (normal app)
time.sleep(0.5)

with open(OUT, "wb") as f:
    while True:
        data = ser.read(4096)
        if data:
            f.write(data)
            f.flush()
