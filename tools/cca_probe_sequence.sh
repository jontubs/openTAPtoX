#!/usr/bin/env bash
set -euo pipefail

BASE_URL="${1:-}"
shift || true

MAX_AUTO_NODES="${MAX_AUTO_NODES:-3}"
WAIT_TIMEOUT_SEC="${WAIT_TIMEOUT_SEC:-8}"
SLEEP_BETWEEN_SEC="${SLEEP_BETWEEN_SEC:-0.2}"

fail() {
  echo "cca probe failed: $*" >&2
  exit 1
}

require_command() {
  local cmd="$1"
  command -v "$cmd" >/dev/null 2>&1 || fail "missing required command: $cmd"
}

usage() {
  cat <<'EOF'
Usage:
  bash tools/cca_probe_sequence.sh http://device-host-or-ip [node_id...]

Behavior:
  - runs a small active-control probe sequence against openTAPtoX
  - if no node ids are provided, auto-discovers up to MAX_AUTO_NODES from /api/node-map
  - waits for command_busy=false between commands

Environment:
  MAX_AUTO_NODES   default: 3
  WAIT_TIMEOUT_SEC default: 8
  SLEEP_BETWEEN_SEC default: 0.2
EOF
}

[[ -n "$BASE_URL" ]] || {
  usage
  exit 1
}

require_command curl
require_command python3

curl_json() {
  local path="$1"
  shift || true
  curl --silent --show-error --fail --get "$BASE_URL$path" "$@"
}

status_field() {
  local field="$1"
  local json
  json="$(curl_json /api/status)"
  python3 -c '
import json
import sys
field = sys.argv[1]
obj = json.load(sys.stdin)
value = obj.get(field, "")
if isinstance(value, bool):
    print("true" if value else "false")
elif value is None:
    print("")
else:
    print(value)
' "$field" <<<"$json"
}

wait_until_idle() {
  local start_ts now busy
  start_ts="$(date +%s)"
  while true; do
    busy="$(status_field command_busy || true)"
    if [[ "$busy" == "false" || -z "$busy" ]]; then
      return 0
    fi
    now="$(date +%s)"
    if (( now - start_ts >= WAIT_TIMEOUT_SEC )); then
      fail "device stayed busy for more than ${WAIT_TIMEOUT_SEC}s"
    fi
    sleep 0.2
  done
}

discover_nodes() {
  local json
  json="$(curl_json /api/node-map)"
  python3 -c '
import json
import sys
limit = int(sys.argv[1])
obj = json.load(sys.stdin)
nodes = []
for item in obj.get("nodes", []):
    node_id = item.get("node_id")
    if isinstance(node_id, int):
        nodes.append(node_id)
for node_id in nodes[:limit]:
    print(node_id)
' "$MAX_AUTO_NODES" <<<"$json"
}

print_status_summary() {
  local json
  json="$(curl_json /api/status)"
  python3 -c '
import json
import sys
obj = json.load(sys.stdin)
summary = {
    "command_name": obj.get("command_name"),
    "command_state": obj.get("command_state"),
    "last_pv_subcmd_hex": obj.get("last_pv_subcmd_hex"),
    "last_pv_request_hex": obj.get("last_pv_request_hex"),
    "last_pv_ack_hex": obj.get("last_pv_ack_hex"),
    "last_pv_ack_status": obj.get("last_pv_ack_status"),
    "last_pv_ack_status_hex": obj.get("last_pv_ack_status_hex"),
    "last_pv_ack_rsp_subcmd_hex": obj.get("last_pv_ack_rsp_subcmd_hex"),
    "last_pv_ack_body_hex": obj.get("last_pv_ack_body_hex"),
}
print(json.dumps(summary, separators=(",", ":")))
' <<<"$json"
}

call_probe() {
  local label="$1"
  local path="$2"
  shift 2 || true
  wait_until_idle
  echo "[probe] $label"
  curl_json "$path" "$@"
  echo
  sleep "$SLEEP_BETWEEN_SEC"
  wait_until_idle
  echo "[status] $(print_status_summary)"
}

declare -a NODE_IDS=("$@")
if [[ "${#NODE_IDS[@]}" -eq 0 ]]; then
  mapfile -t NODE_IDS < <(discover_nodes)
fi

[[ "${#NODE_IDS[@]}" -gt 0 ]] || fail "no node ids available from args or /api/node-map"

echo "[target] $BASE_URL"
echo "[nodes] ${NODE_IDS[*]}"

call_probe "radio_config" "/api/command/radio-config"
call_probe "network_status" "/api/command/network-status"
call_probe "node_table" "/api/command/node-table"

for node_id in "${NODE_IDS[@]}"; do
  call_probe "node ${node_id} text ^00Version" "/api/command/node-text" \
    --data-urlencode "nodeId=${node_id}" \
    --data-urlencode "text=^00Version" \
    --data-urlencode "appendCr=true"
  call_probe "node ${node_id} text ^00Info" "/api/command/node-text" \
    --data-urlencode "nodeId=${node_id}" \
    --data-urlencode "text=^00Info" \
    --data-urlencode "appendCr=true"
  call_probe "node ${node_id} text ^00Smrt" "/api/command/node-text" \
    --data-urlencode "nodeId=${node_id}" \
    --data-urlencode "text=^00Smrt" \
    --data-urlencode "appendCr=true"
  call_probe "node ${node_id} op 0x17/0x0F" "/api/command/node-op" \
    --data-urlencode "subcmd=0x17" \
    --data-urlencode "nodeId=${node_id}" \
    --data-urlencode "opcode=0x0F"
done
