#ifndef OPENTAPTOX_MQTT_RUNTIME_H
#define OPENTAPTOX_MQTT_RUNTIME_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <ctype.h>
#include <string.h>

#include "mqtt_discovery.h"

// This shared header depends on target-specific limits from config.h.
// Include config.h before including this file.

enum class OtxMqttMaintainResult : uint8_t {
  NotReady,
  WaitingRetry,
  ConnectFailed,
  ConnectedNow,
  ConnectedReady,
};

enum class OtxMqttBoolCommandResult : uint8_t {
  NoMatch,
  InvalidPayload,
  Parsed,
};

static OtxMqttMaintainResult otxMaintainMqttConnection(PubSubClient& client,
                                                       bool wifiConnected,
                                                       const char* mqttHost,
                                                       const char* clientId,
                                                       const char* username,
                                                       const char* password,
                                                       uint32_t reconnectEveryMs,
                                                       uint32_t now,
                                                       uint32_t* lastConnectAttemptMs) {
  if (!wifiConnected || mqttHost == nullptr || mqttHost[0] == '\0') {
    return OtxMqttMaintainResult::NotReady;
  }
  if (client.connected()) {
    client.loop();
    return OtxMqttMaintainResult::ConnectedReady;
  }
  if (lastConnectAttemptMs == nullptr) {
    return OtxMqttMaintainResult::ConnectFailed;
  }
  if ((now - *lastConnectAttemptMs) < reconnectEveryMs) {
    return OtxMqttMaintainResult::WaitingRetry;
  }

  *lastConnectAttemptMs = now;
  bool ok = false;
  if (username != nullptr && username[0] != '\0') {
    ok = client.connect(clientId, username, password ? password : "");
  } else {
    ok = client.connect(clientId);
  }
  return ok ? OtxMqttMaintainResult::ConnectedNow
            : OtxMqttMaintainResult::ConnectFailed;
}

static bool otxSubscribeMqttCommand(PubSubClient& client,
                                    const char* baseTopic,
                                    const char* suffix) {
  char topic[MQTT_TOPIC_LEN];
  otxMakeMqttTopic(baseTopic, suffix, topic, sizeof(topic));
  if (topic[0] == '\0') {
    return false;
  }
  return client.subscribe(topic);
}

static bool otxParseBoolText(const char* text,
                             bool currentValue,
                             bool* outValue,
                             bool allowToggle = false) {
  if (text == nullptr || outValue == nullptr) {
    return false;
  }
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
    ++text;
  }
  char normalized[20];
  size_t pos = 0;
  while (*text && pos + 1 < sizeof(normalized)) {
    const char c = *text++;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      continue;
    }
    normalized[pos++] = (char)tolower((unsigned char)c);
  }
  normalized[pos] = '\0';
  if (strcmp(normalized, "1") == 0 ||
      strcmp(normalized, "on") == 0 ||
      strcmp(normalized, "true") == 0 ||
      strcmp(normalized, "enable") == 0 ||
      strcmp(normalized, "enabled") == 0) {
    *outValue = true;
    return true;
  }
  if (strcmp(normalized, "0") == 0 ||
      strcmp(normalized, "off") == 0 ||
      strcmp(normalized, "false") == 0 ||
      strcmp(normalized, "disable") == 0 ||
      strcmp(normalized, "disabled") == 0) {
    *outValue = false;
    return true;
  }
  if (allowToggle && strcmp(normalized, "toggle") == 0) {
    *outValue = !currentValue;
    return true;
  }
  return false;
}

static void otxDecodeMqttPayloadText(const uint8_t* payload,
                                     unsigned int length,
                                     char* out,
                                     size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }
  const size_t copyLen = (length < (outLen - 1U)) ? length : (outLen - 1U);
  if (payload != nullptr && copyLen > 0) {
    memcpy(out, payload, copyLen);
  }
  out[copyLen] = '\0';
}

static OtxMqttBoolCommandResult otxParseMqttBoolCommand(const char* baseTopic,
                                                        const char* suffix,
                                                        const char* topic,
                                                        const uint8_t* payload,
                                                        unsigned int length,
                                                        bool currentValue,
                                                        bool allowToggle,
                                                        bool* outValue,
                                                        char* rawValue,
                                                        size_t rawValueLen) {
  if (topic == nullptr) {
    return OtxMqttBoolCommandResult::NoMatch;
  }
  char commandTopic[MQTT_TOPIC_LEN];
  otxMakeMqttTopic(baseTopic, suffix, commandTopic, sizeof(commandTopic));
  if (strcmp(topic, commandTopic) != 0) {
    return OtxMqttBoolCommandResult::NoMatch;
  }

  otxDecodeMqttPayloadText(payload, length, rawValue, rawValueLen);
  if (!otxParseBoolText(rawValue, currentValue, outValue, allowToggle)) {
    return OtxMqttBoolCommandResult::InvalidPayload;
  }
  return OtxMqttBoolCommandResult::Parsed;
}

#endif
