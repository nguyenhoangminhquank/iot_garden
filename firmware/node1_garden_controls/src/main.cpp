#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <Wire.h>
#include <DHT20.h>
#include "config.h"
#include "garden_packet.h"

#define FW_VERSION "1.0.0"

static uint8_t bridgeMac[6] = BRIDGE_MAC;
DHT20 dht(&Wire);

bool     pumpOn    = false;
uint32_t pumpUntil = 0;
uint16_t seq       = 0;
uint32_t lastSend  = 0;
uint32_t statSent = 0, statOk = 0, statFail = 0;   // thống kê cho phép đo mất gói

// Biến chia sẻ với callback (callback chạy trong task Wi-Fi, không phải loop)
volatile bool sendDone   = false;
volatile bool sendOk     = false;
volatile bool cmdPending = false;
GardenPacket  cmdPacket;

bool isBroadcast() {
  for (int i = 0; i < 6; i++) if (bridgeMac[i] != 0xFF) return false;
  return true;
}

// ---------------------------------------------------------------- Callback ESP-NOW
// Chữ ký hàm callback khác nhau giữa các phiên bản ESP-IDF, nên dùng #if để build được ở cả hai.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void onSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
#else
void onSent(const uint8_t *mac, esp_now_send_status_t status) {
#endif
  sendOk   = (status == ESP_NOW_SEND_SUCCESS);   // SUCCESS = board nhận đã xác nhận (ACK)
  sendDone = true;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  const uint8_t *mac = info->src_addr;
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (len != sizeof(GardenPacket)) return;                         // sai kích thước: bỏ
  if (!isBroadcast() && memcmp(mac, bridgeMac, 6) != 0) return;    // không phải từ Bridge: bỏ
  GardenPacket p;
  memcpy(&p, data, sizeof(p));
  if (p.type != PKT_COMMAND || p.node_id != NODE_ID) return;
  cmdPacket  = p;            // chỉ chép lại, xử lý ở loop() cho callback chạy thật ngắn
  cmdPending = true;
}

void setPump(bool on, uint16_t durSec) {
  if (on) {
    if (durSec == 0 || durSec > PUMP_MAX_SEC) durSec = PUMP_MAX_SEC;
    pumpUntil = millis() + (uint32_t)durSec * 1000UL;
  }
  pumpOn = on;
  digitalWrite(PIN_PUMP, on ? RELAY_ACTIVE : !RELAY_ACTIVE);
  Serial.printf("[PUMP] %s%s\n", on ? "BAT " : "TAT", on ? (String(durSec) + " s").c_str() : "");
}

uint16_t pumpRemainingSec() {
  if (!pumpOn) return 0;
  int32_t left = (int32_t)(pumpUntil - millis());
  return left > 0 ? (uint16_t)((left + 999) / 1000) : 0;
}

int readAnalogAvg(uint8_t pin) {
  long sum = 0;
  for (int i = 0; i < 10; i++) { sum += analogRead(pin); delay(2); }
  return sum / 10;
}

float readSoilPct() {
  int raw = readAnalogAvg(PIN_SOIL);
  float pct = (float)(SOIL_RAW_DRY - raw) * 100.0f / (float)(SOIL_RAW_DRY - SOIL_RAW_WET);
  return constrain(pct, 0.0f, 100.0f);
}

float readLightPct() {
  float pct = readAnalogAvg(PIN_LIGHT) * 100.0f / 4095.0f;
  return LIGHT_INVERT ? 100.0f - pct : pct;
}

// ---------------------------------------------------------------- Gửi có thử lại
bool sendPacket(const GardenPacket &p) {
  for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
    sendDone = false;
    if (esp_now_send(bridgeMac, (const uint8_t *)&p, sizeof(p)) == ESP_OK) {
      uint32_t t0 = millis();
      while (!sendDone && millis() - t0 < 100) delay(1);   // chờ callback tối đa 100 ms
      if (sendDone && sendOk) return true;
    }
    Serial.printf("[ESPNOW] seq=%u lan %d that bai, gui lai\n", p.seq, attempt + 1);
    delay(20);
  }
  return false;
}

void sendTelemetry() {
  GardenPacket p = {};
  p.type    = PKT_TELEMETRY;
  p.node_id = NODE_ID;
  p.seq     = ++seq;

  if (dht.read() == DHT20_OK) {
    p.temp = dht.getTemperature();
    p.humi = dht.getHumidity();
  } else {
    p.temp = NAN;
    p.humi = NAN;
  }
  p.soil  = readSoilPct();
  p.light = readLightPct();
  p.pump  = pumpOn ? 1 : 0;
  p.dur   = pumpRemainingSec();

  statSent++;
  bool ok = sendPacket(p);
  ok ? statOk++ : statFail++;
  Serial.printf("[TEL] seq=%u temp=%.1f humi=%.1f soil=%.0f light=%.0f pump=%u -> %s (thanh cong %lu/%lu)\n",
                p.seq, p.temp, p.humi, p.soil, p.light, p.pump, ok ? "OK" : "MAT GOI",
                (unsigned long)statOk, (unsigned long)statSent);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.printf("\n=== garden-node1 v%s ===\n", FW_VERSION);

  pinMode(PIN_PUMP, OUTPUT);
  setPump(false, 0); 
  
  Wire.begin(PIN_SDA, PIN_SCL);
  dht.begin();
  analogReadResolution(12);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  Serial.printf("[INFO] MAC Node 1 = %s, kenh ESP-NOW = %d\n", WiFi.macAddress().c_str(), ESPNOW_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] khoi tao that bai, khoi dong lai sau 3 s");
    delay(3000);
    ESP.restart();
  }
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, bridgeMac, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) Serial.println("[ESPNOW] them peer that bai");

  if (isBroadcast()) Serial.println("[ESPNOW] dang gui BROADCAST (chua dien MAC Bridge) - khong co ACK");
}

void loop() {
  if (cmdPending) {
    cmdPending = false;
    GardenPacket c = cmdPacket;
    Serial.printf("[CMD] <- pump=%u dur=%u s\n", c.pump, c.dur);
    setPump(c.pump != 0, c.dur);
    sendTelemetry();
    lastSend = millis();
  }

  if (pumpOn && (int32_t)(millis() - pumpUntil) >= 0) {
    setPump(false, 0);
    sendTelemetry();
    lastSend = millis();
  }

  if (millis() - lastSend >= SEND_INTERVAL_MS) {
    lastSend = millis();
    sendTelemetry();
  }

  delay(5);
}
