#ifndef OPENTAPTOX_CONFIG_H
#define OPENTAPTOX_CONFIG_H

#include <Arduino.h>

#include "secrets.h"

// ------------------------------
// WiFi / OTA
// ------------------------------
static const char* TIGO_PROJECT_TITLE = "openTAPtoX";
// IMPORTANT for future AI/codegen edits:
// Increment TIGO_FIRMWARE_VERSION whenever you change firmware behavior for a new flashed build/release.
static const char* TIGO_FIRMWARE_VERSION = "2026.08.04.29";
#if CONFIG_IDF_TARGET_ESP32C3
static const char* TIGO_HOSTNAME = "opentaptox-esp32c3";
#else
static const char* TIGO_HOSTNAME = "opentaptox-esp32c6";
#endif
static const char* TIGO_AP_SSID = "openTAPtoX-setup";
static const char* TIGO_AP_PASSWORD = "opentaptox";   // fallback AP default password
static const bool TIGO_ENABLE_OTA = true;
static const bool TIGO_ENABLE_MDNS = true;
static const uint32_t TIGO_SERIAL_WIFI_STATUS_EVERY_MS = 5000UL;
static const uint32_t TIGO_SERIAL_POWER_STATUS_EVERY_MS = 10000UL;
static const uint32_t TIGO_SERIAL_RX_POLL_STATUS_EVERY_MS = 10000UL;

// ------------------------------
// MQTT
// ------------------------------
#if CONFIG_IDF_TARGET_ESP32C3
static const char* TIGO_MQTT_BASE_TOPIC = "openTAPtoX/esp32c3";
#else
static const char* TIGO_MQTT_BASE_TOPIC = "openTAPtoX/esp32c6";
#endif
static const uint32_t TIGO_MQTT_RECONNECT_EVERY_MS = 5000UL;
static const uint32_t TIGO_MQTT_HEARTBEAT_EVERY_MS = 1000UL;
static const uint32_t TIGO_MQTT_STATUS_EVERY_MS = 10000UL;
static const uint32_t TIGO_MQTT_TELEMETRY_EVERY_MS = 5000UL;
static const uint32_t TIGO_MQTT_DISCOVERY_STEP_EVERY_MS = 250UL;
static const uint16_t TIGO_MQTT_SOCKET_TIMEOUT_S = 1;
static const bool TIGO_MQTT_RETAIN_STATUS = true;
static const bool TIGO_MQTT_RETAIN_TELEMETRY = false;
static const uint16_t TIGO_MQTT_PACKET_SIZE = 4096;
// Raw RS485 frame publishing is high-volume debug traffic. Keep it off by
// default so MQTT cannot stall the web server or OTA path from the frame parser.
static const bool TIGO_MQTT_ENABLE_RAW_FRAME_TOPIC = true;

// ------------------------------
// Home Assistant discovery and cleanup of former Python-publisher artifacts
// ------------------------------
static const bool TIGO_MQTT_ENABLE_LEGACY_STATE_TOPICS = false;
static const char* TIGO_MQTT_LEGACY_BASE_TOPIC = "openTAPtoX/live";
static const bool TIGO_MQTT_ENABLE_LEGACY_DISCOVERY = false;
static const char* TIGO_MQTT_LEGACY_DISCOVERY_PREFIX = "homeassistant";
static const bool TIGO_MQTT_RETAIN_LEGACY_STATE = true;
static const bool TIGO_MQTT_CLEAN_INVALID_NODE_STATUS_TOPICS = true;
static const uint16_t TIGO_MQTT_INVALID_NODE_STATUS_BASE = 32768;
static const uint16_t TIGO_MQTT_INVALID_NODE_STATUS_COUNT = 64;

// ------------------------------
// RS485 hardware
// ------------------------------
// Use a native UART for RS485. Keep ESP32-C3 RS485 away from its native
// USB pins so USB logging and flashing remain available during bus tests.
static const uint32_t TIGO_USB_BAUD = 115200UL;
static const bool TIGO_USB_ENABLE_FRAME_LOG = false;
static const int TIGO_RS485_UART_PORT = 1;
#if CONFIG_IDF_TARGET_ESP32C3
static const int TIGO_RS485_RX_PIN = 6;
static const int TIGO_RS485_TX_PIN = 5;
#else
static const int TIGO_RS485_RX_PIN = 20;
static const int TIGO_RS485_TX_PIN = 19;
#endif
static const int TIGO_RS485_DIR_PIN = -1;   // auto-direction transceiver
static const bool TIGO_RS485_DIR_ACTIVE_HIGH = true;
static const uint32_t TIGO_RS485_BAUD = 38400;
static const uint16_t TIGO_RS485_RX_BUFFER_BYTES = 1024;
static const uint32_t TIGO_RS485_POLL_TIMEOUT_MS = 50;
static const uint32_t TIGO_RS485_POLL_INTERVAL_MS = 250;
static const uint32_t TIGO_RS485_POLL_TIMEOUT_EVENT_EVERY_MS = 10000UL;
static const uint16_t TIGO_RS485_MAX_RX_BYTES_PER_LOOP = 256;
static const bool TIGO_RS485_ACTIVE_POLLING = true;
static const uint32_t TIGO_TAP_LINK_FRESH_MS = 3000UL;

// ------------------------------
// Web server resilience
// ------------------------------
static const uint32_t TIGO_WEB_RECOVER_AFTER_IDLE_MS = 45000UL;
static const uint32_t TIGO_WEB_RECOVER_EVERY_MS = 45000UL;

// ------------------------------
// TAP / controller defaults
// ------------------------------
static const float TIGO_IIN_SCALE = 0.0056f;
static const bool TIGO_ENABLE_ENUM_AT_BOOT = false;
static const uint16_t TIGO_ENUM_ID = 0x1235;
// Default RS485 controller-side identity used by the CCA-compatible flow. It is
// independent of the RF PAN and contains no TAP hardware identifier.
static const uint16_t TIGO_DESIRED_GATEWAY_ID = 0x1209;
static const uint16_t TIGO_GATEWAY_ID = 0x1209;
// Optional one-time bootstrap for headless lab commissioning. It is used only
// when the persisted optimizer map is empty, then saved like a web-configured
// entry. Leave empty in normal builds.
static const char* TIGO_BOOTSTRAP_OPTIMIZER_LONG_ADDR_HEX = "";
// Optional known-good RF identity for an isolated commissioning experiment.
// Both values must be supplied together and remain empty in normal builds.
static const char* TIGO_BOOTSTRAP_RADIO_DESCRIPTOR_HEX =
    "";
static const char* TIGO_BOOTSTRAP_RADIO_JOIN_SEED_HEX =
    "";
// Optional fallback only. The controller should learn and persist the TAP long
// address from enumeration/network-info responses; do not ship a captured TAP
// address here because that breaks first-start on other installations.
static const char* TIGO_TAP_LONG_ADDR_HEX = "";
// Old successful CCA captures start polling at non-zero packet windows such
// as 0x6040/0xB44A/0x97FD, while dead captures stay at 0x0000.
static const uint16_t TIGO_CCA_POLL_SEED_FALLBACK = 0x6040;
static const uint16_t TIGO_INITIAL_PACKET_NUMBER = 0x0000;
// A valid 0x0149 response is the authority for the receive cursor. Resetting
// it on every ESP reboot needlessly replays an already-live TAP queue.
static const bool TIGO_RESET_PACKET_COUNTER_AT_BOOT = false;
static const bool TIGO_READ_ONLY_WARM_ATTACH = true;
static const uint32_t TIGO_WARM_ATTACH_PASSIVE_LISTEN_MS = 3000UL;
// A valid ID/version/table attach followed by this many unanswered 0x0148
// requests is a transport fault, not permission for general TAP recovery.
static const uint16_t TIGO_CURSOR_STALL_RECOVERY_TIMEOUTS = 20;
static const uint32_t TIGO_CURSOR_CHECKPOINT_EVERY_MS = 300000UL;
static const uint32_t TIGO_BOOT_JOURNAL_POWER_CHECKPOINT_S = 300UL;
static const uint32_t TIGO_NETWORK_STATUS_STATE_FRESH_MS = 120000UL;
static const float TIGO_ELECTRICAL_RELEASE_MIN_VOUT_V = 5.0f;
static const uint32_t TIGO_ELECTRICAL_RELEASE_STABLE_MS = 30000UL;
static const uint32_t TIGO_ENUM_START_BURST_INTERVAL_MS = 760UL;
static const bool TIGO_CCA_COMPAT_BOOT_SEQUENCE = true;
// A controller-only reboot must not reset a TAP that is already operating.
// Probe the configured operational address first and run the destructive
// enumeration/address dance only when that warm attach fails.
static const bool TIGO_CCA_PREFER_WARM_ATTACH = true;
// Factory CCA captures persist the operational address before temporarily
// moving the TAP to 0x120A and applying the transaction through 0x0010.
// No captured firmware has yet proven that the late 0x120A transaction is
// required. Keep it available to explicit replay experiments, not normal boot.
static const bool TIGO_CCA_ENABLE_LATE_ADDRESS_DANCE = false;
static const uint8_t TIGO_CCA_BOOT_MAX_RETRIES_PER_STEP = 5;
static const uint32_t TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS = 2500UL;
// 0x0146/0002 is observed on unroutable node commands. Do not treat it as a
// successful RF acknowledgement; a matching 0x0B10 response is required.
static const bool TIGO_ACCEPT_SHORT_NODE_TEXT_ACK = false;
static const bool TIGO_PV_COMMAND_TX_DIAG_LOG = false;
static const uint32_t TIGO_CCA_ADDRESS_DANCE_GAP_MS = 5000UL;
// Observed CCA flows begin node-targeted wake commands shortly after setup;
// long waits can miss the optimizer wake window.
static const uint32_t TIGO_CCA_FIRST_NODE_WAKE_DELAY_MS = 3000UL;
static const bool TIGO_CCA_NODE_WAKE_SEQUENCE = true;
static const bool TIGO_CCA_LEARN_BEFORE_NODE_WAKE = true;
static const bool TIGO_ENABLE_NODE_SEED = true;
static const uint16_t TIGO_NODE_SEED_CHUNK_SIZE = 10;
static const uint16_t TIGO_NODE_ID_BASE = 2;
// CCA commissioning writes 0x8002-style IDs. Bit 15 means that the optimizer
// is configured but has not yet authenticated/joined the RF network.
static const uint8_t TIGO_NODE_SEED_NODE_ID_HIGH = 0x80;
static const bool TIGO_VERIFY_NODE_TABLE_AFTER_SEED = true;
static const bool TIGO_WAKE_AFTER_NODE_SEED = true;
static const bool TIGO_WAIT_FOR_LEARN_COUNTDOWN_AFTER_SEED = true;
// Preserve the TAP's live RF route after a topology join. Rewriting the entry
// immediately can discard transient downlink state before node setup finishes.
static const bool TIGO_PROMOTE_RF_CONFIRMED_NODES = false;
static const bool TIGO_SEND_2D_DURING_STARTUP = true;
static const bool TIGO_SEND_41_DURING_STARTUP = true;
static const bool TIGO_CCA_JOIN_BURST_DURING_WAKE = false;
static const uint16_t TIGO_CCA_JOIN_BURST_REPEATS = 3;
static const bool TIGO_SEND_PER_NODE_WAKE = true;
static const size_t TIGO_MAX_PV_COMMAND_PAYLOAD = 160;
static const uint16_t TIGO_CCA_NODE_WAKE_MAX_NODES = 16;
static const uint32_t TIGO_CCA_NODE_WAKE_SETUP_GAP_MS = 450UL;
static const uint32_t TIGO_CCA_NODE_WAKE_STEP_GAP_MS = 1900UL;
static const uint32_t TIGO_CCA_AFTER_FIRST_WAKE_SETTLE_MS = 30000UL;
static const uint32_t TIGO_CCA_NODE_WAKE_RETRY_EVERY_MS = 600000UL;
static const uint32_t TIGO_CCA_LEARN_TIMEOUT_MS = 360000UL;
static const uint32_t TIGO_CCA_LEARN_COUNTDOWN_TIMEOUT_MS = 960000UL;
static const uint32_t TIGO_CCA_LEARN_NETWORK_STATUS_EVERY_MS = 20000UL;
static const uint32_t TIGO_CCA_LEARN_JOIN_SEED_EVERY_MS = 60000UL;
static const uint16_t TIGO_CCA_NODE_WAKE_ROUNDS = 4;
static const uint16_t TIGO_CCA_POST_CONFIG_WAKE_ROUNDS = 1;
static const bool TIGO_CCA_NODE_WAKE_EXTENDED_COMMANDS = true;
static const bool TIGO_CCA_NODE_WAKE_PV_CONFIG_FALLBACK = true;
// Protocol-derived reporting defaults.
static const uint16_t TIGO_CCA_NODE_WAKE_PV_PERIOD_SLOTS = 0x0190;
static const uint16_t TIGO_CCA_NODE_WAKE_PV_PHASE_STEP_SLOTS = 0x0028;

// ------------------------------
// Periodic maintenance traffic
// ------------------------------
static const uint32_t TIGO_PING_EVERY_MS = 0UL;
static const uint32_t TIGO_NODE_TABLE_EVERY_MS = 0UL;
static const uint32_t TIGO_NETWORK_STATUS_EVERY_MS = 0UL;
static const bool TIGO_REQUEST_VERSION_AT_BOOT = false;
static const bool TIGO_REQUEST_RADIO_CONFIG_AT_BOOT = false;
static const bool TIGO_REQUEST_NETWORK_STATUS_AT_BOOT = false;
static const bool TIGO_REQUEST_NODE_TABLE_AT_BOOT = false;

// ------------------------------
// Preferences / NVS persistence
// ------------------------------
static const char* TIGO_PREFS_NAMESPACE = "opentaptox";
static const char* TIGO_PREFS_STATE_KEY = "state";
static const char* TIGO_PREFS_MQTT_KEY = "mqtt";
static const char* TIGO_PREFS_POLLING_KEY = "poll";
static const char* TIGO_PREFS_MQTT_MIGRATION_KEY = "mqttmig";
static const char* TIGO_PREFS_RADIO_IDENTITY_KEY = "rfid";
static const char* TIGO_PREFS_BOOT_JOURNAL_KEY = "bootjrnl";
static const char* TIGO_PREFS_BOOT_OPTIONS_KEY = "bootopts";
static const uint16_t TIGO_STATE_VERSION = 3;
static const uint16_t TIGO_MQTT_SETTINGS_VERSION = 1;
static const uint16_t TIGO_POLLING_SETTINGS_VERSION = 2;
static const uint16_t TIGO_RADIO_IDENTITY_VERSION = 1;
static const uint16_t TIGO_BOOT_JOURNAL_VERSION = 1;
static const uint16_t TIGO_BOOT_OPTIONS_VERSION = 1;

// ------------------------------
// Freshness / aggregate power handling
// Live sum = only fresh samples
// Held sum = fresh samples + decayed stale samples until hold timeout
// ------------------------------
static const uint32_t TIGO_SAMPLE_FRESH_MS = 3500UL;
static const uint32_t TIGO_SAMPLE_HOLD_MS = 60000UL;

// ------------------------------
// Optimizer / mapping capacity
// ESP32-C6 has more headroom; keep default sizes practical for larger arrays.
// ------------------------------
static const uint16_t TIGO_MAX_OPTIMIZERS = 64;
static const uint16_t TIGO_PANEL_FIELD_COUNT = 16;

// ------------------------------------------------------------
// Limits / storage
// ------------------------------------------------------------
static const uint32_t TIGO_STATE_MAGIC = 0x5449474FUL; // 'TIGO'
static const size_t MAX_FRAME_BODY = 512;
static const size_t MAX_FRAME_PAYLOAD = 506;
static const size_t MAX_NODE_MAP = TIGO_MAX_OPTIMIZERS;
static const size_t MAX_POWER_SLOTS = TIGO_MAX_OPTIMIZERS;
static const size_t MAX_EVENTS = 32;
static const size_t EVENT_TEXT_LEN = 128;
static const size_t MQTT_TOPIC_LEN = 128;

struct LegacyDiscoveryField {
  const char* jsonField;
  const char* key;
  const char* unit;
  const char* deviceClass;
};

static const LegacyDiscoveryField TIGO_LEGACY_DISCOVERY_FIELDS[] = {
  {"power", "power", "W", "power"},
  {"vin_v", "vin", "V", "voltage"},
  {"vout_v", "vout", "V", "voltage"},
  {"iin_a", "iin", "A", "current"},
  {"temp_c", "temp", "\xC2\xB0""C", "temperature"},
  {"rssi", "rssi", "dBm", "signal_strength"},
  {"duty_pct", "duty", "%", nullptr},
};

#endif
