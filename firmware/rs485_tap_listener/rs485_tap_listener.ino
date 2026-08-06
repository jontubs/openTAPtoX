#include <Arduino.h>

static const uint16_t TIGO_MAX_OPTIMIZERS = 64;
static const size_t MAX_FRAME_BODY = 512;
static const size_t MAX_FRAME_PAYLOAD = 506;
static const size_t EVENT_TEXT_LEN = 128;

#include <otx_common_models.h>
#include <otx_common_protocol.h>

static const char* LISTENER_VERSION = "2026.07.25.4-c3-c6-uart";
static const uint32_t USB_BAUD = 115200UL;
static const uint32_t RS485_BAUD = 38400UL;
#if CONFIG_IDF_TARGET_ESP32C3
static const int RS485_RX_PIN = 6;
#else
static const int RS485_RX_PIN = 20;
#endif
static const int RS485_TX_PIN = -1;
static const uint16_t RX_BUFFER_BYTES = 4096;
static const size_t RAW_CHUNK_BYTES = 128;
static const size_t USB_LINE_LEN = 80;

static GatewayStreamParser parser;
static uint32_t rawBytes = 0;
static uint32_t rawChunks = 0;
static uint32_t frames = 0;
static uint32_t crcOkFrames = 0;
static uint32_t crcBadFrames = 0;
static uint32_t lastByteMs = 0;
static uint32_t lastFrameMs = 0;
static bool rawLogging = true;
static bool parsedLogging = true;
static char usbLine[USB_LINE_LEN] = "";
static size_t usbLineLen = 0;

static void printHexByte(uint8_t value) {
  if (value < 0x10) {
    Serial.print('0');
  }
  Serial.print(value, HEX);
}

static void printHexBytes(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (i > 0) {
      Serial.print(' ');
    }
    printHexByte(data[i]);
  }
}

static void printStatus(const char* prefix) {
  Serial.print(prefix);
  Serial.print(" version=");
  Serial.print(LISTENER_VERSION);
  Serial.print(" baud=");
  Serial.print(RS485_BAUD);
  Serial.print(" rx_pin=");
  Serial.print(RS485_RX_PIN);
  Serial.print(" tx_pin=");
  Serial.print(RS485_TX_PIN);
  Serial.print(" raw_bytes=");
  Serial.print(rawBytes);
  Serial.print(" raw_chunks=");
  Serial.print(rawChunks);
  Serial.print(" frames=");
  Serial.print(frames);
  Serial.print(" crc_ok=");
  Serial.print(crcOkFrames);
  Serial.print(" crc_bad=");
  Serial.print(crcBadFrames);
  Serial.print(" last_byte_ms=");
  Serial.print(lastByteMs);
  Serial.print(" last_frame_ms=");
  Serial.print(lastFrameMs);
  Serial.print(" raw_logging=");
  Serial.print(rawLogging ? "on" : "off");
  Serial.print(" parsed_logging=");
  Serial.print(parsedLogging ? "on" : "off");
  Serial.println();
}

static void printHelp() {
  Serial.println("USB commands:");
  Serial.println("@S  print status");
  Serial.println("@C  clear counters");
  Serial.println("@R  toggle raw byte logging");
  Serial.println("@P  toggle parsed frame logging");
  Serial.println("@H  print help");
}

static void clearCounters() {
  rawBytes = 0;
  rawChunks = 0;
  frames = 0;
  crcOkFrames = 0;
  crcBadFrames = 0;
  lastByteMs = 0;
  lastFrameMs = 0;
  parser.reset();
  Serial.println("LISTENER counters_cleared");
}

static void handleUsbCommand(const char* line) {
  if (strcmp(line, "@S") == 0 || strcmp(line, "@s") == 0) {
    printStatus("STATUS");
    return;
  }
  if (strcmp(line, "@C") == 0 || strcmp(line, "@c") == 0) {
    clearCounters();
    return;
  }
  if (strcmp(line, "@R") == 0 || strcmp(line, "@r") == 0) {
    rawLogging = !rawLogging;
    Serial.print("LISTENER raw_logging=");
    Serial.println(rawLogging ? "on" : "off");
    return;
  }
  if (strcmp(line, "@P") == 0 || strcmp(line, "@p") == 0) {
    parsedLogging = !parsedLogging;
    Serial.print("LISTENER parsed_logging=");
    Serial.println(parsedLogging ? "on" : "off");
    return;
  }
  if (strcmp(line, "@H") == 0 || strcmp(line, "@h") == 0) {
    printHelp();
    return;
  }
  Serial.print("LISTENER unknown_command=");
  Serial.println(line);
  printHelp();
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
      Serial.println("LISTENER usb_line_overflow_reset");
    }
  }
}

static void logFrame(const GatewayFrame& frame) {
  ++frames;
  if (frame.crcOk) {
    ++crcOkFrames;
  } else {
    ++crcBadFrames;
  }
  lastFrameMs = millis();

  if (!parsedLogging) {
    return;
  }

  Serial.print("FRAME t=");
  Serial.print(lastFrameMs);
  Serial.print(" dir=");
  Serial.print(frame.fromGateway ? "gateway" : "tap");
  Serial.print(" addr_raw=");
  Serial.print(frame.addrRaw, HEX);
  Serial.print(" gateway=");
  Serial.print(frame.gatewayId, HEX);
  Serial.print(" type=");
  Serial.print(frame.typeCode, HEX);
  Serial.print(" crc=");
  Serial.print(frame.crcOk ? "ok" : "bad");
  Serial.print(" payload_len=");
  Serial.print(frame.payloadLen);
  Serial.print(" payload=");
  printHexBytes(frame.payload, frame.payloadLen);
  Serial.println();
}

static void serviceRs485() {
  uint8_t chunk[RAW_CHUNK_BYTES];
  size_t len = 0;
  GatewayFrame frame;

  while (Serial1.available() > 0 && len < sizeof(chunk)) {
    const int ch = Serial1.read();
    if (ch < 0) {
      break;
    }
    const uint8_t value = (uint8_t)ch;
    chunk[len++] = value;
    if (parser.feed(value, frame)) {
      logFrame(frame);
    }
  }

  if (len == 0) {
    return;
  }

  rawBytes += len;
  ++rawChunks;
  lastByteMs = millis();
  if (rawLogging) {
    Serial.print("RAW t=");
    Serial.print(lastByteMs);
    Serial.print(" len=");
    Serial.print(len);
    Serial.print(" hex=");
    printHexBytes(chunk, len);
    Serial.println();
  }
}

void setup() {
  Serial.begin(USB_BAUD);
  delay(500);
  Serial.println();
  Serial.println("RS485_TAP_LISTENER boot");
  Serial.print("version=");
  Serial.println(LISTENER_VERSION);
  Serial.print("usb_baud=");
  Serial.println(USB_BAUD);
  Serial.print("rs485_baud=");
  Serial.println(RS485_BAUD);
  Serial.print("rx_pin=");
  Serial.println(RS485_RX_PIN);
  Serial.print("tx_pin=");
  Serial.println(RS485_TX_PIN);
  Serial.println("mode=passive_rx_only");
  Serial1.setRxBufferSize(RX_BUFFER_BYTES);
  Serial1.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  printHelp();
}

void loop() {
  serviceUsb();
  serviceRs485();
  delay(1);
}
