#include <Arduino.h>
#include <Wifi.h>
#include <Wire.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT20.h>
#include <config.h>

#define FW_TITLE "graden-node3"
#define FW_VERSION "1.0.0"

static const char *TOPIC_TELE ="v1/devices/me/telemetry";
static const char *TOPIC_ATTR ="v1/devices/me/attributes";
static const char *TOPIC_ATTR_REQ ="v1/devices/me/attributes/request/1";
static const char *TOPIC_ATTR_RESP  = "v1/devices/me/attributes/response/+";
static const char *TOPIC_RPC_REQ    = "v1/devices/me/rpc/request/+";
static const char *PREFIX_RPC_REQ   = "v1/devices/me/rpc/request/";
static const char *PREFIX_ATTR_RESP = "v1/devices/me/attributes/response/";

WiFiClient net;
PubSubClient mqtt(net);
DHT20 dht(&Wire);

bool fanOn=false;
bool lampOn=false;
uint32_t intervalMs=1000;
uint32_t lastRetry=0;
uint32_t lastSend=0;

void applyRelay(uint8_t pin, bool on){
  digitalWrite(pin, on ? RELAY_ACTIVE : !RELAY_ACTIVE);
}

void publishJson(const char *topic, JsonDocument &doc){
  char buf[256];
  size_t n=serializeJson(doc,buf,sizeof(buf));
  mqtt.publish(topic, (const uint8_t *)buf,n,false);
  Serial.printf("[MQTT]->%s %s\n",topic,buf);
}

void publishState(){
  JsonDocument doc;
  doc["fan"]=fanOn;
  doc["lamp"]=lampOn;
  publishJson(TOPIC_TELE, doc);
}

void setFan(bool on){
  fanOn=on;
  applyRelay(PIN_FAN, on);
  Serial.printf("[RELAY]fan=%d\n",on);
}
void setLamp(bool on){
  lampOn=on;
  applyRelay(PIN_LAMP, on);
  Serial.printf("[RELAY]lamp=%d\n",on);
}

void sendTelemetry(){
  JsonDocument doc;
  int status=dht.read();
  if(status==DHT20_OK){
    doc["temp"]=roundf(dht.getTemperature() *10)/10.0;
    doc["humid"]=roundf(dht.getHumidity()*10)/10.0;
    doc["sensor_ok"]=true;
  }
  else{
    doc["sensor_ok"]=false;
    Serial.printf("DHT20] loi doc, status-%d\n", status);
  }
  int raw=analogRead(PIN_LIGHT);
  int pct=map(raw,0,4095,0,100);
  if(LIGHT_INVERT) pct=100-pct;
  doc["light"]=pct;

  doc["fan"]=fanOn;
  doc["lamp"]=lampOn;
  doc["rssi"]=WiFi.RSSI();
  publishJson(TOPIC_TELE,doc);
}

void applySharedAttributes(JsonVariantConst attrs){
  if (attrs["interval"].is<int>()){
    int s= attrs["interval"].as<int>();
    if (s>=2 && s<=3600){
      intervalMs=(uint32_t)s*1000;
      Serial.printf("[ATTR] interval = %d s\n",s);
    }
  }
}

bool paramAsBool(JsonVariantConst p){
  if(p.is<bool>()) return p.as<bool>();
  if(p["value"].is<bool>()) return p["value"].as<bool>();
  if(p.is<int>()) return p.as<int>()!=0;
  return false;
}

void handleRpc(const char *reqId, JsonDocument &req) {
  const char *method=req["method"]|"";
  JsonVariantConst params = req["params"];
  String respTopic = String("v1/devices/me/rpc/response/") + reqId;
  String resp;

  Serial.printf("[RPC] <- id=%s method=%s\n",reqId,method);

  if (strcmp(method, "setFan") == 0)  { setFan(paramAsBool(params));  resp=fanOn?"true":"false"; publishState(); }
  else if (strcmp(method, "getFan") == 0)  { resp=fanOn?"true":"false"; }
  else if (strcmp(method, "setLamp") == 0) { setLamp(paramAsBool(params)); resp=lampOn?"true":"false"; publishState(); }
  else if (strcmp(method, "getLamp") == 0) { resp=lampOn?"true":"false"; }
  else if (strcmp(method, "getState") == 0) {
    JsonDocument s;
    s["fan"]=fanOn; s["lamp"]=lampOn; s["fw_version"]=FW_VERSION;
    serializeJson(s, resp);
  } else {
    resp="{\"error\":\"unknown method\"}";
  }

  mqtt.publish(respTopic.c_str(),resp.c_str());
  Serial.printf("[RPC] -> %s %s\n",respTopic.c_str(),resp.c_str());
}

void onMessage(char *topic, byte *payload, unsigned int len) {
  JsonDocument doc;
  if (deserializeJson(doc,payload,len)) {
    Serial.printf("[MQTT] JSON loi tren topic %s\n",topic);
    return;
  }
  if (strncmp(topic,PREFIX_RPC_REQ,strlen(PREFIX_RPC_REQ)) == 0) {
    handleRpc(topic+strlen(PREFIX_RPC_REQ),doc);
  } else if (strncmp(topic, PREFIX_ATTR_RESP, strlen(PREFIX_ATTR_RESP)) == 0) {
    applySharedAttributes(doc["shared"]);
  } else if (strcmp(topic, TOPIC_ATTR) == 0) {
    applySharedAttributes(doc);
  }
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("[WIFI] ket noi %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("[WIFI] OK, IP=%s RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else
    Serial.println("[WIFI] that bai, se thu lai");
}

bool connectMqtt() {
  String clientId = "node3-" + WiFi.macAddress();
  Serial.printf("[MQTT] ket noi %s:%d ... ", COREIOT_HOST, COREIOT_PORT);
  if (!mqtt.connect(clientId.c_str(), DEVICE_TOKEN, nullptr)) {   // username = token
    Serial.printf("loi, state=%d\n", mqtt.state());
    return false;
  }
  Serial.println("OK");

  mqtt.subscribe(TOPIC_RPC_REQ);
  mqtt.subscribe(TOPIC_ATTR);
  mqtt.subscribe(TOPIC_ATTR_RESP);

  JsonDocument info;
  info["fw_title"]   = FW_TITLE;
  info["fw_version"] = FW_VERSION;
  info["ip"]         = WiFi.localIP().toString();
  info["mac"]        = WiFi.macAddress();
  publishJson(TOPIC_ATTR, info);

  mqtt.publish(TOPIC_ATTR_REQ, "{\"sharedKeys\":\"interval\"}");
  publishState();
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.printf("\n=== %s v%s ===\n", FW_TITLE, FW_VERSION);

  pinMode(PIN_FAN, OUTPUT);
  pinMode(PIN_LAMP, OUTPUT);
  setFan(false);
  setLamp(false);

  Wire.begin(PIN_SDA, PIN_SCL);
  dht.begin();
  analogReadResolution(12);

  mqtt.setServer(COREIOT_HOST, COREIOT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(1024);

  connectWifi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastRetry > 5000) { lastRetry = millis(); connectWifi(); }
    return;
  }
  if (!mqtt.connected()) {
    if (millis() - lastRetry > 5000) { lastRetry = millis(); connectMqtt(); }
    return;
  }
  mqtt.loop();

  if (millis() - lastSend >= intervalMs) {
    lastSend = millis();
    sendTelemetry();
  }
}