#ifndef OPENTAPTOX_UTILS_H
#define OPENTAPTOX_UTILS_H

#include <Arduino.h>
#include <string.h>
#include <stdarg.h>

// This shared header depends on target-specific limits from config.h.
// Include config.h before including this file.

static uint32_t fnv1a32(const uint8_t* data, size_t len) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < len; ++i) {
    hash ^= data[i];
    hash *= 16777619UL;
  }
  return hash;
}

static void copyString(char* dst, size_t dstLen, const char* src) {
  if (dstLen == 0) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, dstLen - 1);
  dst[dstLen - 1] = '\0';
}

static size_t appendToBuffer(char* dst, size_t dstLen, size_t pos, const char* src) {
  if (dstLen == 0 || src == nullptr) {
    return pos;
  }
  while (*src && (pos + 1) < dstLen) {
    dst[pos++] = *src++;
  }
  dst[(pos < dstLen) ? pos : (dstLen - 1)] = '\0';
  return pos;
}

static size_t appendFormat(char* dst, size_t dstLen, size_t pos, const char* fmt, ...) {
  if (dstLen == 0 || pos >= dstLen) {
    return pos;
  }
  va_list ap;
  va_start(ap, fmt);
  const int written = vsnprintf(dst + pos, dstLen - pos, fmt, ap);
  va_end(ap);
  if (written < 0) {
    dst[dstLen - 1] = '\0';
    return pos;
  }
  const size_t next = pos + (size_t)written;
  if (next >= dstLen) {
    dst[dstLen - 1] = '\0';
    return dstLen - 1;
  }
  return next;
}

static size_t appendJsonEscaped(char* dst, size_t dstLen, size_t pos, const char* s) {
  if (dstLen == 0 || s == nullptr) {
    return pos;
  }
  while (*s && (pos + 1) < dstLen) {
    const char c = *s++;
    switch (c) {
      case '"':
        pos = appendToBuffer(dst, dstLen, pos, "\\\"");
        break;
      case '\\':
        pos = appendToBuffer(dst, dstLen, pos, "\\\\");
        break;
      case '\b':
        pos = appendToBuffer(dst, dstLen, pos, "\\b");
        break;
      case '\f':
        pos = appendToBuffer(dst, dstLen, pos, "\\f");
        break;
      case '\n':
        pos = appendToBuffer(dst, dstLen, pos, "\\n");
        break;
      case '\r':
        pos = appendToBuffer(dst, dstLen, pos, "\\r");
        break;
      case '\t':
        pos = appendToBuffer(dst, dstLen, pos, "\\t");
        break;
      default:
        if ((uint8_t)c < 0x20) {
          pos = appendFormat(dst, dstLen, pos, "\\u%04x", (unsigned int)(uint8_t)c);
        } else {
          dst[pos++] = c;
          dst[pos] = '\0';
        }
        break;
    }
  }
  dst[(pos < dstLen) ? pos : (dstLen - 1)] = '\0';
  return pos;
}

static void jsonEscapeToBuffer(const char* s, char* dst, size_t dstLen) {
  if (dstLen == 0) {
    return;
  }
  dst[0] = '\0';
  appendJsonEscaped(dst, dstLen, 0, s);
}

static void formatHex4(uint16_t value, char* dst, size_t dstLen) {
  if (dstLen == 0) {
    return;
  }
  snprintf(dst, dstLen, "0x%04X", value);
  dst[dstLen - 1] = '\0';
}

static void formatUnsigned32(uint32_t value, char* dst, size_t dstLen) {
  if (dstLen == 0) {
    return;
  }
  snprintf(dst, dstLen, "%lu", (unsigned long)value);
  dst[dstLen - 1] = '\0';
}

static void formatUnsigned16(uint16_t value, char* dst, size_t dstLen) {
  if (dstLen == 0) {
    return;
  }
  snprintf(dst, dstLen, "%u", (unsigned)value);
  dst[dstLen - 1] = '\0';
}

static void formatInt32(int32_t value, char* dst, size_t dstLen) {
  if (dstLen == 0) {
    return;
  }
  snprintf(dst, dstLen, "%ld", (long)value);
  dst[dstLen - 1] = '\0';
}

static void formatFloat(float value, uint8_t decimals, char* dst, size_t dstLen) {
  if (dstLen == 0) {
    return;
  }
  snprintf(dst, dstLen, "%.*f", (int)decimals, (double)value);
  dst[dstLen - 1] = '\0';
}

static void slugifyIdentifierToBuffer(const char* value, char* dst, size_t dstLen) {
  if (dstLen == 0) {
    return;
  }
  size_t pos = 0;
  bool lastUnderscore = false;
  if (value != nullptr) {
    while (*value && (pos + 1) < dstLen) {
      char c = *value++;
      if (c >= 'A' && c <= 'Z') {
        c = (char)(c - 'A' + 'a');
      }
      const bool isAlphaNum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
      if (isAlphaNum) {
        dst[pos++] = c;
        lastUnderscore = false;
        continue;
      }
      if (!lastUnderscore && pos > 0 && (pos + 1) < dstLen) {
        dst[pos++] = '_';
        lastUnderscore = true;
      }
    }
  }
  while (pos > 0 && dst[pos - 1] == '_') {
    --pos;
  }
  if (pos == 0) {
    copyString(dst, dstLen, "item");
    return;
  }
  dst[pos] = '\0';
}

static uint16_t panelFieldCountClamped() {
  if (TIGO_PANEL_FIELD_COUNT < 1) {
    return 1;
  }
  if (TIGO_PANEL_FIELD_COUNT > TIGO_MAX_OPTIMIZERS) {
    return TIGO_MAX_OPTIMIZERS;
  }
  return TIGO_PANEL_FIELD_COUNT;
}

static uint16_t clampPanelFieldCountValue(uint16_t count) {
  if (count < 1) {
    return 1;
  }
  if (count > TIGO_MAX_OPTIMIZERS) {
    return TIGO_MAX_OPTIMIZERS;
  }
  return count;
}

static void makeDefaultPanelLabel(size_t index, char* dst, size_t dstLen) {
  if (dstLen == 0) {
    return;
  }
  snprintf(dst, dstLen, "A%u", (unsigned)(index + 1U));
  dst[dstLen - 1] = '\0';
}

static void makePanelDisplayName(const char* label, char* dst, size_t dstLen) {
  if (dstLen == 0) {
    return;
  }
  if (label == nullptr || label[0] == '\0') {
    dst[0] = '\0';
    return;
  }
  snprintf(dst, dstLen, "Tigo %s", label);
  dst[dstLen - 1] = '\0';
}

static String jsonEscape(const char* s) {
  char tmp[256];
  jsonEscapeToBuffer(s, tmp, sizeof(tmp));
  return String(tmp);
}

static String jsonEscape(const String& s) {
  return jsonEscape(s.c_str());
}

static String hex4(uint16_t value) {
  char tmp[7];
  formatHex4(value, tmp, sizeof(tmp));
  return String(tmp);
}

static String hex8(uint32_t value) {
  char buf[9];
  snprintf(buf, sizeof(buf), "%08lX", (unsigned long)value);
  return String(buf);
}

static String slugifyIdentifier(const String& value) {
  char tmp[96];
  slugifyIdentifierToBuffer(value.c_str(), tmp, sizeof(tmp));
  return String(tmp);
}

static bool hexNibble(char c, uint8_t& out) {
  if (c >= '0' && c <= '9') {
    out = (uint8_t)(c - '0');
    return true;
  }
  if (c >= 'A' && c <= 'F') {
    out = (uint8_t)(10 + c - 'A');
    return true;
  }
  if (c >= 'a' && c <= 'f') {
    out = (uint8_t)(10 + c - 'a');
    return true;
  }
  return false;
}

static bool hex16ToBytes(const char* hex, uint8_t out[8]) {
  if (hex == nullptr) {
    return false;
  }
  if (strlen(hex) != 16) {
    return false;
  }
  for (size_t i = 0; i < 8; ++i) {
    uint8_t hi = 0;
    uint8_t lo = 0;
    if (!hexNibble(hex[i * 2], hi) || !hexNibble(hex[i * 2 + 1], lo)) {
      return false;
    }
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

static bool hexTextToBytes(const char* text,
                           uint8_t* out,
                           size_t outCap,
                           size_t* outLen) {
  if (outLen != nullptr) {
    *outLen = 0;
  }
  if (text == nullptr || out == nullptr) {
    return false;
  }
  size_t count = 0;
  bool haveHighNibble = false;
  uint8_t highNibble = 0;
  while (*text) {
    const char c = *text++;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
        c == ':' || c == '-' || c == '_') {
      continue;
    }
    uint8_t nibble = 0;
    if (!hexNibble(c, nibble)) {
      return false;
    }
    if (!haveHighNibble) {
      highNibble = nibble;
      haveHighNibble = true;
      continue;
    }
    if (count >= outCap) {
      return false;
    }
    out[count++] = (uint8_t)((highNibble << 4) | nibble);
    haveHighNibble = false;
  }
  if (haveHighNibble) {
    return false;
  }
  if (outLen != nullptr) {
    *outLen = count;
  }
  return true;
}

static bool bitArrayGet(const uint8_t* bits, size_t index) {
  return (bits[index >> 3] & (uint8_t)(1U << (index & 7U))) != 0;
}

static void bitArraySet(uint8_t* bits, size_t index, bool value) {
  const uint8_t mask = (uint8_t)(1U << (index & 7U));
  if (value) {
    bits[index >> 3] |= mask;
  } else {
    bits[index >> 3] &= (uint8_t)~mask;
  }
}

#endif
