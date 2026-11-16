#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <ArduinoJson.h>
#include "config.h"
#include "garden_packet.h"

#define FW_VERSION "1.0.0"

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t nodeMac[6] = NODE1_MAC;
bool nodeKnown = false;

struct RxItem {
  uint8_t      mac[6];
  int8_t       rssi;
  GardenPacket pkt;
};
QueueHandle_t rxQueue;

volatile bool sendDone = false;
volatile bool sendOk   = false;

char   lineBuf[256];
size_t lineLen = 0;

bool isBroadcastMac(const uint8_t *m) {
  for (int i = 0; i < 6; i++) if (m[i] != 0xFF) return false;
  return true;
}

String macToStr(const uint8_t *m) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}

bool addPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  return esp_now_add_peer(&peer) == ESP_OK;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void onSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
#else
void onSent(const uint8_t *mac, esp_now_send_status_t status) {
#endif
  sendOk   = (status == ESP_NOW_SEND_SUCCESS);
  sendDone = true;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  const uint8_t *mac = info->src_addr;
  int8_t rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  int8_t rssi = 0;
#endif
  if (len != sizeof(GardenPacket)) return;
  RxItem it;
  memcpy(it.mac, mac, 6);
  it.rssi = rssi;
  memcpy(&it.pkt, data, sizeof(GardenPacket));
  if (it.pkt.type != PKT_TELEMETRY) return;
  xQueueSend(rxQueue, &it, 0);
}

void putNumberOrNull(JsonDocument &d, const char *key, float v) {
  if (isnan(v)) d[key] = nullptr;
  else          d[key] = round(v * 10.0) / 10.0;
}

void forwardTelemetry(const RxItem &it) {
  if (!nodeKnown) {
    memcpy(nodeMac, it.mac, 6);
    nodeKnown = addPeer(nodeMac);
    Serial.printf("[INFO] hoc duoc MAC Node %u = %s\n", it.pkt.node_id, macToStr(nodeMac).c_str());
  }

  JsonDocument d;
  d["type"] = "tel";
  d["node"] = it.pkt.node_id;
  d["seq"]  = it.pkt.seq;
  putNumberOrNull(d, "temp",  it.pkt.temp);
  putNumberOrNull(d, "humi",  it.pkt.humi);
  putNumberOrNull(d, "soil",  it.pkt.soil);
  putNumberOrNull(d, "light", it.pkt.light);
  d["pump"] = it.pkt.pump;
  d["dur"]  = it.pkt.dur;
  if (it.rssi != 0) d["rssi"] = it.rssi;
  serializeJson(d, Serial);
  Serial.println();
}

bool sendCommand(uint8_t node, bool pump, uint16_t dur) {
  static uint16_t cmdSeq = 0;
  GardenPacket p = {};
  p.type    = PKT_COMMAND;
  p.node_id = node;
  p.seq     = ++cmdSeq;
  p.pump    = pump ? 1 : 0;
  p.dur     = dur;

  const uint8_t *target = nodeKnown ? nodeMac : BROADCAST_MAC;
  bool ok = false;
  for (int attempt = 0; attempt <= CMD_MAX_RETRIES && !ok; attempt++) {
    sendDone = false;
    if (esp_now_send(target, (const uint8_t *)&p, sizeof(p)) == ESP_OK) {
      uint32_t t0 = millis();
      while (!sendDone && millis() - t0 < 100) delay(1);
      ok = sendDone && sendOk;
    }
    if (!ok) delay(20);
  }

  JsonDocument a;                                  // báo kết quả cho PC Gateway
  a["type"] = "ack";
  a["node"] = node;
  a["seq"]  = p.seq;
  a["ok"]   = ok;
  serializeJson(a, Serial);
  Serial.println();
  return ok;
}

void processLine(const char *line) {
  JsonDocument d;
  if (deserializeJson(d, line)) {
    Serial.printf("[ERR] khong doc duoc JSON: %s\n", line);
    return;
  }
  const char *type = d["type"] | "";

  if (strcmp(type, "cmd") == 0) {
    uint8_t  node = d["node"] | 1;
    bool     pump = (d["pump"] | 0) != 0;
    uint16_t dur  = d["dur"]  | 20;
    Serial.printf("[CMD] -> node %u pump=%d dur=%u\n", node, pump, dur);
    sendCommand(node, pump, dur);
  } else if (strcmp(type, "ping") == 0) {
    JsonDocument r;
    r["type"]       = "pong";
    r["fw"]         = FW_VERSION;
    r["mac"]        = WiFi.macAddress();
    r["node_known"] = nodeKnown;
    serializeJson(r, Serial);
    Serial.println();
  } else {
    Serial.printf("[ERR] lenh khong ho tro: %s\n", type);
  }
}

void readSerialLines() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        processLine(lineBuf);
        lineLen = 0;
      }
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.printf("[INFO] === garden-bridge v%s ===\n", FW_VERSION);

  rxQueue = xQueueCreate(16, sizeof(RxItem));

  WiFi.mode(WIFI_STA);             // bật radio, KHÔNG kết nối router
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  Serial.printf("[INFO] MAC Bridge = %s  (dien vao BRIDGE_MAC cua Node 1), kenh = %d\n",
                WiFi.macAddress().c_str(), ESPNOW_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERR] khoi tao ESP-NOW that bai, khoi dong lai sau 3 s");
    delay(3000);
    ESP.restart();
  }
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onRecv);

  addPeer(BROADCAST_MAC);                          // để gửi được khi chưa biết MAC Node 1
  if (!isBroadcastMac(nodeMac)) nodeKnown = addPeer(nodeMac);

  Serial.println("[INFO] san sang. Thu lenh: {\"type\":\"ping\"}");
}

void loop() {
  RxItem it;
  while (xQueueReceive(rxQueue, &it, 0) == pdTRUE) forwardTelemetry(it);
  readSerialLines();
  delay(2);
}

