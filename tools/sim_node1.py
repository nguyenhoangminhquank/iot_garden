"""Giả lập Node 1 (node tưới) để thử RuleChain khi chưa có phần cứng.

Script kết nối CoreIOT bằng token của thiết bị Garden-Node1, gửi telemetry
giả (độ ẩm đất giảm dần) và phản hồi RPC setPump như node thật:
khi bơm chạy, độ ẩm đất tăng lên.

Cài đặt:  pip install paho-mqtt
Chạy:     python sim_node1.py --token <ACCESS_TOKEN_NODE1>
          python sim_node1.py --token <TOKEN> --host app.coreiot.io --period 5 --hot
"""
import argparse
import json
import random
import time
from pathlib import Path
import os

import paho.mqtt.client as mqtt

TOPIC_TEL = "v1/devices/me/telemetry"
TOPIC_RPC = "v1/devices/me/rpc/request/+"

def load_env(path: Path) -> None:
        if not path.exists():
            return
        for line in path.read_text(encoding='utf-8').splitlines():
              line=line.strip()
              if line and not line.startswith("#") and "=" in line:
                    key, val = line. split("=", 1)
                    os.environ.setdefault(key.strip(), val.strip())


class Node1Sim:
    def __init__(self, hot: bool):
        self.soil = 45.0          # % độ ẩm đất
        self.temp = 39.0 if hot else 30.0
        self.humi = 70.0
        self.light = 60
        self.pump_until = 0.0     # thời điểm bơm tự tắt

    @property
    def pump(self) -> int:
        return 1 if time.time() < self.pump_until else 0

    def step(self, period: float) -> dict:
        if self.pump:
            self.soil = min(95.0, self.soil + 3.0 * period / 5)   # đang tưới: đất ẩm lên
        else:
            self.soil = max(5.0, self.soil - 1.5 * period / 5)    # đất khô dần
        self.temp += random.uniform(-0.2, 0.2)
        self.humi += random.uniform(-0.5, 0.5)
        return {
            "temp": round(self.temp, 1),
            "humi": round(self.humi, 1),
            "soil": round(self.soil, 1),
            "light": self.light,
            "pump": self.pump,
        }


def make_client() -> mqtt.Client:
    # paho-mqtt 2.x cần chỉ rõ phiên bản callback API; 1.x thì không có tham số này
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="sim-node1")
    except AttributeError:
        return mqtt.Client(client_id="sim-node1")


def main() -> None:
    load_env(Path(__file__).resolve().parent.parent / ".env")
    ap = argparse.ArgumentParser()
    ap.add_argument("--token", default=os.getenv("NODE1_TOKEN"))
    ap.add_argument("--host", default=os.getenv("COREIOT_HOST", "app.coreiot.io"))
    ap.add_argument("--port", type=int, default=int(os.getenv("COREIOT_PORT", "1883")))
    ap.add_argument("--period", type=float, default=5.0, help="chu kỳ gửi, giây")
    ap.add_argument("--hot", action="store_true", help="nhiệt độ ~39 °C để thử alarm nhiệt độ cao")
    ap.add_argument("--dark", action="store_true", help="ánh sáng thấp (10%%)")
    args = ap.parse_args()

    node = Node1Sim(args.hot)
    cli = make_client()
    cli.username_pw_set(args.token)

    def on_connect(client, userdata, *rest):
        print("[MQTT] đã kết nối, subscribe RPC")
        client.subscribe(TOPIC_RPC)

    def on_message(client, userdata, msg):
        req_id = msg.topic.rsplit("/", 1)[-1]
        body = json.loads(msg.payload or b"{}")
        method, params = body.get("method"), body.get("params")
        print(f"[RPC] <- {method} {params}")
        if method == "setPump":
            if isinstance(params, dict):
                on, dur = bool(params.get("on", True)), int(params.get("dur", 20))
            else:
                on, dur = bool(params), 20
            node.pump_until = time.time() + dur if on else 0
            resp = {"pump": node.pump}
        elif method == "getPump":
            resp = node.pump == 1
        else:
            resp = {"error": "unknown method"}
        client.publish(f"v1/devices/me/rpc/response/{req_id}", json.dumps(resp))
        client.publish(TOPIC_TEL, json.dumps({"pump": node.pump}))

    cli.on_connect = on_connect
    cli.on_message = on_message
    cli.connect(args.host, args.port, keepalive=60)
    cli.loop_start()

    try:
        while True:
            data = node.step(args.period)
            cli.publish(TOPIC_TEL, json.dumps(data))
            print(f"[TEL] -> {data}")
            time.sleep(args.period)
    except KeyboardInterrupt:
        pass
    finally:
        cli.loop_stop()
        cli.disconnect()


if __name__ == "__main__":
    main()
