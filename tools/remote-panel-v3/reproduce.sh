#!/bin/bash
# Run in a normal macOS Terminal: the Codex sandbox used for development denied
# loopback sockets and macOS GUI registration. No device/iPad automation here.
set -euo pipefail
cd "$(dirname "$0")/../.."
out=tools/remote-panel-v3/evidence
mkdir -p "$out"
cmake --build build/macos-md-arm64 --target mdJucePlugin_Standalone -j 8 > "$out/build.log" 2>&1
GM_REMOTE_PANEL=1 "build/macos-md-arm64/products/Release/Standalone/Gearmulator MD.app/Contents/MacOS/Gearmulator MD" > "$out/runtime.log" 2>&1 &
app_pid=$!
trap 'kill "$app_pid" 2>/dev/null || true' EXIT
port=""
for ((i=0;i<100;i++)); do
  if ! kill -0 "$app_pid" 2>/dev/null; then
    echo "Standalone exited before endpoint startup; inspect $out/runtime.log" >&2
    exit 1
  fi
  port=$(sed -n 's/.*\[remotePanel\] listening on port \([0-9]*\).*/\1/p' "$out/runtime.log" | head -n 1)
  if [[ -n "$port" ]]; then break; fi
  sleep 0.2
done
if [[ -z "$port" ]]; then echo 'No listening port after 20 s' >&2; exit 1; fi
python3 tools/remote-panel-v3/probe.py --port "$port" --output "$out" | tee "$out/probe.log"
