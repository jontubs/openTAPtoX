#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_FILES=(
  "$REPO_DIR/firmware/openTAPtoX_esp32c6/opentaptox_esp32c6_app.cpp"
)
COMMON_FILES=(
  "$REPO_DIR/firmware/common/mqtt_telemetry.h"
)

fail() {
  echo "power semantics check failed: $1" >&2
  exit 1
}

forbid_exact_in_file() {
  local path="$1"
  local needle="$2"
  local label="$3"
  if grep -Fq -- "$needle" "$path"; then
    fail "unexpected ${label}: ${needle}"
  fi
}

expect_in_file() {
  local path="$1"
  local needle="$2"
  local label="$3"
  grep -Fq -- "$needle" "$path" || fail "missing ${label}: ${needle}"
}

forbid_publish_lines() {
  local path="$1"
  local needle="$2"
  local label="$3"
  local bad_line
  bad_line="$(grep -F -- "$needle" "$path" | grep -Fv "mqttClearRetainedTopic" | grep -Fv "makeMqttTopic" | head -n 1 || true)"
  if [[ -n "$bad_line" ]]; then
    fail "unexpected ${label}: ${bad_line#"${bad_line%%[![:space:]]*}"}"
  fi
}

check_source() {
  local path="$1"
  local name
  name="$(basename "$path")"
  expect_in_file "$path" 'slot->powerInW = vinV * iinA;' "${name} real power calculation"
  expect_in_file "$path" 'publishSummarySensor("power", String(mqttSettings_.baseTopic) + "/status/power/held_sum_input_w", "Tigo Leistung", "W", "power", true)' "${name} summary power discovery"
  expect_in_file "$path" '\"vin_v\":%s,\"vout_v\":%s,\"iin_a\":%s,\"power\":%s,' "${name} API JSON power field"
  if grep -Fq -- 'slot->powerOutEstW = voutV * iinA;' "$path"; then
    fail "${name} still derives output power from input current"
  fi
}

check_common_source() {
  local path="$1"
  local name
  name="$(basename "$path")"
  expect_in_file "$path" 'publishSubScalarFloat(suffix, "power", slot.powerInW, 3, TIGO_MQTT_RETAIN_TELEMETRY);' "${name} telemetry power publish"
}

check_forbidden_patterns() {
  local path="$1"
  local name
  name="$(basename "$path")"
  forbid_exact_in_file "$path" 'mqttPublishSubScalarFloat(suffix, "power_in_w", slot.powerInW, 3, TIGO_MQTT_RETAIN_TELEMETRY);' "${name} redundant power_in_w publish"
  forbid_exact_in_file "$path" '\"power_in_w\":%s,' "${name} redundant JSON power_in_w field"
  forbid_exact_in_file "$path" 'mqttPublishTopic((prefix + "/power_in_w").c_str(), String(slot.powerInW, 3), TIGO_MQTT_RETAIN_LEGACY_STATE);' "${name} redundant legacy power_in_w publish"
  forbid_publish_lines "$path" "power_out_est_w" "${name} power_out_est_w publish"
  forbid_publish_lines "$path" "live_sum_output_w" "${name} live_sum_output_w publish"
  forbid_publish_lines "$path" "held_sum_output_w" "${name} held_sum_output_w publish"
}

check_example() {
  local result
  result="$(awk 'BEGIN {
    vin_v = 34.75
    iin_a = 1.327
    vout_v = 5.8
    power_in_w = vin_v * iin_a
    old_wrong_power_w = vout_v * iin_a
    if ((power_in_w < 46.10) || (power_in_w > 46.14)) {
      print "unexpected power_in_w: " power_in_w
      exit 1
    }
    if ((old_wrong_power_w < 7.68) || (old_wrong_power_w > 7.72)) {
      print "unexpected old_wrong_power_w: " old_wrong_power_w
      exit 1
    }
    if (power_in_w <= old_wrong_power_w) {
      print "real input power should exceed the old wrong vout * iin value for the regression example"
      exit 1
    }
  }' 2>/dev/null)" || fail "$result"
}

for path in "${APP_FILES[@]}"; do
  check_source "$path"
done

for path in "${COMMON_FILES[@]}"; do
  check_common_source "$path"
done

for path in "${APP_FILES[@]}" "${COMMON_FILES[@]}"; do
  check_forbidden_patterns "$path"
done

check_example
echo "power semantics OK"
