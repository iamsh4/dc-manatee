#!/bin/sh
# Apply annotations to the Ghidra GUI via GhidraMCP (Ghidra must run with GHIDRA_MCP_ALLOW_SCRIPTS=1).
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
python3 "$ROOT/tools/gen_ghidra_types.py" >/dev/null || echo "warning: gen_ghidra_types failed (header mid-edit?), using previous build/ghidra_types.h"
curl -s -m 600 -X POST http://127.0.0.1:8089/run_ghidra_script \
  -H 'Content-Type: application/json' \
  -d "{\"script_name\": \"$ROOT/tools/ghidra/ApplyManatee.java\", \"args\": \"$ROOT/tools/ghidra/annotations\", \"timeout_seconds\": 600}"
echo
