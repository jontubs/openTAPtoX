# Refactor Target Structure

Target structure after moving the project to `ESP32-C6` only.

## Goals

- keep `firmware/openTAPtoX_esp32c6/openTAPtoX_esp32c6.ino` as the permanent Arduino entry point
- let a new reader understand the firmware lifecycle by reading the `.ino`
- keep generic TAP, MQTT, persistence, web asset and utility logic in `firmware/common/`
- keep ESP32-C6 runtime details in `firmware/openTAPtoX_esp32c6/`
- keep the repository focused on the current ESP32-C6 CCA-replacement build

## Intended Reading Experience

The `.ino` file should stay short and tell the story at a glance:

1. start the controller/runtime
2. start gateway services
3. run one gateway cycle in `loop()`

The `.ino` file should not contain protocol parsing, MQTT formatting, JSON
building, persistence encoding or web API logic.

## Stable Top-Level Responsibilities

### `firmware/openTAPtoX_esp32c6/openTAPtoX_esp32c6.ino`

- permanent Arduino entry point for the `ESP32-C6`
- delegates setup/loop to the ESP32-C6 app wrapper

### `firmware/openTAPtoX_esp32c6/opentaptox_esp32c6_app.cpp`

- wires together shared gateway logic with the ESP32-C6 runtime adapter
- owns orchestration, web-server registration and target glue that has not yet
  moved into shared modules
- should shrink over time as generic application behavior moves into
  `firmware/common/`

### `firmware/openTAPtoX_esp32c6/platform_runtime.h`

- native UART / RS485 access
- `Preferences` / NVS persistence backend
- board/system helpers such as timing, yield, reset reason and instance ID
- concrete ESP32-C6 pin wiring

## ESP32-C6 Hardware Boundary

- active firmware target: `ESP32-C6`
- RS485 UART: `TIGO_RS485_UART_PORT = 1`
- RS485 RX pin: `GPIO20`
- RS485 TX pin: `GPIO19`
- OTA: ArduinoOTA enabled when the controller is connected to WiFi
- persistence: `Preferences` namespace configured in `config.h`
- original Tigo CCA: not required for normal operation

## Shared Modules Under `firmware/common/`

These responsibilities are controller-neutral and should stay shared:

- TAP frame parsing and protocol helpers
- power/freshness/aggregate calculations
- MQTT topic building, telemetry publishing and Home Assistant discovery helpers
- MQTT runtime helper logic where it does not depend on concrete networking
- web UI static assets
- persistence encoding/decoding
- shared data models and utility helpers

## Remaining Gap To Target State

The ESP32-C6 app wrapper is still large and still owns much of the effective
gateway application core.

Major responsibilities still local in `opentaptox_esp32c6_app.cpp`:

- MQTT retained-state cleanup above the generic helper level
- Home Assistant discovery orchestration
- web/API JSON rendering and endpoint handlers
- node-map / power-slot mutation and lookup logic
- TAP command orchestration and boot-command sequencing
- CCA-compatible startup sequence and fallback to normal TAP polling
- RS485 receive/service flow and decoded-frame dispatch

## Practical Boundary Rule

Move code to `firmware/common/` only if it:

- does not care about ESP32-C6 pins, UART objects or concrete web-server classes
- can work through a thin runtime adapter for time, IO, persistence or reset
- can be tested by compiling the ESP32-C6 target without changing behavior

Keep code in the ESP32-C6 target directory if it directly depends on:

- board pins, UART mode or serial implementation
- `Preferences` / NVS setup details
- ESP32-specific network/server behavior
- hardware timing quirks that still need real-device verification

## Next Recommended Step

- move retained-state cleanup and Home Assistant discovery publishing further
  into shared MQTT modules
- keep only connect side effects and target-only web-server wiring local
- delay TAP command state-machine and RS485 receive-path extraction until the
  higher-level MQTT/web duplication is reduced further

## Verification

Run this after each extraction or behavior change:

```sh
bash tools/check_repo.sh
```

For hardware changes, additionally flash the ESP32-C6 and verify:

- web UI loads
- `/api/status` returns valid JSON
- MQTT heartbeat and telemetry update
- OTA service appears as `opentaptox-esp32c6.local` on `_arduino._tcp`
- RS485 traffic is received on `GPIO20`/`GPIO19`
