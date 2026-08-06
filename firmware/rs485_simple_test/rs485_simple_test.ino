#include <Arduino.h>

static const uint32_t USB_BAUD = 115200;
static const uint32_t DEFAULT_RS485_BAUD = 115200;
static const int RS485_RX_PIN = 20;
static const int RS485_TX_PIN = 19;
// Set to the GPIO wired to DE and /RE for non-auto-direction RS485 modules.
// Keep -1 for auto-direction modules.
static const int RS485_DIR_PIN = -1;
static const bool RS485_DIR_ACTIVE_HIGH = true;
static const bool PERIODIC_RS485_TX_ENABLED = true;
static const uint32_t PERIODIC_TX_EVERY_MS = 1000;
static const uint32_t STATUS_EVERY_MS = 5000;
static const uint32_t ACK_DELAY_MS = 25;
static const size_t RX_LINE_LEN = 160;
static const size_t USB_LINE_LEN = 96;
static const bool LOG_RX_BYTES = false;

static uint32_t nextTxMs = 0;
static uint32_t txPhaseMs = 100;
static uint32_t lastStatusMs = 0;
static uint32_t currentRs485Baud = DEFAULT_RS485_BAUD;
static uint32_t txCount = 0;
static uint32_t rxCount = 0;
static uint32_t helloRxCount = 0;
static uint32_t ackRxCount = 0;
static uint32_t ackTxCount = 0;
static uint32_t overflowCount = 0;
static uint32_t malformedCount = 0;
static uint32_t baudChangeCount = 0;
static char boardId[24] = "";
static char rxLine[RX_LINE_LEN] = "";
static size_t rxLineLen = 0;
static char usbLine[USB_LINE_LEN] = "";
static size_t usbLineLen = 0;

static void setRs485TransmitEnabled(bool enabled) {
  if (RS485_DIR_PIN < 0) {
    return;
  }
  digitalWrite(RS485_DIR_PIN, enabled == RS485_DIR_ACTIVE_HIGH ? HIGH : LOW);
}

static void rs485WriteByte(uint8_t value) {
  setRs485TransmitEnabled(true);
  Serial1.write(value);
  Serial1.flush();
  setRs485TransmitEnabled(false);
}

static void rs485Print(const char* text) {
  setRs485TransmitEnabled(true);
  Serial1.print(text);
}

static void rs485Print(unsigned long value) {
  setRs485TransmitEnabled(true);
  Serial1.print(value);
}

static void rs485FlushToReceive() {
  Serial1.flush();
  setRs485TransmitEnabled(false);
}

static void printHexByte(uint8_t value) {
  if (value < 0x10) {
    Serial.print('0');
  }
  Serial.print(value, HEX);
}

static void resetCounters() {
  nextTxMs = millis() + txPhaseMs;
  lastStatusMs = millis();
  txCount = 0;
  rxCount = 0;
  helloRxCount = 0;
  ackRxCount = 0;
  ackTxCount = 0;
  overflowCount = 0;
  malformedCount = 0;
  rxLineLen = 0;
  rxLine[0] = '\0';
}

static void beginRs485(uint32_t baud) {
  setRs485TransmitEnabled(false);
  Serial1.flush();
  Serial1.end();
  delay(20);
  Serial1.setRxBufferSize(2048);
  Serial1.begin(baud, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  currentRs485Baud = baud;
  ++baudChangeCount;
  resetCounters();
  Serial.print("RS485_BAUD_SET baud=");
  Serial.println(currentRs485Baud);
}

static void printStatus(const char* prefix) {
  Serial.print(prefix);
  Serial.print(" board_id=");
  Serial.print(boardId);
  Serial.print(" baud=");
  Serial.print(currentRs485Baud);
  Serial.print(" tx_phase_ms=");
  Serial.print(txPhaseMs);
  Serial.print(" tx=");
  Serial.print(txCount);
  Serial.print(" rx_bytes=");
  Serial.print(rxCount);
  Serial.print(" hello_rx=");
  Serial.print(helloRxCount);
  Serial.print(" ack_tx=");
  Serial.print(ackTxCount);
  Serial.print(" ack_rx=");
  Serial.print(ackRxCount);
  Serial.print(" overflow=");
  Serial.print(overflowCount);
  Serial.print(" malformed=");
  Serial.print(malformedCount);
  Serial.print(" baud_changes=");
  Serial.print(baudChangeCount);
  Serial.println();
}

static void serviceRs485Rx() {
  while (Serial1.available() > 0) {
    const int ch = Serial1.read();
    if (ch < 0) {
      return;
    }
    ++rxCount;
    const char c = (char)ch;
    if (LOG_RX_BYTES) {
      Serial.print("UART_RX_BYTE count=");
      Serial.print(rxCount);
      Serial.print(" hex=");
      printHexByte((uint8_t)ch);
      Serial.print(" ascii=");
      if (c >= 32 && c <= 126) {
        Serial.write(c);
      } else {
        Serial.print('.');
      }
      Serial.println();
    }

    if (c == '\n' || c == '\r') {
      if (rxLineLen > 0) {
        rxLine[rxLineLen] = '\0';
        Serial.print("UART_LINE ");
        Serial.println(rxLine);
        if (strncmp(rxLine, "HELLO ", 6) == 0) {
          ++helloRxCount;
          delay(ACK_DELAY_MS);
          rs485Print("ACK from=");
          rs485Print(boardId);
          rs485Print(" rx_count=");
          rs485Print((unsigned long)rxCount);
          rs485Print(" for=\"");
          rs485Print(rxLine);
          rs485Print("\"\r\n");
          rs485FlushToReceive();
          ++ackTxCount;
          Serial.println("UART_ACK sent");
        } else if (strncmp(rxLine, "ACK ", 4) == 0) {
          ++ackRxCount;
        } else {
          ++malformedCount;
        }
        rxLineLen = 0;
      }
    } else if (rxLineLen + 1 < sizeof(rxLine)) {
      rxLine[rxLineLen++] = c;
    } else {
      ++overflowCount;
      rxLineLen = 0;
      Serial.println("UART_LINE overflow_reset");
    }
  }
}

static void printHelp() {
  Serial.println("USB commands:");
  Serial.println("@B <baud>  set RS485 baud on this controller");
  Serial.println("@C         clear counters");
  Serial.println("@S         print status");
  Serial.println("@H         print help");
}

static void handleUsbCommand(const char* line) {
  if (line[0] != '@') {
    for (const char* p = line; *p != '\0'; ++p) {
      rs485WriteByte((uint8_t)*p);
    }
    rs485WriteByte('\r');
    rs485WriteByte('\n');
    Serial.print("USB_TO_RS485 line=");
    Serial.println(line);
    return;
  }

  if (line[1] == 'B' || line[1] == 'b') {
    const uint32_t baud = strtoul(line + 2, nullptr, 10);
    if (baud < 1200 || baud > 2000000UL) {
      Serial.print("RS485_BAUD_REJECTED baud=");
      Serial.println(baud);
      return;
    }
    beginRs485(baud);
    return;
  }
  if (line[1] == 'C' || line[1] == 'c') {
    resetCounters();
    Serial.println("COUNTERS_CLEARED");
    return;
  }
  if (line[1] == 'S' || line[1] == 's') {
    printStatus("STATUS");
    return;
  }
  if (line[1] == 'H' || line[1] == 'h') {
    printHelp();
    return;
  }
  Serial.print("USB_COMMAND_UNKNOWN line=");
  Serial.println(line);
}

static void serviceUsb() {
  while (Serial.available() > 0) {
    const int ch = Serial.read();
    if (ch < 0) {
      return;
    }
    const char c = (char)ch;
    if (c == '\n' || c == '\r') {
      if (usbLineLen > 0) {
        usbLine[usbLineLen] = '\0';
        handleUsbCommand(usbLine);
        usbLineLen = 0;
      }
    } else if (usbLineLen + 1 < sizeof(usbLine)) {
      usbLine[usbLineLen++] = c;
    } else {
      usbLineLen = 0;
      Serial.println("USB_LINE overflow_reset");
    }
  }
}

static void periodicRs485Tx() {
  if (!PERIODIC_RS485_TX_ENABLED) {
    return;
  }
  const uint32_t now = millis();
  if ((int32_t)(now - nextTxMs) < 0) {
    return;
  }
  nextTxMs += PERIODIC_TX_EVERY_MS;
  if ((int32_t)(now - nextTxMs) > 0) {
    nextTxMs = now + PERIODIC_TX_EVERY_MS;
  }
  ++txCount;
  rs485Print("HELLO from=");
  rs485Print(boardId);
  rs485Print(" seq=");
  rs485Print((unsigned long)txCount);
  rs485Print(" ms=");
  rs485Print((unsigned long)now);
  rs485Print("\r\n");
  rs485FlushToReceive();
  Serial.print("RS485_TX count=");
  Serial.println(txCount);
}

static void periodicStatus() {
  const uint32_t now = millis();
  if (now - lastStatusMs < STATUS_EVERY_MS) {
    return;
  }
  lastStatusMs = now;
  printStatus("STATUS");
}

void setup() {
  Serial.begin(USB_BAUD);
  delay(1000);
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(boardId,
           sizeof(boardId),
           "%04lX%08lX",
           (unsigned long)((mac >> 32) & 0xFFFFUL),
           (unsigned long)(mac & 0xFFFFFFFFUL));
  // Use the high MAC byte so the two local C6 test boards get opposite
  // half-duplex send slots instead of talking over each other.
  txPhaseMs = 150 + (uint32_t)(((mac >> 40) & 0x01ULL) * 500ULL);
  Serial.println();
  Serial.println("RS485_SIMPLE_TEST UART_PRETEST");
  Serial.print("usb_baud=");
  Serial.println(USB_BAUD);
  Serial.print("rs485_default_baud=");
  Serial.println(DEFAULT_RS485_BAUD);
  Serial.print("rx_pin=");
  Serial.println(RS485_RX_PIN);
  Serial.print("tx_pin=");
  Serial.println(RS485_TX_PIN);
  Serial.print("dir_pin=");
  Serial.println(RS485_DIR_PIN);
  Serial.print("dir_active_high=");
  Serial.println(RS485_DIR_ACTIVE_HIGH ? "true" : "false");
  Serial.print("board_id=");
  Serial.println(boardId);
  Serial.print("tx_phase_ms=");
  Serial.println(txPhaseMs);
  Serial.println("wiring: ESP RX20 <- local RS485 RO, ESP TX19 -> local RS485 DI");
  Serial.println("wiring: local RS485 A/B <-> peer RS485 A/B, common GND recommended");

  if (RS485_DIR_PIN >= 0) {
    pinMode(RS485_DIR_PIN, OUTPUT);
    setRs485TransmitEnabled(false);
  }
  beginRs485(DEFAULT_RS485_BAUD);
  printHelp();
}

void loop() {
  serviceUsb();
  serviceRs485Rx();
  periodicRs485Tx();
  periodicStatus();
  delay(1);
}
