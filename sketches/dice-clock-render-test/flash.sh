#!/usr/bin/env bash
set -euo pipefail
CLI="${ARDUINO_CLI:-/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FQBN="${FQBN:-esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,FlashMode=qio,CDCOnBoot=cdc}"
if [[ ! -x "$CLI" ]]; then
    echo "arduino-cli not found at: $CLI" >&2
    echo "Set ARDUINO_CLI=/path/to/arduino-cli, or open the Arduino IDE once." >&2
    exit 1
fi
PORT="${1:-}"
if [[ -z "$PORT" ]]; then
    PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)"
fi
if [[ -z "$PORT" ]]; then
    echo "No /dev/cu.usbmodem* port found." >&2
    echo "Hold BOOT, tap RESET, release BOOT, then run this again." >&2
    exit 1
fi
echo "FQBN: $FQBN"
echo "Port: $PORT"
echo "=== build ==="
"$CLI" compile --fqbn "$FQBN" "$HERE"
echo "=== upload ==="
"$CLI" upload -p "$PORT" --fqbn "$FQBN" "$HERE"
echo "=== done ==="
