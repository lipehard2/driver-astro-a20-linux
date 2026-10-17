#!/usr/bin/env python3
"""Wrap raw S16_LE mono 16 kHz microphone samples from gip_probe in a WAV file."""
import sys
import wave

if len(sys.argv) != 3:
    sys.exit(f"usage: {sys.argv[0]} mic.raw mic.wav")

with open(sys.argv[1], "rb") as f:
    data = f.read()

with wave.open(sys.argv[2], "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(16000)
    w.writeframes(data)
