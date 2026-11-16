import argparse
import json
import os
import random
import time
from pathlib import Path
import paho.mqtt.client as mqtt

TOPIC_TELE = "v1/devices/me/telemetry"
TOPIC_ATTR = "v1/devices/me/attributes"
TOPIC_ATTR_REQUEST = "v1/devices/me/attributes/request/1"
TOPIC_ATTR_RESP ="v1/devices/me/attributes/response/+"
TOPIC_RPC = "v1/devices/me/rpc/request/+"
FW_TITLE,FW_VERSION = "garden-node-sim-3", "1.0.0"

def load_env(path: Path) -> None:
        if not path.exists():
            return
        for line in path.read_text(encoding='utf-8').splitlines():
              line=line.strip()
              if line and not line.startswith("#") and "=" in line:
                    key, val = line. split("=", 1)
                    os.environ.setdefault(key.strip(), val.strip())
class Node3Sim:
    def __init__(self, hot: bool, dark: bool):
        self.temp = 37.0 if hot else 30.0
        self.humi = 65.0
        self.light = 10 if dark else 60
        self.fan = False
        self.lamp = False
        self.interval = 10
    
    def step(self) ->dict:
        self.temp+=-0.6 if self.fan else 0.3
        self.temp = min(40.0, max(26.0, self.temp + random.uniform(-0.1, 0.1)))
        self.humi = min(90.0, max(40.0, self.humi + random.uniform(-0.5, 0.5)))
        light = self.light + (10 if self.lamp else 0) + random.randint(-2, 2)
        return {
            "temp": round(self.temp, 1),
            "humi": round(self.humi, 1),
            "sensor_ok": True,
            "light": max(0, min(100, light)),
            "fan": self.fan,
            "lamp": self.lamp,
            "rssi": random.randint(-65, -50),
        }


def as_bool(params) -> bool:
    if isinstance(params, dict):
        params = params.get("value", False)
    return bool(params)


def make_client(client_id: str) -> mqtt.Client:
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
    except AttributeError:
        return mqtt.Client(client_id=client_id)

def main() -> None:
    load_env(Path(__file__).resolve().parent.parent / ".env")
    ap = argparse.ArgumentParser()
    ap.add_argument("--token", default=os.getenv("NODE3_TOKEN"))
    ap.add_argument("--host", default=os.getenv("COREIOT_HOST", "app.coreiot.io"))
    ap.add_argument("--port", type=int, default=int(os.getenv("COREIOT_PORT", "1883")))
    ap.add_argument("--hot", action="store_true", help="bắt đầu ở 37 °C")
    ap.add_argument("--dark", action="store_true", help="ánh sáng thấp (10%%)")
    args = ap.parse_args()
    if not args.token:
        ap.error("thiếu token: thêm --token hoặc điền NODE3_TOKEN trong .env")

    node = Node3Sim(args.hot, args.dark)
    cli = make_client("sim-node3")
    cli.username_pw_set(args.token)

    def publish(topic: str, data) -> None:
        payload = json.dumps(data)
        cli.publish(topic, payload)
        print(f"[MQTT] -> {topic} {payload}")

    def publish_state() -> None:
        publish(TOPIC_TELE, {"fan": node.fan, "lamp": node.lamp})

    def apply_shared(attrs: dict) -> None:
        if isinstance(attrs.get("interval"), (int, float)) and 2 <= attrs["interval"] <= 3600:
            node.interval = int(attrs["interval"])
            print(f"[ATTR] interval = {node.interval} s")

    def on_connect(client, userdata, *rest):
        print(f"[MQTT] đã kết nối {args.host}:{args.port}")
        client.subscribe(TOPIC_RPC)
        client.subscribe(TOPIC_ATTR)
        client.subscribe(TOPIC_ATTR_RESP)
        publish(TOPIC_ATTR, {"fw_title": FW_TITLE, "fw_version": FW_VERSION,
                             "ip": "192.168.1.99", "mac": "SIM:00:00:00:00:03"})
        client.publish(TOPIC_ATTR_REQUEST, json.dumps({"sharedKeys": "interval"}))
        publish_state()

    def on_message(client, userdata, msg):
        body = json.loads(msg.payload or b"{}")
        if msg.topic.startswith("v1/devices/me/rpc/request/"):
            req_id = msg.topic.rsplit("/", 1)[-1]
            method, params = body.get("method"), body.get("params")
            print(f"[RPC] <- id={req_id} method={method} params={params}")
            if method == "setFan":
                node.fan = as_bool(params); resp = node.fan; publish_state()
            elif method == "getFan":
                resp = node.fan
            elif method == "setLamp":
                node.lamp = as_bool(params); resp = node.lamp; publish_state()
            elif method == "getLamp":
                resp = node.lamp
            elif method == "getState":
                resp = {"fan": node.fan, "lamp": node.lamp, "fw_version": FW_VERSION}
            else:
                resp = {"error": "unknown method"}
            client.publish(f"v1/devices/me/rpc/response/{req_id}", json.dumps(resp))
            print(f"[RPC] -> id={req_id} {json.dumps(resp)}")
        elif msg.topic.startswith("v1/devices/me/attributes/response/"):
            apply_shared(body.get("shared", {}))
        elif msg.topic == TOPIC_ATTR:
            apply_shared(body)

    cli.on_connect = on_connect
    cli.on_message = on_message
    cli.connect(args.host, args.port, keepalive=60)
    cli.loop_start()

    try:
        while True:
            publish(TOPIC_TELE, node.step())
            time.sleep(node.interval)
    except KeyboardInterrupt:
        pass
    finally:
        cli.loop_stop()
        cli.disconnect()


if __name__ == "__main__":
    main()            


