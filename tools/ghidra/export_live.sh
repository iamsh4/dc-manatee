#!/bin/sh
# Re-export listing/decompilation from the running Ghidra GUI via GhidraMCP.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
rm -rf "$ROOT/out/ghidra/functions"
curl -s -m 900 -X POST http://127.0.0.1:8089/run_ghidra_script \
  -H 'Content-Type: application/json' \
  -d "{\"script_name\": \"$ROOT/tools/ghidra/ExportManatee.java\", \"args\": \"$ROOT/out/ghidra\", \"timeout_seconds\": 900}" \
  | python3 -c 'import json,sys;d=json.load(sys.stdin);print("export ok" if d["success"] else d["console_output"][-2000:])'
