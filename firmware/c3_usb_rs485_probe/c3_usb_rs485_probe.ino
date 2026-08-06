#include <Arduino.h>

static const uint32_t USB_BAUD = 115200;
static const uint32_t RS485_BAUD = 38400;
static const int RS485_RX_PIN = 6;
static const int RS485_TX_PIN = 5;

static uint32_t rxBytes = 0;
static uint32_t lastStatusMs = 0;

void setup() {
  Serial.begin(USB_BAUD);
  Serial1.setRxBufferSize(2048);
  Serial1.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  delay(250);
  Serial.println("C3_USB_RS485_PROBE boot rx=6 tx=5 baud=38400");
}

void loop() {
  while (Serial1.available() > 0) {
    const int value = Serial1.read();
    if (value < 0) {
      break;
    }
    ++rxBytes;
    Serial.printf("RX t=%lu count=%lu byte=%02X\r\n",
                  (unsigned long)millis(),
                  (unsigned long)rxBytes,
                  (unsigned int)(uint8_t)value);
  }

  const uint32_t now = millis();
  if (now - lastStatusMs >= 1000) {
    lastStatusMs = now;
    Serial.printf("STATUS t=%lu usb=ok rx_pin=6 tx_pin=5 rx_bytes=%lu\r\n",
                  (unsigned long)now,
                  (unsigned long)rxBytes);
  }
  delay(1);
}
