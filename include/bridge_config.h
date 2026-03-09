#pragma once

#ifndef BRIDGE_HOSTNAME
#define BRIDGE_HOSTNAME "rgbcube"
#endif

#ifndef BRIDGE_TCP_PORT
#define BRIDGE_TCP_PORT 7777
#endif

#ifndef BRIDGE_UART_BAUD
#define BRIDGE_UART_BAUD 115200
#endif

// Local secrets file (not committed):
// copy include/bridge_secrets.h.example -> include/bridge_secrets.h
#if __has_include("bridge_secrets.h")
#include "bridge_secrets.h"
#endif

#ifndef BRIDGE_WIFI_SSID
#define BRIDGE_WIFI_SSID "YOUR_WIFI_SSID"
#endif

#ifndef BRIDGE_WIFI_PASS
#define BRIDGE_WIFI_PASS "YOUR_WIFI_PASSWORD"
#endif

#ifndef BRIDGE_MQTT_ENABLED
#define BRIDGE_MQTT_ENABLED 0
#endif

#ifndef BRIDGE_MQTT_HOST
#define BRIDGE_MQTT_HOST ""
#endif

#ifndef BRIDGE_MQTT_PORT
#define BRIDGE_MQTT_PORT 1883
#endif

#ifndef BRIDGE_MQTT_USER
#define BRIDGE_MQTT_USER ""
#endif

#ifndef BRIDGE_MQTT_PASS
#define BRIDGE_MQTT_PASS ""
#endif

#ifndef BRIDGE_MQTT_PREFIX
#define BRIDGE_MQTT_PREFIX "rgbcube"
#endif

#ifndef BRIDGE_MQTT_CLIENT_ID
#define BRIDGE_MQTT_CLIENT_ID ""
#endif

#ifndef BRIDGE_OTA_PORT
#define BRIDGE_OTA_PORT 8266
#endif

#ifndef BRIDGE_OTA_PASSWORD
#define BRIDGE_OTA_PASSWORD ""
#endif

#ifndef BRIDGE_ESPHOME_MODE
#define BRIDGE_ESPHOME_MODE 0
#endif

#ifndef BRIDGE_ESPHOME_NODE
#define BRIDGE_ESPHOME_NODE BRIDGE_HOSTNAME
#endif

#ifndef BRIDGE_FIRMWARE_VERSION
#define BRIDGE_FIRMWARE_VERSION "2026.02.1"
#endif

#ifndef BRIDGE_BOARD
#define BRIDGE_BOARD "d1_mini_pro"
#endif

#ifndef BRIDGE_PROJECT_NAME
#define BRIDGE_PROJECT_NAME "tnkrtrn.RGBCubeEspBridge"
#endif

#ifndef BRIDGE_PROJECT_VERSION
#define BRIDGE_PROJECT_VERSION "0.1"
#endif

#ifndef BRIDGE_MODEL
#define BRIDGE_MODEL "ESP UART Bridge"
#endif

#ifndef BRIDGE_MANUFACTURER
#define BRIDGE_MANUFACTURER "RGBCube"
#endif
