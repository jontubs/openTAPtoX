# Refactor Safety Net

Baseline for the controller-neutral refactor effort as of `2026-05-07`.

## Automated Checks

Run this before and after each extraction step:

```sh
bash tools/check_repo.sh
```

This currently verifies:

- power semantics invariants via `tools/check_power_semantics.sh`
- successful `ESP32-C6` compile for `esp32:esp32:esp32c6`

## Manual Smoke Checklist

After flashing the affected target, verify the following on real hardware:

- the device boots and stays reachable on serial without repeated resets
- live TAP reception continues without multi-second gaps
- the web UI root page loads
- `/api/status` returns valid JSON with gateway/system fields
- `/api/live-frame` updates while TAP traffic is present
- `/api/power` returns valid JSON with node power rows
- `/api/node-map` and `/api/panel-map` return valid JSON
- `/api/mqtt-settings` returns the active runtime MQTT configuration
- MQTT status topics update under `openTAPtoX/status/...`
- per-node telemetry updates under `openTAPtoX/telemetry/...`
- Home Assistant discovery topics update under `homeassistant/sensor/...`
- per-node `power` remains present
- redundant per-node `power_in_w` does not reappear
- stale legacy state topics under `openTAPtoX/live/...` are not recreated
- normal monitoring works without the original Tigo CCA connected

## Recommended Refactor Rhythm

For each extraction step:

1. move one narrow responsibility only
2. run `bash tools/check_repo.sh`
3. flash the changed target
4. run the manual smoke checklist
5. only then continue to the next extraction
