#!/bin/sh
# End-to-end check through PipeWire: play a sound on the headset, record the
# microphone for a few seconds and play the recording back on the headset.
set -e

SINK=$(pactl list short sinks | awk '/Astro_A20/ {print $2; exit}')
SRC=$(pactl list short sources | awk '/Astro_A20/ && !/monitor/ {print $2; exit}')

if [ -z "$SINK" ] || [ -z "$SRC" ]; then
	echo "Astro A20 not found in PipeWire (is the transmitter plugged in?)" >&2
	exit 1
fi

TMP=$(mktemp --suffix=.wav)
trap 'rm -f "$TMP"' EXIT

echo ">> 1/3 playing a test sound on the headset"
pw-play --target "$SINK" /usr/share/sounds/freedesktop/stereo/complete.oga

sleep 1
echo ">> 2/3 SPEAK NOW: recording 5 seconds from the microphone"
pw-record --target "$SRC" --rate 16000 --channels 1 "$TMP" &
REC=$!
sleep 5
kill "$REC"
wait "$REC" 2>/dev/null || true

echo ">> 3/3 playing the recording back on the headset"
pw-play --target "$SINK" "$TMP"
echo ">> done"
