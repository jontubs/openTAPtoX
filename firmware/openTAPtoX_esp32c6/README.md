# Firmware Build Notes

This directory contains the `openTAPtoX` firmware for both supported boards:

- `ESP32-C6`: preferred for new installations; RS485 uses `GPIO20` RX and `GPIO19` TX.
- `ESP32-C3`: tested compact option; RS485 uses `GPIO6` RX and `GPIO5` TX so USB remains available.

## Configure

Copy `secrets.example.h` to `secrets.h` and enter local WiFi and MQTT settings. The real `secrets.h` is intentionally ignored by Git.

The default MQTT base topic is board-specific:

- C6: `openTAPtoX/esp32c6`
- C3: `openTAPtoX/esp32c3`

This matters when both boards publish to the same broker.

## Build

Run from the repository root:

```powershell
arduino-cli compile --libraries "$PWD/firmware/common" --fqbn esp32:esp32:esp32c6 firmware/openTAPtoX_esp32c6
arduino-cli compile --libraries "$PWD/firmware/common" --fqbn esp32:esp32:esp32c3 firmware/openTAPtoX_esp32c6
```

For a first flash, use USB and the target board's serial port. OTA updates are enabled after the controller has joined WiFi.

## Runtime Notes

- Settings and panel mappings are stored in `Preferences`/NVS.
- The web UI exposes MQTT configuration, TAP polling, detected nodes, panel mapping, status, and a single short setup guide.
- Valid TAP replies from both normal polling and management commands keep the TAP link indicator fresh. Poll-response statistics remain separate.
- A known TAP starts through a passive, read-only warm attach. Its verified address, radio profile, node table, and receive cursor are preserved; recovery requires a diagnosed fault or explicit operator action.
- Full-duplex raw-frame diagnostics are buffered while a TAP reply is outstanding, so MQTT or web traffic cannot delay RS485 response handling.
- **Release optimizers (RSD run)** sends the verified `0x0B00 01` selector. Electrical release can take 60 to 90 seconds and must be checked at the inverter; the TAP acknowledgement alone is not proof of release.
- Normal operation needs no Tigo CCA. RF profile changes, table clearing, and commissioning recovery are advanced diagnostics; see [ALTDOKU](../../docs/ALTDOKU.md).

The main project README contains wiring, first setup, MQTT layout, and troubleshooting.
