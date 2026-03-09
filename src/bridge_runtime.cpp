#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>
#include <stdlib.h>
#include <string.h>

#include "bridge_config.h"
#include "bridge_runtime.h"
#include "bridge_text_utils.h"

namespace {
using namespace bridge::text;

// Fixed by board wiring:
// STM PA09 (TX) -> ESP GPIO3 (RX)
// STM PA10 (RX) <- ESP GPIO2 (TX)
constexpr uint8_t STM_RX_PIN = 3;
constexpr uint8_t STM_TX_PIN = 2;
constexpr uint16_t CONTROL_LINE_MAX = 240;
constexpr uint32_t MQTT_RETRY_MS = 3000;
constexpr uint32_t BOOT_STM_CONFIG_WAIT_MS = 1800;

SoftwareSerial stmUart(STM_RX_PIN, STM_TX_PIN, false);
WiFiServer server(BRIDGE_TCP_PORT);
WiFiClient client;
WiFiClient mqttNet;
PubSubClient mqttClient(mqttNet);

struct BridgeRuntimeConfig {
  char wifiSsid[64];
  char wifiPass[96];
  bool mqttEnabled;
  char mqttHost[64];
  uint16_t mqttPort;
  char mqttUser[64];
  char mqttPass[96];
  char mqttPrefix[64];
  char mqttClientId[64];
  bool esphomeMode;
  char esphomeNode[64];
};

BridgeRuntimeConfig cfg;
char stmLineBuf[CONTROL_LINE_MAX + 1];
uint16_t stmLineLen = 0;
char usbLineBuf[CONTROL_LINE_MAX + 1];
uint16_t usbLineLen = 0;
char tcpLineBuf[CONTROL_LINE_MAX + 1];
uint16_t tcpLineLen = 0;
bool wifiReconfigurePending = false;
bool wifiSettingsDirty = false;
bool mqttReconfigurePending = false;
uint32_t lastMqttRetryMs = 0;
bool tcpToUartDebug = false;
uint32_t tcpToUartBytes = 0;
uint32_t tcpToUartDropped = 0;
uint32_t tcpToUartLines = 0;
uint32_t tcpToUartRb = 0;
uint32_t tcpToUartRk = 0;
uint32_t tcpToUartRf = 0;
uint32_t tcpToUartRx = 0;

char mqttAvailTopic[128];
char mqttCmdTopic[128];
char mqttStateTopic[128];
char mqttDisplayCmdTopic[128];
char mqttDisplayStateTopic[128];
char mqttModeCmdTopic[128];
char mqttModeStateTopic[128];
char mqttEsphomeDiscoverTopic[128];
char mqttHaRestartCfgTopic[192];
char mqttHaStmRestartCfgTopic[192];
char mqttHaStmDisplaySwitchCfgTopic[192];
char mqttHaStmModeSelectCfgTopic[192];
char mqttHaStateCfgTopic[192];
bool mqttDiscoveryEverPublished = false;
bool mqttDiscoveryLastOk = false;
bool mqttHaRestartLastOk = false;
bool mqttHaStmRestartLastOk = false;
bool mqttHaStmDisplaySwitchLastOk = false;
bool mqttHaStmModeSelectLastOk = false;
bool mqttHaStateLastOk = false;
uint32_t mqttDiscoveryLastMs = 0;
bool otaReady = false;
uint8_t otaLastProgress = 255;

void buildEsphomeNodeId(char *out, size_t outSize);

void ensureOta() {
  if (otaReady || WiFi.status() != WL_CONNECTED) {
    return;
  }

  ArduinoOTA.setHostname(BRIDGE_HOSTNAME);
#if BRIDGE_OTA_PORT > 0
  ArduinoOTA.setPort(BRIDGE_OTA_PORT);
#endif
  if (BRIDGE_OTA_PASSWORD[0] != '\0') {
    ArduinoOTA.setPassword(BRIDGE_OTA_PASSWORD);
  }

  ArduinoOTA.onStart([]() {
    if (client && client.connected()) {
      client.stop();
    }
    if (mqttClient.connected()) {
      mqttClient.publish(mqttStateTopic, "[bridge] ota start");
      mqttClient.disconnect();
    }
    Serial.println(F("[ota] start"));
  });

  ArduinoOTA.onEnd([]() {
    Serial.println(F("[ota] end"));
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (total == 0) {
      return;
    }
    uint8_t percent = (uint8_t)((progress * 100U) / total);
    if (percent == otaLastProgress) {
      return;
    }
    otaLastProgress = percent;
    if ((percent % 10U) == 0U || percent == 100U) {
      Serial.printf("[ota] progress %u%%\n", percent);
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[ota] error=%u\n", (unsigned int)error);
  });

  ArduinoOTA.begin();
  otaReady = true;
  Serial.print(F("[ota] ready port="));
  Serial.println(BRIDGE_OTA_PORT);
}

void logBridgeLine(const char *msg) {
  Serial.println(msg);
  if (client && client.connected()) {
    client.print(msg);
    client.print('\n');
  }
}

void resetTcpToUartStats() {
  tcpToUartBytes = 0;
  tcpToUartDropped = 0;
  tcpToUartLines = 0;
  tcpToUartRb = 0;
  tcpToUartRk = 0;
  tcpToUartRf = 0;
  tcpToUartRx = 0;
}

void printTcpToUartStats() {
  char msg[196];
  snprintf(msg, sizeof(msg),
           "[bridge] tcp->uart dbg=%s bytes=%lu dropped=%lu lines=%lu rb/rk/rf/rx=%lu/%lu/%lu/%lu",
           tcpToUartDebug ? "ON" : "OFF", (unsigned long)tcpToUartBytes,
           (unsigned long)tcpToUartDropped, (unsigned long)tcpToUartLines,
           (unsigned long)tcpToUartRb, (unsigned long)tcpToUartRk,
           (unsigned long)tcpToUartRf, (unsigned long)tcpToUartRx);
  logBridgeLine(msg);
}

void classifyTcpLine() {
  if (tcpLineLen == 0) {
    return;
  }
  tcpLineBuf[tcpLineLen] = '\0';
  char *line = trimInPlace(tcpLineBuf);
  if (line[0] == '\0') {
    return;
  }

  tcpToUartLines++;
  if (startsWithIgnoreCase(line, "rb")) {
    tcpToUartRb++;
  } else if (startsWithIgnoreCase(line, "rk")) {
    tcpToUartRk++;
  } else if (startsWithIgnoreCase(line, "rf")) {
    tcpToUartRf++;
  } else if (startsWithIgnoreCase(line, "rx")) {
    tcpToUartRx++;
  }

  if (tcpToUartDebug && ((tcpToUartLines % 64U) == 0U || startsWithIgnoreCase(line, "rf"))) {
    printTcpToUartStats();
  }
}

void initDefaults() {
  copyText(cfg.wifiSsid, sizeof(cfg.wifiSsid), BRIDGE_WIFI_SSID);
  copyText(cfg.wifiPass, sizeof(cfg.wifiPass), BRIDGE_WIFI_PASS);
  cfg.mqttEnabled = BRIDGE_MQTT_ENABLED != 0;
  copyText(cfg.mqttHost, sizeof(cfg.mqttHost), BRIDGE_MQTT_HOST);
  cfg.mqttPort = (uint16_t)BRIDGE_MQTT_PORT;
  copyText(cfg.mqttUser, sizeof(cfg.mqttUser), BRIDGE_MQTT_USER);
  copyText(cfg.mqttPass, sizeof(cfg.mqttPass), BRIDGE_MQTT_PASS);
  copyText(cfg.mqttPrefix, sizeof(cfg.mqttPrefix), BRIDGE_MQTT_PREFIX);
  copyText(cfg.mqttClientId, sizeof(cfg.mqttClientId), BRIDGE_MQTT_CLIENT_ID);
  cfg.esphomeMode = BRIDGE_ESPHOME_MODE != 0;
  copyText(cfg.esphomeNode, sizeof(cfg.esphomeNode), BRIDGE_ESPHOME_NODE);
  sanitizeTopicToken(cfg.mqttPrefix, true, "rgbcube");
  sanitizeTopicToken(cfg.esphomeNode, false, "rgbcube");
}

void rebuildMqttTopics() {
  sanitizeTopicToken(cfg.mqttPrefix, true, "rgbcube");
  sanitizeTopicToken(cfg.esphomeNode, false, "rgbcube");

  if (cfg.esphomeMode) {
    snprintf(mqttAvailTopic, sizeof(mqttAvailTopic), "esphome/%s/status", cfg.esphomeNode);
    snprintf(mqttCmdTopic, sizeof(mqttCmdTopic), "esphome/%s/command", cfg.esphomeNode);
    snprintf(mqttStateTopic, sizeof(mqttStateTopic), "esphome/%s/state", cfg.esphomeNode);
    snprintf(mqttDisplayCmdTopic, sizeof(mqttDisplayCmdTopic), "esphome/%s/stm_display/set", cfg.esphomeNode);
    snprintf(mqttDisplayStateTopic, sizeof(mqttDisplayStateTopic), "esphome/%s/stm_display/state", cfg.esphomeNode);
    snprintf(mqttModeCmdTopic, sizeof(mqttModeCmdTopic), "esphome/%s/stm_mode/set", cfg.esphomeNode);
    snprintf(mqttModeStateTopic, sizeof(mqttModeStateTopic), "esphome/%s/stm_mode/state", cfg.esphomeNode);
  } else {
    snprintf(mqttAvailTopic, sizeof(mqttAvailTopic), "%s/bridge/availability", cfg.mqttPrefix);
    snprintf(mqttCmdTopic, sizeof(mqttCmdTopic), "%s/bridge/command", cfg.mqttPrefix);
    snprintf(mqttStateTopic, sizeof(mqttStateTopic), "%s/bridge/state", cfg.mqttPrefix);
    snprintf(mqttDisplayCmdTopic, sizeof(mqttDisplayCmdTopic), "%s/bridge/stm_display/set", cfg.mqttPrefix);
    snprintf(mqttDisplayStateTopic, sizeof(mqttDisplayStateTopic), "%s/bridge/stm_display/state", cfg.mqttPrefix);
    snprintf(mqttModeCmdTopic, sizeof(mqttModeCmdTopic), "%s/bridge/stm_mode/set", cfg.mqttPrefix);
    snprintf(mqttModeStateTopic, sizeof(mqttModeStateTopic), "%s/bridge/stm_mode/state", cfg.mqttPrefix);
  }

  char esphomeNodeId[80];
  buildEsphomeNodeId(esphomeNodeId, sizeof(esphomeNodeId));
  snprintf(mqttEsphomeDiscoverTopic, sizeof(mqttEsphomeDiscoverTopic), "esphome/discover/%s", esphomeNodeId);
  snprintf(mqttHaRestartCfgTopic, sizeof(mqttHaRestartCfgTopic), "homeassistant/button/%s-restart/config",
           cfg.esphomeNode);
  snprintf(mqttHaStmRestartCfgTopic, sizeof(mqttHaStmRestartCfgTopic), "homeassistant/button/%s-stm_restart/config",
           cfg.esphomeNode);
  snprintf(mqttHaStmDisplaySwitchCfgTopic, sizeof(mqttHaStmDisplaySwitchCfgTopic),
           "homeassistant/switch/%s-stm_display/config", cfg.esphomeNode);
  snprintf(mqttHaStmModeSelectCfgTopic, sizeof(mqttHaStmModeSelectCfgTopic),
           "homeassistant/select/%s-stm_mode/config", cfg.esphomeNode);
  snprintf(mqttHaStateCfgTopic, sizeof(mqttHaStateCfgTopic), "homeassistant/sensor/%s-bridge_state/config",
           cfg.esphomeNode);
}

void mqttPublishState(const char *line) {
  if (!mqttClient.connected() || line == nullptr || line[0] == '\0') {
    return;
  }
  mqttClient.publish(mqttStateTopic, line);
}

void mqttPublishDisplayState(bool enabled) {
  if (!mqttClient.connected()) {
    return;
  }
  mqttClient.publish(mqttDisplayStateTopic, enabled ? "ON" : "OFF", true);
}

void mqttPublishModeState(uint8_t mode) {
  if (!mqttClient.connected()) {
    return;
  }
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", (unsigned)mode);
  mqttClient.publish(mqttModeStateTopic, buf, true);
}

void formatMacNoSeparator(char *out, size_t outSize) {
  if (outSize < 13) {
    if (outSize > 0) {
      out[0] = '\0';
    }
    return;
  }

  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(out, outSize, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void buildEsphomeNodeId(char *out, size_t outSize) {
  if (outSize == 0) {
    return;
  }
  char macNoSep[13];
  formatMacNoSeparator(macNoSep, sizeof(macNoSep));
  const char *tail = macNoSep;
  size_t macLen = strlen(macNoSep);
  if (macLen > 6) {
    tail = macNoSep + (macLen - 6);
  }
  snprintf(out, outSize, "%s-%s", cfg.esphomeNode, tail);
}

bool isMqttRestartCommand(const char *payload) {
  if (payload == nullptr || payload[0] == '\0') {
    return false;
  }
  if (equalsIgnoreCase(payload, "restart")) {
    return true;
  }
  if (strstr(payload, "\"cmd\"") != nullptr && strstr(payload, "restart") != nullptr) {
    return true;
  }
  return false;
}

void mqttPublishDiscovery() {
  if (!mqttClient.connected()) {
    return;
  }
  if (!cfg.esphomeMode) {
    return;
  }

  char macNoSep[13];
  formatMacNoSeparator(macNoSep, sizeof(macNoSep));
  char esphomeNodeId[80];
  buildEsphomeNodeId(esphomeNodeId, sizeof(esphomeNodeId));

  String ip = WiFi.localIP().toString();
  String discover = String("{\"ip\":\"") + ip + "\",\"name\":\"" + esphomeNodeId +
                    "\",\"friendly_name\":\"RGB Cube " + esphomeNodeId +
                    "\",\"version\":\"" BRIDGE_FIRMWARE_VERSION "\",\"mac\":\"" + macNoSep +
                    "\",\"platform\":\"ESP8266\",\"board\":\"" BRIDGE_BOARD
                    "\",\"network\":\"wifi\",\"project_name\":\"" BRIDGE_PROJECT_NAME
                    "\",\"project_version\":\"" BRIDGE_PROJECT_VERSION "\"}";
  bool okDiscover = mqttClient.publish(mqttEsphomeDiscoverTopic, discover.c_str(), true);

  String deviceBlock = String("\"device\":{\"ids\":[\"") + macNoSep + "\"],\"name\":\"" + cfg.esphomeNode +
                       "\",\"mdl\":\"" BRIDGE_MODEL "\",\"mf\":\"" BRIDGE_MANUFACTURER "\",\"cu\":\"http://" + ip +
                       "/\",\"sw\":\"" BRIDGE_FIRMWARE_VERSION "\"}";

  String restartPayload = String("{\"stat_t\":\"") + mqttAvailTopic + "\",\"avty_t\":\"" + mqttAvailTopic +
                          "\",\"dev_cla\":\"restart\",\"name\":\"SYS: Restart bridge\",\"uniq_id\":\"" + macNoSep +
                          "-restart\",\"pl_prs\":\"{\\\"cmd\\\":\\\"restart\\\"}\",\"pl_avail\":\"online\","
                          "\"pl_not_avail\":\"offline\",\"cmd_t\":\"" +
                          mqttCmdTopic + "\"," + deviceBlock + "}";
  bool okRestart = mqttClient.publish(mqttHaRestartCfgTopic, restartPayload.c_str(), true);

  String stmRestartPayload = String("{\"stat_t\":\"") + mqttAvailTopic + "\",\"avty_t\":\"" + mqttAvailTopic +
                             "\",\"dev_cla\":\"restart\",\"name\":\"SYS: Restart STM32\",\"uniq_id\":\"" + macNoSep +
                             "-stm-restart\",\"pl_prs\":\"rst\",\"pl_avail\":\"online\","
                             "\"pl_not_avail\":\"offline\",\"cmd_t\":\"" +
                             mqttCmdTopic + "\"," + deviceBlock + "}";
  bool okStmRestart = mqttClient.publish(mqttHaStmRestartCfgTopic, stmRestartPayload.c_str(), true);

  String stmDisplaySwitchPayload =
      String("{\"stat_t\":\"") + mqttDisplayStateTopic + "\",\"avty_t\":\"" + mqttAvailTopic +
      "\",\"name\":\"SYS: Cube Display\",\"uniq_id\":\"" + macNoSep +
      "-stm-display-switch\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"stat_on\":\"ON\",\"stat_off\":\"OFF\","
      "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"cmd_t\":\"" + mqttDisplayCmdTopic + "\"," +
      deviceBlock + "}";
  bool okStmDisplaySwitch =
      mqttClient.publish(mqttHaStmDisplaySwitchCfgTopic, stmDisplaySwitchPayload.c_str(), true);

  String stmModeSelectPayload =
      String("{\"name\":\"SYS: Cube Mode\",\"uniq_id\":\"") + macNoSep +
      "-stm-mode-select\",\"cmd_t\":\"" + mqttModeCmdTopic + "\",\"stat_t\":\"" + mqttModeStateTopic +
      "\",\"avty_t\":\"" + mqttAvailTopic +
      "\",\"options\":[\"0\",\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\",\"10\"],"
      "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"," +
      deviceBlock + "}";
  bool okStmModeSelect = mqttClient.publish(mqttHaStmModeSelectCfgTopic, stmModeSelectPayload.c_str(), true);

  String sensorPayload = String("{\"name\":\"Bridge State\",\"uniq_id\":\"") + macNoSep +
                         "-bridge-state\",\"stat_t\":\"" + mqttStateTopic + "\",\"avty_t\":\"" + mqttAvailTopic +
                         "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"icon\":\"mdi:console\","
                         "\"entity_category\":\"diagnostic\"," +
                         deviceBlock + "}";
  bool okState = mqttClient.publish(mqttHaStateCfgTopic, sensorPayload.c_str(), true);

  mqttDiscoveryEverPublished = true;
  mqttDiscoveryLastOk = okDiscover;
  mqttHaRestartLastOk = okRestart;
  mqttHaStmRestartLastOk = okStmRestart;
  mqttHaStmDisplaySwitchLastOk = okStmDisplaySwitch;
  mqttHaStmModeSelectLastOk = okStmModeSelect;
  mqttHaStateLastOk = okState;
  mqttDiscoveryLastMs = millis();

  Serial.print(F("[bridge] discovery publish esphome="));
  Serial.print(okDiscover ? F("OK") : F("FAIL"));
  Serial.print(F(" topic="));
  Serial.println(mqttEsphomeDiscoverTopic);
  Serial.print(F("[bridge] discovery publish ha restart/stm/display_switch/mode_select/state="));
  Serial.print(okRestart ? F("OK") : F("FAIL"));
  Serial.print('/');
  Serial.print(okStmRestart ? F("OK") : F("FAIL"));
  Serial.print('/');
  Serial.print(okStmDisplaySwitch ? F("OK") : F("FAIL"));
  Serial.print('/');
  Serial.print(okStmModeSelect ? F("OK") : F("FAIL"));
  Serial.print('/');
  Serial.println(okState ? F("OK") : F("FAIL"));
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.println(F("[bridge] connecting to Wi-Fi..."));
  WiFi.begin(cfg.wifiSsid, cfg.wifiPass);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 20000UL) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("[bridge] Wi-Fi connected, IP="));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("[bridge] Wi-Fi connect timeout"));
  }
}

void ensureClient() {
  if (client && client.connected()) {
    return;
  }

  WiFiClient incoming = server.accept();
  if (incoming) {
    if (client) {
      client.stop();
    }
    client = incoming;
    client.setNoDelay(true);
    Serial.print(F("[bridge] client connected: "));
    Serial.println(client.remoteIP());
  }
}

bool parseQuoted(const char *src, char *out, size_t outSize, const char *&next) {
  next = src;
  while (*next == ' ' || *next == '\t') {
    next++;
  }
  if (*next != '"') {
    return false;
  }
  next++;

  size_t n = 0;
  while (*next != '\0' && *next != '"') {
    if (*next == '\\' && next[1] != '\0') {
      next++;
    }
    if (n + 1 < outSize) {
      out[n++] = *next;
    }
    next++;
  }
  if (*next != '"') {
    return false;
  }
  out[n] = '\0';
  next++;
  return true;
}

bool parseKeyValue(const char *line, char *key, size_t keySize, char *value, size_t valueSize) {
  const char *eq = strchr(line, '=');
  if (eq == nullptr) {
    return false;
  }

  size_t kn = (size_t)(eq - line);
  if (kn == 0) {
    return false;
  }
  if (kn >= keySize) {
    kn = keySize - 1;
  }
  memcpy(key, line, kn);
  key[kn] = '\0';

  copyText(value, valueSize, eq + 1);
  char *k = trimInPlace(key);
  if (k != key) {
    memmove(key, k, strlen(k) + 1);
  }
  char *v = trimInPlace(value);
  if (v != value) {
    memmove(value, v, strlen(v) + 1);
  }
  return key[0] != '\0';
}

void applyKeyValue(const char *key, const char *value) {
  if (equalsIgnoreCase(key, "BRIDGE_WIFI_SSID")) {
    copyText(cfg.wifiSsid, sizeof(cfg.wifiSsid), value);
    wifiSettingsDirty = true;
    return;
  }
  if (equalsIgnoreCase(key, "BRIDGE_WIFI_PASS")) {
    copyText(cfg.wifiPass, sizeof(cfg.wifiPass), value);
    wifiSettingsDirty = true;
    return;
  }

  if (equalsIgnoreCase(key, "BRIDGE_MQTT_ENABLED")) {
    bool b = false;
    if (parseBoolLike(value, b)) {
      cfg.mqttEnabled = b;
      mqttReconfigurePending = true;
    }
    return;
  }
  if (equalsIgnoreCase(key, "BRIDGE_MQTT_HOST")) {
    copyText(cfg.mqttHost, sizeof(cfg.mqttHost), value);
    mqttReconfigurePending = true;
    return;
  }
  if (equalsIgnoreCase(key, "BRIDGE_MQTT_PORT")) {
    long p = strtol(value, nullptr, 10);
    if (p > 0 && p <= 65535) {
      cfg.mqttPort = (uint16_t)p;
      mqttReconfigurePending = true;
    }
    return;
  }
  if (equalsIgnoreCase(key, "BRIDGE_MQTT_USER")) {
    copyText(cfg.mqttUser, sizeof(cfg.mqttUser), value);
    mqttReconfigurePending = true;
    return;
  }
  if (equalsIgnoreCase(key, "BRIDGE_MQTT_PASS")) {
    copyText(cfg.mqttPass, sizeof(cfg.mqttPass), value);
    mqttReconfigurePending = true;
    return;
  }
  if (equalsIgnoreCase(key, "BRIDGE_MQTT_PREFIX")) {
    copyText(cfg.mqttPrefix, sizeof(cfg.mqttPrefix), value);
    sanitizeTopicToken(cfg.mqttPrefix, true, "rgbcube");
    mqttReconfigurePending = true;
    return;
  }
  if (equalsIgnoreCase(key, "BRIDGE_MQTT_CLIENT_ID")) {
    copyText(cfg.mqttClientId, sizeof(cfg.mqttClientId), value);
    mqttReconfigurePending = true;
    return;
  }

  if (equalsIgnoreCase(key, "BRIDGE_ESPHOME_MODE")) {
    bool b = false;
    if (parseBoolLike(value, b)) {
      cfg.esphomeMode = b;
      mqttReconfigurePending = true;
    }
    return;
  }
  if (equalsIgnoreCase(key, "BRIDGE_ESPHOME_NODE")) {
    copyText(cfg.esphomeNode, sizeof(cfg.esphomeNode), value);
    sanitizeTopicToken(cfg.esphomeNode, false, "rgbcube");
    mqttReconfigurePending = true;
    return;
  }
}

void printRuntimeStatus() {
  rebuildMqttTopics();
  Serial.print(F("[bridge] wifi ssid="));
  Serial.println(cfg.wifiSsid);
  Serial.print(F("[bridge] mqtt="));
  Serial.print(cfg.mqttEnabled ? F("ON ") : F("OFF "));
  Serial.print(cfg.mqttHost);
  Serial.print(':');
  Serial.println(cfg.mqttPort);
  Serial.print(F("[bridge] esphome="));
  Serial.print(cfg.esphomeMode ? F("ON node=") : F("OFF node="));
  Serial.println(cfg.esphomeNode);
  Serial.print(F("[bridge] mqtt cmd topic="));
  Serial.println(mqttCmdTopic);
  Serial.print(F("[bridge] mqtt state topic="));
  Serial.println(mqttStateTopic);
}

void printRuntimeDiag() {
  rebuildMqttTopics();
  Serial.println(F("[bridge] diag begin"));
  Serial.print(F("[bridge] wifi connected="));
  Serial.println(WiFi.status() == WL_CONNECTED ? F("YES") : F("NO"));
  Serial.print(F("[bridge] mqtt connected="));
  Serial.println(mqttClient.connected() ? F("YES") : F("NO"));
  Serial.print(F("[bridge] mqtt state rc="));
  Serial.println(mqttClient.state());
  Serial.print(F("[bridge] pending wifiReconfigure="));
  Serial.print(wifiReconfigurePending ? F("YES") : F("NO"));
  Serial.print(F(" wifiDirty="));
  Serial.print(wifiSettingsDirty ? F("YES") : F("NO"));
  Serial.print(F(" mqttReconfigure="));
  Serial.println(mqttReconfigurePending ? F("YES") : F("NO"));
  Serial.print(F("[bridge] topic avail="));
  Serial.println(mqttAvailTopic);
  Serial.print(F("[bridge] topic cmd="));
  Serial.println(mqttCmdTopic);
  Serial.print(F("[bridge] topic state="));
  Serial.println(mqttStateTopic);
  Serial.print(F("[bridge] topic display_cmd="));
  Serial.println(mqttDisplayCmdTopic);
  Serial.print(F("[bridge] topic display_state="));
  Serial.println(mqttDisplayStateTopic);
  Serial.print(F("[bridge] topic mode_cmd="));
  Serial.println(mqttModeCmdTopic);
  Serial.print(F("[bridge] topic mode_state="));
  Serial.println(mqttModeStateTopic);
  Serial.print(F("[bridge] topic esphome_discover="));
  Serial.println(mqttEsphomeDiscoverTopic);
  Serial.print(F("[bridge] topic ha_restart="));
  Serial.println(mqttHaRestartCfgTopic);
  Serial.print(F("[bridge] topic ha_stm_restart="));
  Serial.println(mqttHaStmRestartCfgTopic);
  Serial.print(F("[bridge] topic ha_stm_display_switch="));
  Serial.println(mqttHaStmDisplaySwitchCfgTopic);
  Serial.print(F("[bridge] topic ha_stm_mode_select="));
  Serial.println(mqttHaStmModeSelectCfgTopic);
  Serial.print(F("[bridge] topic ha_state="));
  Serial.println(mqttHaStateCfgTopic);
  Serial.print(F("[bridge] discovery ever="));
  Serial.print(mqttDiscoveryEverPublished ? F("YES") : F("NO"));
  Serial.print(F(" lastMs="));
  Serial.println((unsigned long)mqttDiscoveryLastMs);
  Serial.print(F("[bridge] discovery publish esphome="));
  Serial.print(mqttDiscoveryLastOk ? F("OK") : F("FAIL"));
  Serial.print(F(" ha restart/stm/display_switch/mode_select/state="));
  Serial.print(mqttHaRestartLastOk ? F("OK") : F("FAIL"));
  Serial.print('/');
  Serial.print(mqttHaStmRestartLastOk ? F("OK") : F("FAIL"));
  Serial.print('/');
  Serial.print(mqttHaStmDisplaySwitchLastOk ? F("OK") : F("FAIL"));
  Serial.print('/');
  Serial.print(mqttHaStmModeSelectLastOk ? F("OK") : F("FAIL"));
  Serial.print('/');
  Serial.println(mqttHaStateLastOk ? F("OK") : F("FAIL"));
  Serial.println(F("[bridge] diag end"));
}

bool processBridgeControlLine(char *line) {
  char *cmd = trimInPlace(line);
  if (cmd[0] == '\0') {
    return false;
  }
  Serial.print(F("[bridge] controlLine: "));
  Serial.println(cmd);

  if (startsWithIgnoreCase(cmd, "wifi set")) {
    const char *p = cmd + 8;
    char ssid[64];
    char pass[96];
    const char *after = nullptr;
    if (!parseQuoted(p, ssid, sizeof(ssid), after) || !parseQuoted(after, pass, sizeof(pass), after)) {
      logBridgeLine("[bridge] wifi set format: wifi set \"ssid\" \"pass\"");
      return true;
    }
    copyText(cfg.wifiSsid, sizeof(cfg.wifiSsid), ssid);
    copyText(cfg.wifiPass, sizeof(cfg.wifiPass), pass);
    wifiSettingsDirty = true;
    logBridgeLine("[bridge] wifi credentials updated");
    return true;
  }

  if (equalsIgnoreCase(cmd, "wifi apply")) {
    wifiReconfigurePending = true;
    wifiSettingsDirty = false;
    logBridgeLine("[bridge] wifi apply queued");
    return true;
  }

  if (startsWithIgnoreCase(cmd, "mqtt set ")) {
    char local[CONTROL_LINE_MAX + 1];
    copyText(local, sizeof(local), cmd + 9);
    char *tok = strtok(local, " ");
    while (tok != nullptr) {
      char key[64];
      char value[96];
      if (parseKeyValue(tok, key, sizeof(key), value, sizeof(value))) {
        if (equalsIgnoreCase(key, "host")) {
          copyText(cfg.mqttHost, sizeof(cfg.mqttHost), value);
        } else if (equalsIgnoreCase(key, "port")) {
          long p = strtol(value, nullptr, 10);
          if (p > 0 && p <= 65535) {
            cfg.mqttPort = (uint16_t)p;
          }
        } else if (equalsIgnoreCase(key, "user")) {
          copyText(cfg.mqttUser, sizeof(cfg.mqttUser), value);
        } else if (equalsIgnoreCase(key, "pass")) {
          copyText(cfg.mqttPass, sizeof(cfg.mqttPass), value);
        } else if (equalsIgnoreCase(key, "prefix")) {
          copyText(cfg.mqttPrefix, sizeof(cfg.mqttPrefix), value);
          sanitizeTopicToken(cfg.mqttPrefix, true, "rgbcube");
        } else if (equalsIgnoreCase(key, "client")) {
          copyText(cfg.mqttClientId, sizeof(cfg.mqttClientId), value);
        } else if (equalsIgnoreCase(key, "enabled")) {
          bool b = false;
          if (parseBoolLike(value, b)) {
            cfg.mqttEnabled = b;
          }
        }
      }
      tok = strtok(nullptr, " ");
    }
    mqttReconfigurePending = true;
    logBridgeLine("[bridge] mqtt settings updated");
    return true;
  }

  if (startsWithIgnoreCase(cmd, "esphome set ")) {
    char local[CONTROL_LINE_MAX + 1];
    copyText(local, sizeof(local), cmd + 12);
    char *tok = strtok(local, " ");
    while (tok != nullptr) {
      char key[64];
      char value[96];
      if (parseKeyValue(tok, key, sizeof(key), value, sizeof(value))) {
        if (equalsIgnoreCase(key, "mode") || equalsIgnoreCase(key, "enabled")) {
          bool b = false;
          if (parseBoolLike(value, b)) {
            cfg.esphomeMode = b;
          }
        } else if (equalsIgnoreCase(key, "node") || equalsIgnoreCase(key, "name")) {
          copyText(cfg.esphomeNode, sizeof(cfg.esphomeNode), value);
          sanitizeTopicToken(cfg.esphomeNode, false, "rgbcube");
        }
      }
      tok = strtok(nullptr, " ");
    }
    mqttReconfigurePending = true;
    logBridgeLine("[bridge] esphome settings updated");
    return true;
  }

  if (equalsIgnoreCase(cmd, "mqtt apply") || equalsIgnoreCase(cmd, "bridge apply")) {
    mqttReconfigurePending = true;
    logBridgeLine("[bridge] mqtt apply queued");
    return true;
  }

  if (equalsIgnoreCase(cmd, "bridge status")) {
    printRuntimeStatus();
    return true;
  }

  if (equalsIgnoreCase(cmd, "bridge tcpdbg")) {
    printTcpToUartStats();
    return true;
  }

  if (startsWithIgnoreCase(cmd, "bridge tcpdbg ")) {
    bool enabled = false;
    char *p = trimInPlace(cmd + 14);
    if (!parseBoolLike(p, enabled)) {
      logBridgeLine("[bridge] use: bridge tcpdbg [0|1]");
      return true;
    }
    tcpToUartDebug = enabled;
    resetTcpToUartStats();
    logBridgeLine(tcpToUartDebug ? "[bridge] tcp->uart debug ON" : "[bridge] tcp->uart debug OFF");
    return true;
  }

  if (equalsIgnoreCase(cmd, "bridge diag")) {
    printRuntimeDiag();
    return true;
  }

  char key[64];
  char value[128];
  if (parseKeyValue(cmd, key, sizeof(key), value, sizeof(value))) {
    if (startsWithIgnoreCase(key, "BRIDGE_")) {
      applyKeyValue(key, value);
      return true;
    }
  }

  return false;
}

void mqttSendUartCommand(const char *line) {
  if (line == nullptr || line[0] == '\0') {
    return;
  }

  if (cfg.esphomeMode) {
    if (equalsIgnoreCase(line, "ON")) {
      stmUart.print("m 1\n");
      return;
    }
    if (equalsIgnoreCase(line, "OFF")) {
      stmUart.print("m 2\n");
      return;
    }
  }

  stmUart.print(line);
  stmUart.print('\n');
}

bool mqttSendDisplaySwitchCommand(const char *payload) {
  bool enabled = false;
  if (parseBoolLike(payload, enabled)) {
    stmUart.print(enabled ? "dp 1\n" : "dp 0\n");
    return true;
  }
  if (equalsIgnoreCase(payload, "toggle") || equalsIgnoreCase(payload, "t")) {
    stmUart.print("dp t\n");
    return true;
  }
  return false;
}

bool mqttSendModeSelectCommand(const char *payload) {
  if (payload == nullptr || payload[0] == '\0') {
    return false;
  }
  char *endp = nullptr;
  long m = strtol(payload, &endp, 10);
  if (endp == payload || m < 0 || m > 10) {
    return false;
  }
  char cmd[16];
  snprintf(cmd, sizeof(cmd), "m %ld\n", m);
  stmUart.print(cmd);
  return true;
}

void mqttCallback(char *topic, uint8_t *payload, unsigned int length) {
  if (topic == nullptr || payload == nullptr || length == 0) {
    return;
  }
  bool isMainCmd = strcmp(topic, mqttCmdTopic) == 0;
  bool isDisplayCmd = strcmp(topic, mqttDisplayCmdTopic) == 0;
  bool isModeCmd = strcmp(topic, mqttModeCmdTopic) == 0;
  if (!isMainCmd && !isDisplayCmd && !isModeCmd) {
    return;
  }

  char line[CONTROL_LINE_MAX + 1];
  unsigned int n = (length > CONTROL_LINE_MAX) ? CONTROL_LINE_MAX : length;
  memcpy(line, payload, n);
  line[n] = '\0';
  char *trimmed = trimInPlace(line);
  if (trimmed[0] == '\0') {
    return;
  }

  // Log received command
  Serial.print(F("[mqtt] received command: "));
  Serial.println(trimmed);

  if (isMainCmd && isMqttRestartCommand(trimmed)) {
    Serial.println(F("[mqtt] action: ESP will restart"));
    mqttPublishState("[bridge] restart requested via mqtt");
    mqttClient.loop();
    delay(50);
    ESP.restart();
    return;
  }

  if (isDisplayCmd) {
    Serial.print(F("[mqtt] action: set STM display via switch cmd: "));
    Serial.println(trimmed);
    if (!mqttSendDisplaySwitchCommand(trimmed)) {
      Serial.println(F("[mqtt] ignored invalid display switch payload (use ON/OFF)"));
    }
    return;
  }

  if (isModeCmd) {
    Serial.print(F("[mqtt] action: set STM mode via select cmd: "));
    Serial.println(trimmed);
    if (!mqttSendModeSelectCommand(trimmed)) {
      Serial.println(F("[mqtt] ignored invalid mode payload (use 0..10)"));
    }
    return;
  }

  // Log UART command mapping/action
  Serial.print(F("[mqtt] action: send to STM UART: "));
  Serial.println(trimmed);
  mqttSendUartCommand(trimmed);
}

bool mqttConnect() {
  if (!cfg.mqttEnabled || cfg.mqttHost[0] == '\0') {
    return false;
  }

  mqttClient.setServer(cfg.mqttHost, cfg.mqttPort);
  mqttClient.setCallback(mqttCallback);

  char clientId[64];
  if (cfg.mqttClientId[0] != '\0') {
    copyText(clientId, sizeof(clientId), cfg.mqttClientId);
  } else {
    snprintf(clientId, sizeof(clientId), "rgbcube-%06x", (unsigned int)ESP.getChipId());
  }

  bool ok = false;
  if (cfg.mqttUser[0] != '\0') {
    ok = mqttClient.connect(clientId, cfg.mqttUser, cfg.mqttPass, mqttAvailTopic, 1, true, "offline");
  } else {
    ok = mqttClient.connect(clientId, mqttAvailTopic, 1, true, "offline");
  }

  if (!ok) {
    return false;
  }

  mqttClient.publish(mqttAvailTopic, "online", true);
  mqttClient.subscribe(mqttCmdTopic);
  mqttClient.subscribe(mqttDisplayCmdTopic);
  mqttClient.subscribe(mqttModeCmdTopic);
  mqttPublishDiscovery();
  mqttPublishState("[bridge] mqtt connected");
  // Force fresh display/mode state for HA entities.
  stmUart.print("dp\n");
  stmUart.print("p\n");
  return true;
}

void ensureMqtt() {
  if (!cfg.mqttEnabled || cfg.mqttHost[0] == '\0' || WiFi.status() != WL_CONNECTED) {
    if (mqttClient.connected()) {
      mqttClient.disconnect();
    }
    return;
  }

  if (mqttClient.connected()) {
    mqttClient.loop();
    return;
  }

  uint32_t now = millis();
  if ((uint32_t)(now - lastMqttRetryMs) < MQTT_RETRY_MS) {
    return;
  }
  lastMqttRetryMs = now;

  if (mqttConnect()) {
    Serial.print(F("[bridge] mqtt connected "));
    Serial.print(cfg.mqttHost);
    Serial.print(':');
    Serial.println(cfg.mqttPort);
  } else {
    Serial.print(F("[bridge] mqtt connect failed, rc="));
    Serial.println(mqttClient.state());
  }
}

void applyPendingConfig() {
  if (wifiReconfigurePending) {
    WiFi.disconnect();
    delay(10);
    ensureWiFi();
    wifiReconfigurePending = false;
    mqttReconfigurePending = true;
  }

  if (mqttReconfigurePending) {
    rebuildMqttTopics();
    if (mqttClient.connected()) {
      mqttClient.disconnect();
    }
    mqttReconfigurePending = false;
  }

  if (wifiSettingsDirty) {
    // Keep receiving the rest of a config burst; reconnect only on explicit "wifi apply".
    // This avoids dropping later control lines on SoftwareSerial.
  }
}

void pumpTcpToUart() {
  if (!(client && client.connected())) {
    return;
  }

  while (client.available() > 0) {
    uint8_t b = (uint8_t)client.read();
    tcpToUartBytes++;
    size_t wr = stmUart.write(b);
    if (wr != 1U) {
      tcpToUartDropped++;
    }

    if (b == '\r') {
      continue;
    }
    if (b == '\n') {
      classifyTcpLine();
      tcpLineLen = 0;
      continue;
    }
    if (tcpLineLen + 1 < sizeof(tcpLineBuf)) {
      tcpLineBuf[tcpLineLen++] = (char)b;
    } else {
      // Continue forwarding to UART even if this line overflows local debug parsing.
      tcpLineLen = 0;
    }
  }
}

void flushStmLineToOutputs() {
  if (stmLineLen == 0) {
    return;
  }

  stmLineBuf[stmLineLen] = '\0';
  char temp[CONTROL_LINE_MAX + 1];
  copyText(temp, sizeof(temp), stmLineBuf);
  bool consumed = processBridgeControlLine(temp);
  if (!consumed) {
    if (client && client.connected()) {
      client.print(stmLineBuf);
      client.print('\n');
    }
    const char *dp = strstr(stmLineBuf, "display=");
    if (dp != nullptr) {
      dp += 8;
      if (startsWithIgnoreCase(dp, "ON")) {
        mqttPublishDisplayState(true);
      } else if (startsWithIgnoreCase(dp, "OFF")) {
        mqttPublishDisplayState(false);
      }
    }
    const char *mp = strstr(stmLineBuf, "mode=");
    if (mp != nullptr) {
      mp += 5;
      long mode = strtol(mp, nullptr, 10);
      if (mode >= 0 && mode <= 10) {
        mqttPublishModeState((uint8_t)mode);
      }
    }
    mqttPublishState(stmLineBuf);
  }
  stmLineLen = 0;
}

void pumpUartToTcp() {
  while (stmUart.available() > 0) {
    char c = (char)stmUart.read();

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      flushStmLineToOutputs();
      continue;
    }

    if ((uint8_t)c < 32 && c != '\t') {
      if (stmLineLen > 0 && client && client.connected()) {
        client.write((const uint8_t *)stmLineBuf, stmLineLen);
      }
      stmLineLen = 0;
      if (client && client.connected()) {
        client.write((uint8_t)c);
      }
      continue;
    }

    if (stmLineLen + 1 < sizeof(stmLineBuf)) {
      stmLineBuf[stmLineLen++] = c;
    } else {
      if (client && client.connected()) {
        client.write((const uint8_t *)stmLineBuf, stmLineLen);
        client.write((uint8_t)c);
      }
      stmLineLen = 0;
    }
  }
}

void pumpUsbControl() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      usbLineBuf[usbLineLen] = '\0';
      if (usbLineLen > 0) {
        char temp[CONTROL_LINE_MAX + 1];
        copyText(temp, sizeof(temp), usbLineBuf);
        if (processBridgeControlLine(temp)) {
          // handled locally
        }
      }
      usbLineLen = 0;
      continue;
    }
    if (usbLineLen + 1 < sizeof(usbLineBuf)) {
      usbLineBuf[usbLineLen++] = c;
    }
  }
}

void requestBridgeSettingsFromStmOnBoot() {
  Serial.println(F("[bridge] requesting settings from STM..."));

  uint8_t requestsSent = 0;
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < BOOT_STM_CONFIG_WAIT_MS) {
    uint32_t elapsed = (uint32_t)(millis() - start);
    if (requestsSent == 0 || (requestsSent == 1 && elapsed >= 400) || (requestsSent == 2 && elapsed >= 1000)) {
      stmUart.print("sd bridge\n");
      requestsSent++;
    }
    pumpUartToTcp();
    delay(5);
  }

  Serial.println(F("[bridge] STM settings request window closed"));
}

} // namespace

void bridge::setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println();
  Serial.println(F("[bridge] boot"));

  initDefaults();
  rebuildMqttTopics();

  stmUart.begin(BRIDGE_UART_BAUD, SWSERIAL_8N1, STM_RX_PIN, STM_TX_PIN, false, 256);
  resetTcpToUartStats();
  tcpLineLen = 0;

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(BRIDGE_HOSTNAME);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  requestBridgeSettingsFromStmOnBoot();
  applyPendingConfig();
  rebuildMqttTopics();

  ensureWiFi();
  ensureOta();
  server.begin();
  server.setNoDelay(true);
  bool mqttBufOk = mqttClient.setBufferSize(2048);

  Serial.print(F("[bridge] hostname="));
  Serial.println(BRIDGE_HOSTNAME);
  Serial.print(F("[bridge] tcp port="));
  Serial.println(BRIDGE_TCP_PORT);
  Serial.print(F("[bridge] uart baud="));
  Serial.println(BRIDGE_UART_BAUD);
  Serial.print(F("[bridge] mqtt buffer="));
  Serial.println(mqttBufOk ? F("2048 OK") : F("setBufferSize FAIL"));
  printRuntimeStatus();
}

void bridge::loop() {
  pumpUsbControl();

  if (WiFi.status() != WL_CONNECTED) {
    ensureWiFi();
    delay(50);
    return;
  }

  ensureOta();
  ArduinoOTA.handle();
  applyPendingConfig();
  ensureClient();
  pumpTcpToUart();
  pumpUartToTcp();
  ensureMqtt();
}
