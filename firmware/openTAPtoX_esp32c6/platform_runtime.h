#ifndef OPENTAPTOX_ESP32C6_PLATFORM_RUNTIME_H
#define OPENTAPTOX_ESP32C6_PLATFORM_RUNTIME_H

#include <Arduino.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>

#include <otx_common_platform_runtime.h>
#include "config.h"

class Esp32c6PreferencesStore : public PlatformPersistentStore {
 public:
  bool begin() override {
    ready_ = prefs_.begin(TIGO_PREFS_NAMESPACE, false);
    return ready_;
  }

  bool ready() const override {
    return ready_;
  }

  size_t loadBytes(const char* key, void* out, size_t len) override {
    if (!ready_ || key == nullptr || out == nullptr) {
      return 0;
    }
    return prefs_.getBytes(key, out, len);
  }

  bool saveBytes(const char* key, const void* data, size_t len) override {
    if (!ready_ || key == nullptr || data == nullptr) {
      return false;
    }
    return prefs_.putBytes(key, data, len) == len;
  }

 private:
  Preferences prefs_;
  bool ready_ = false;
};

class Esp32c6Rs485Port : public PlatformRs485Port {
 public:
  Esp32c6Rs485Port() : serial_(TIGO_RS485_UART_PORT) {}

  void begin() override {
    if (TIGO_RS485_DIR_PIN >= 0) {
      pinMode(TIGO_RS485_DIR_PIN, OUTPUT);
      setTransmitMode(false);
    }
    serial_.setRxBufferSize(TIGO_RS485_RX_BUFFER_BYTES);
    serial_.begin(TIGO_RS485_BAUD, SERIAL_8N1, TIGO_RS485_RX_PIN, TIGO_RS485_TX_PIN);
  }

  void prepareReceive() override {}

  Stream& stream() override {
    return serial_;
  }

  void setTransmitMode(bool tx) override {
    if (TIGO_RS485_DIR_PIN < 0) {
      (void)tx;
      return;
    }
    const uint8_t level = (tx == TIGO_RS485_DIR_ACTIVE_HIGH) ? HIGH : LOW;
    digitalWrite(TIGO_RS485_DIR_PIN, level);
  }

  void flushTransmit() override {
    serial_.flush();
  }

 private:
  HardwareSerial serial_;
};

class Esp32c6PlatformRuntime : public PlatformRuntime {
 public:
  uint32_t millis32() const override {
    return millis();
  }

  void delayMilliseconds(uint32_t ms) const override {
    delay(ms);
  }

  void delayMicrosecondsExact(uint32_t us) const override {
    delayMicroseconds(us);
  }

  void yieldNow() const override {
    yield();
  }

  void fillIpAddress(bool apMode, char* out, size_t outLen) const override {
    if (out == nullptr || outLen == 0) {
      return;
    }
    const IPAddress ip = apMode ? WiFi.softAPIP() : WiFi.localIP();
    snprintf(out, outLen, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    out[outLen - 1] = '\0';
  }

  void fillResetReason(char* out, size_t outLen) const override {
    if (out == nullptr || outLen == 0) {
      return;
    }
    snprintf(out, outLen, "esp_reset_reason=%d", (int)esp_reset_reason());
    out[outLen - 1] = '\0';
  }

  uint32_t instanceId24() const override {
    return (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFULL);
  }

  PlatformPersistentStore& persistentStore() override {
    return store_;
  }

  PlatformRs485Port& rs485Port() override {
    return rs485_;
  }

 private:
  Esp32c6PreferencesStore store_;
  Esp32c6Rs485Port rs485_;
};

#endif
