#ifndef OPENTAPTOX_PROTOCOL_H
#define OPENTAPTOX_PROTOCOL_H

#include <Arduino.h>
#include <string.h>

#include "models.h"

static uint16_t crc16Tigo(const uint8_t* data, size_t len, uint16_t init = 0x8408) {
  uint16_t crc = init;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      if (crc & 1U) {
        crc = (crc >> 1) ^ 0x8408U;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

static bool decodeEscape(uint8_t esc, uint8_t& out) {
  switch (esc) {
    case 0x00: out = 0x7E; return true;
    case 0x01: out = 0x24; return true;
    case 0x02: out = 0x23; return true;
    case 0x03: out = 0x25; return true;
    case 0x04: out = 0xA4; return true;
    case 0x05: out = 0xA3; return true;
    case 0x06: out = 0xA5; return true;
    default: return false;
  }
}

static void writeEscapedByte(Stream& out, uint8_t b) {
  switch (b) {
    case 0x7E: out.write((uint8_t)0x7E); out.write((uint8_t)0x00); break;
    case 0x24: out.write((uint8_t)0x7E); out.write((uint8_t)0x01); break;
    case 0x23: out.write((uint8_t)0x7E); out.write((uint8_t)0x02); break;
    case 0x25: out.write((uint8_t)0x7E); out.write((uint8_t)0x03); break;
    case 0xA4: out.write((uint8_t)0x7E); out.write((uint8_t)0x04); break;
    case 0xA3: out.write((uint8_t)0x7E); out.write((uint8_t)0x05); break;
    case 0xA5: out.write((uint8_t)0x7E); out.write((uint8_t)0x06); break;
    default: out.write(b); break;
  }
}

class GatewayStreamParser {
 public:
  GatewayStreamParser() {
    reset();
  }

  void reset() {
    waitingForStartMarker_ = false;
    inFrame_ = false;
    escapePending_ = false;
    bodyLen_ = 0;
  }

  bool feed(uint8_t b, GatewayFrame& outFrame) {
    if (!inFrame_) {
      if (!waitingForStartMarker_) {
        waitingForStartMarker_ = (b == 0x7E);
      } else {
        if (b == 0x07) {
          inFrame_ = true;
          escapePending_ = false;
          bodyLen_ = 0;
        }
        waitingForStartMarker_ = (b == 0x7E);
      }
      return false;
    }

    if (!escapePending_) {
      if (b == 0x7E) {
        escapePending_ = true;
        return false;
      }
      if (bodyLen_ < MAX_FRAME_BODY) {
        body_[bodyLen_++] = b;
      } else {
        reset();
      }
      return false;
    }

    escapePending_ = false;
    if (b == 0x08) {
      const bool ok = parseDecodedBody(body_, bodyLen_, outFrame);
      reset();
      return ok;
    }

    if (b == 0x07) {
      inFrame_ = true;
      bodyLen_ = 0;
      return false;
    }

    uint8_t decoded = 0;
    if (!decodeEscape(b, decoded)) {
      reset();
      return false;
    }
    if (bodyLen_ < MAX_FRAME_BODY) {
      body_[bodyLen_++] = decoded;
    } else {
      reset();
    }
    return false;
  }

 private:
  bool parseDecodedBody(const uint8_t* body, size_t len, GatewayFrame& outFrame) {
    memset(&outFrame, 0, sizeof(outFrame));
    if (len < 6) {
      return false;
    }
    const size_t dataLen = len - 2;
    const uint16_t wireCrc = (uint16_t)body[dataLen] | ((uint16_t)body[dataLen + 1] << 8);
    const uint16_t calcCrc = crc16Tigo(body, dataLen);

    const uint16_t addrRaw = ((uint16_t)body[0] << 8) | body[1];
    const uint16_t typeCode = ((uint16_t)body[2] << 8) | body[3];
    const size_t payloadLen = dataLen - 4;
    if (payloadLen > MAX_FRAME_PAYLOAD) {
      return false;
    }

    outFrame.valid = true;
    outFrame.addrRaw = addrRaw;
    outFrame.fromGateway = (addrRaw & 0x8000U) != 0;
    outFrame.gatewayId = addrRaw & 0x7FFFU;
    outFrame.typeCode = typeCode;
    outFrame.payloadLen = (uint16_t)payloadLen;
    outFrame.crcOk = (wireCrc == calcCrc);
    if (payloadLen > 0) {
      memcpy(outFrame.payload, body + 4, payloadLen);
    }
    return true;
  }

  bool waitingForStartMarker_;
  bool inFrame_;
  bool escapePending_;
  uint8_t body_[MAX_FRAME_BODY];
  size_t bodyLen_;
};

#endif
