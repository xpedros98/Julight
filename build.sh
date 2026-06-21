#!/usr/bin/env bash
# Build (and optionally run) Julight for the fenix 7X.
#
#   ./build.sh          # compile to bin/Julight.prg
#   ./build.sh run      # compile, then launch in the Connect IQ simulator
#
set -euo pipefail

SDK="$HOME/AppData/Roaming/Garmin/ConnectIQ/Sdks/connectiq-sdk-win-9.2.0-2026-06-09-92a1605b2"
KEY="$HOME/.garmin_keys/developer_key.der"
DEVICE="fenix7x"
OUT="bin/Julight.prg"

mkdir -p bin
"$SDK/bin/monkeyc.bat" -f monkey.jungle -d "$DEVICE" -o "$OUT" -y "$KEY"
echo "Built $OUT"

if [ "${1:-}" = "run" ]; then
    "$SDK/bin/connectiq.bat" &      # start the simulator
    sleep 3
    "$SDK/bin/monkeydo.bat" "$OUT" "$DEVICE"
fi
