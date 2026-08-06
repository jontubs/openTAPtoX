# ESP32-C6 / RS485 / TAP 24V Wiring

Recommended standard platform: ESP32-C6 Super Mini (4 MB flash).

Suggested UART pins for RS485 transceiver:
- GPIO20 (RX) -> RS485 TXD
- GPIO19 (TX) -> RS485 RXD
- 3V3 -> RS485 VCC
- GND -> RS485 GND

The ESP32-C6 controller replaces the CCA-side controller for normal local
monitoring. The Tigo CCA does not need to stay connected once the TAP bus wiring
and shared ground are correct.

Power from TAP:
- TAP 24V -> MP1584 VIN+
- TAP GND -> MP1584 VIN-
- Adjust MP1584 to 5.0V output
- MP1584 VOUT+ -> ESP32-C6 5V
- MP1584 VOUT- -> ESP32-C6 GND

Bus side:
- RS485 A -> TAP RS485 A
- RS485 B -> TAP RS485 B
- RS485 GND / ESP32-C6 GND -> TAP GND

If the RX LEDs are active but frames are incomplete or only prefixes such as
`7E 08` appear, verify common ground first. A wedged TAP/RS485 side may also need
a TAP power cycle.
