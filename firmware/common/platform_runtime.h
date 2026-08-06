#ifndef OPENTAPTOX_PLATFORM_RUNTIME_H
#define OPENTAPTOX_PLATFORM_RUNTIME_H

#include <Arduino.h>

class PlatformPersistentStore {
 public:
  virtual ~PlatformPersistentStore() {}
  virtual bool begin() = 0;
  virtual bool ready() const = 0;
  virtual size_t loadBytes(const char* key, void* out, size_t len) = 0;
  virtual bool saveBytes(const char* key, const void* data, size_t len) = 0;
};

class PlatformRs485Port {
 public:
  virtual ~PlatformRs485Port() {}
  virtual void begin() = 0;
  virtual void prepareReceive() = 0;
  virtual Stream& stream() = 0;
  virtual void setTransmitMode(bool tx) = 0;
  virtual void flushTransmit() = 0;
};

class PlatformRuntime {
 public:
  virtual ~PlatformRuntime() {}
  virtual uint32_t millis32() const = 0;
  virtual void delayMilliseconds(uint32_t ms) const = 0;
  virtual void delayMicrosecondsExact(uint32_t us) const = 0;
  virtual void yieldNow() const = 0;
  virtual void fillIpAddress(bool apMode, char* out, size_t outLen) const = 0;
  virtual void fillResetReason(char* out, size_t outLen) const = 0;
  virtual uint32_t instanceId24() const = 0;
  virtual PlatformPersistentStore& persistentStore() = 0;
  virtual PlatformRs485Port& rs485Port() = 0;
};

static inline void otxBuildMqttClientId(const PlatformRuntime& runtime,
                                        const char* hostname,
                                        char* out,
                                        size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }
  snprintf(out, outLen, "%s-%06X", hostname ? hostname : "opentaptox",
           (unsigned)runtime.instanceId24());
  out[outLen - 1] = '\0';
}

#endif
