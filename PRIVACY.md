# Privacy and safe diagnostic sharing

openTAPtoX processes information from a private network and a physical PV installation. A diagnostic file that looks anonymous can still identify a household, network, or device.

## Never commit live data

Keep these artifacts local:

- Wi-Fi and MQTT credentials, SSIDs, hostnames, IP addresses, MAC addresses, and local account paths;
- TAP and optimizer EUI-64 values, printed barcodes, panel-to-device mappings, radio descriptors, join seeds, and network keys;
- raw RS485, serial, HTTP, MQTT, or Home Assistant captures;
- timestamped power, voltage, current, temperature, signal, or availability telemetry;
- screenshots from a live controller, broker, router, or Home Assistant instance;
- generated analysis or handover notes derived from any of the above.

The default local output locations (`data/`, `docs/analysis/`, and live screenshots under `assets/`) are ignored by Git. Before committing, run:

```console
python tools/check_public_tree.py
```

## Creating a public fixture

Prefer a small synthetic fixture over redacting a real capture. Use documentation-only hostnames, locally administered synthetic identifiers, invented timestamps, and non-operational key material. Preserve only the minimum bytes needed by the test.

Do not rely on replacing a person's name. Device identifiers, RF material, network topology, exact timestamps, and household energy curves can still be identifying.

## If data was already published

A normal deletion commit does not remove earlier Git objects. Rewrite every affected ref, force-update the remote, remove affected issues or other GitHub content, and ask collaborators to replace old clones. Treat published credentials or RF key material as compromised and rotate or recommission them where the hardware supports it.
