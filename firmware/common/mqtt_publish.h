#ifndef OPENTAPTOX_MQTT_PUBLISH_H
#define OPENTAPTOX_MQTT_PUBLISH_H

#include <Arduino.h>
#include <PubSubClient.h>

#include "mqtt_discovery.h"
#include "utils.h"

// This shared header depends on target-specific limits from config.h.
// Include config.h before including this file.

static void otxMqttMakeTopic(const char* baseTopic,
                             const char* suffix,
                             char* out,
                             size_t outLen) {
  otxMakeMqttTopic(baseTopic, suffix, out, outLen);
}

static void otxMqttMakeChildTopic(const char* prefix,
                                  const char* key,
                                  char* out,
                                  size_t outLen) {
  otxJoinTopic(prefix, key, out, outLen);
}

static bool otxMqttPublishTopic(PubSubClient& client,
                                const char* topic,
                                const char* payload,
                                bool retained) {
  if (!client.connected() || topic == nullptr || topic[0] == '\0') {
    return false;
  }
  return client.publish(topic, payload ? payload : "", retained);
}

static bool otxMqttPublishTopic(PubSubClient& client,
                                const char* topic,
                                const String& payload,
                                bool retained) {
  return otxMqttPublishTopic(client, topic, payload.c_str(), retained);
}

static bool otxMqttPublish(PubSubClient& client,
                           const char* baseTopic,
                           const char* suffix,
                           const char* payload,
                           bool retained) {
  char topic[MQTT_TOPIC_LEN];
  otxMqttMakeTopic(baseTopic, suffix, topic, sizeof(topic));
  if (topic[0] == '\0') {
    return false;
  }
  return otxMqttPublishTopic(client, topic, payload, retained);
}

static bool otxMqttPublish(PubSubClient& client,
                           const char* baseTopic,
                           const char* suffix,
                           const String& payload,
                           bool retained) {
  return otxMqttPublish(client, baseTopic, suffix, payload.c_str(), retained);
}

static bool otxMqttClearRetainedTopic(PubSubClient& client, const char* topic) {
  return otxMqttPublishTopic(client, topic, "", true);
}

static void otxMqttPublishSubScalar(PubSubClient& client,
                                    const char* baseTopic,
                                    const char* prefix,
                                    const char* key,
                                    const char* payload,
                                    bool retained) {
  char suffix[MQTT_TOPIC_LEN];
  otxMqttMakeChildTopic(prefix, key, suffix, sizeof(suffix));
  otxMqttPublish(client, baseTopic, suffix, payload, retained);
}

static void otxMqttPublishSubScalar(PubSubClient& client,
                                    const char* baseTopic,
                                    const String& prefix,
                                    const char* key,
                                    const char* payload,
                                    bool retained) {
  otxMqttPublishSubScalar(client, baseTopic, prefix.c_str(), key, payload, retained);
}

static void otxMqttPublishSubScalar(PubSubClient& client,
                                    const char* baseTopic,
                                    const String& prefix,
                                    const char* key,
                                    const String& payload,
                                    bool retained) {
  otxMqttPublishSubScalar(client, baseTopic, prefix.c_str(), key, payload.c_str(), retained);
}

static void otxMqttPublishSubScalarU32(PubSubClient& client,
                                       const char* baseTopic,
                                       const char* prefix,
                                       const char* key,
                                       uint32_t value,
                                       bool retained) {
  char payload[16];
  formatUnsigned32(value, payload, sizeof(payload));
  otxMqttPublishSubScalar(client, baseTopic, prefix, key, payload, retained);
}

static void otxMqttPublishSubScalarU16(PubSubClient& client,
                                       const char* baseTopic,
                                       const char* prefix,
                                       const char* key,
                                       uint16_t value,
                                       bool retained) {
  char payload[8];
  formatUnsigned16(value, payload, sizeof(payload));
  otxMqttPublishSubScalar(client, baseTopic, prefix, key, payload, retained);
}

static void otxMqttPublishSubScalarFloat(PubSubClient& client,
                                         const char* baseTopic,
                                         const char* prefix,
                                         const char* key,
                                         float value,
                                         uint8_t decimals,
                                         bool retained) {
  char payload[20];
  formatFloat(value, decimals, payload, sizeof(payload));
  otxMqttPublishSubScalar(client, baseTopic, prefix, key, payload, retained);
}

static bool otxMqttPublishScalar(PubSubClient& client,
                                 const char* baseTopic,
                                 const char* suffix,
                                 const char* payload,
                                 bool retained) {
  return otxMqttPublish(client, baseTopic, suffix, payload, retained);
}

static bool otxMqttPublishScalar(PubSubClient& client,
                                 const char* baseTopic,
                                 const char* suffix,
                                 const String& payload,
                                 bool retained) {
  return otxMqttPublish(client, baseTopic, suffix, payload, retained);
}

static void otxMqttPublishScalarU32(PubSubClient& client,
                                    const char* baseTopic,
                                    const char* suffix,
                                    uint32_t value,
                                    bool retained) {
  char payload[16];
  formatUnsigned32(value, payload, sizeof(payload));
  otxMqttPublishScalar(client, baseTopic, suffix, payload, retained);
}

static void otxMqttPublishScalarU16(PubSubClient& client,
                                    const char* baseTopic,
                                    const char* suffix,
                                    uint16_t value,
                                    bool retained) {
  char payload[8];
  formatUnsigned16(value, payload, sizeof(payload));
  otxMqttPublishScalar(client, baseTopic, suffix, payload, retained);
}

static void otxMqttPublishScalarFloat(PubSubClient& client,
                                      const char* baseTopic,
                                      const char* suffix,
                                      float value,
                                      uint8_t decimals,
                                      bool retained) {
  char payload[20];
  formatFloat(value, decimals, payload, sizeof(payload));
  otxMqttPublishScalar(client, baseTopic, suffix, payload, retained);
}

static void otxMqttPublishScalarHex4(PubSubClient& client,
                                     const char* baseTopic,
                                     const char* suffix,
                                     uint16_t value,
                                     bool retained) {
  char payload[7];
  formatHex4(value, payload, sizeof(payload));
  otxMqttPublishScalar(client, baseTopic, suffix, payload, retained);
}

#endif
