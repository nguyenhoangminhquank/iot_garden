"""PC Gateway (T08) – cầu nối giữa Bridge (USB Serial, JSON mỗi dòng) và CoreIOT (MQTT) cho Node 1.

Chức năng:
  F03  Đọc telemetry Node 1 từ Bridge, gửi lên thiết bị gardenNode1 trên CoreIOT.
       Nhận RPC setPump / getPump từ CoreIOT, chuyển thành lệnh cmd xuống Bridge, trả kết quả.
  F04  Khi mất kết nối CoreIOT: tự tưới theo soil_min (đọc từ shared attribute), lưu telemetry vào
       hàng đợi và gửi bù kèm thời điểm gốc (ts) khi kết nối lại.
  +    Đếm gói mất theo seq, ghi log có timestamp vào logs/gateway.log (dùng cho Chương 4).

Chạy:
  python gateway/gateway.py --mock            (Bridge giả, không cần phần cứng)
  python gateway/gateway.py                   (Bridge thật, cổng lấy từ BRIDGE_PORT trong .env)
  python gateway/gateway.py --port COM5
"""
import argparse
import json
import logging
import os
import threading
import time
from collections import deque
from pathlib import Path

import paho.mqtt.client as mqtt

ROOT = Path(__file__).resolve().parent.parent

TOPIC_TEL = "v1/devices/me/telemetry"
TOPIC_ATTR = "v1/devices/me/attributes"
TOPIC_ATTR_REQ = "v1/devices/me/attributes/request/1"
TOPIC_ATTR_RESP = "v1/devices/me/attributes/response/+"
TOPIC_RPC = "v1/devices/me/rpc/request/+"

LOCAL_PUMP_SEC = 20        # mỗi lượt tưới cục bộ
LOCAL_COOLDOWN_SEC = 60    # nghỉ giữa hai lượt tưới cục bộ để đất kịp ngấm
ACK_TIMEOUT_SEC = 2.0      # chờ Bridge xác nhận lệnh

log = logging.getLogger("gateway")


# ---------------------------------------------------------------- tiện ích
def load_env(path: Path) -> None:
    """Đọc file .env (KEY=VALUE) vào biến môi trường, không cần thư viện ngoài."""
    if not path.exists():
        return
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            key, val = line.split("=", 1)
            os.environ.setdefault(key.strip(), val.strip())


def setup_logging() -> None:
    (ROOT / "logs").mkdir(exist_ok=True)
    fmt = logging.Formatter("%(asctime)s.%(msecs)03d %(levelname)s %(message)s", "%Y-%m-%d %H:%M:%S")
    for handler in (logging.StreamHandler(), logging.FileHandler(ROOT / "logs" / "gateway.log", encoding="utf-8")):
        handler.setFormatter(fmt)
        log.addHandler(handler)
    log.setLevel(logging.INFO)


def make_client(client_id: str) -> mqtt.Client:
    try:  # paho-mqtt 2.x
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
    except AttributeError:  # paho-mqtt 1.x
        return mqtt.Client(client_id=client_id)


class SerialBridge:
    """Bridge thật qua cổng COM. Cùng giao diện với MockBridge."""

    def __init__(self, port: str, baud: int = 115200):
        import serial  # pyserial
        self.ser = serial.Serial(port, baud, timeout=1)

    def readline(self, timeout: float = 1.0):
        raw = self.ser.readline()
        text = raw.decode("utf-8", errors="replace").strip()
        return text or None

    def write_line(self, line: str) -> None:
        self.ser.write((line + "\n").encode("utf-8"))

    def close(self) -> None:
        self.ser.close()


# ---------------------------------------------------------------- Gateway
class Gateway:
    def __init__(self, bridge, host: str, port: int, token: str, soil_min_default: float):
        self.bridge = bridge
        self.host, self.port = host, port
        self.write_lock = threading.Lock()

        self.connected = False
        self.buffer: deque = deque(maxlen=5000)        # telemetry chờ gửi bù khi mất kết nối
        self.soil_min = soil_min_default                 # ngưỡng cho luật tưới cục bộ
        self.last_values: dict = {}
        self.last_seq = None
        self.stat_received = 0
        self.stat_lost = 0
        self.next_local_pump = 0.0

        self.ack_event = threading.Event()
        self.last_ack: dict = {}

        self.client = make_client("pc-gateway-node1")
        self.client.username_pw_set(token)
        self.client.on_connect = self.on_connect
        self.client.on_disconnect = self.on_disconnect
        self.client.on_message = self.on_message
        self.client.reconnect_delay_set(min_delay=1, max_delay=30)

    # ------------------------------------------------------------ MQTT
    def on_connect(self, client, userdata, *rest):
        self.connected = True
        log.info("[MQTT] da ket noi %s:%s", self.host, self.port)
        client.subscribe(TOPIC_RPC)
        client.subscribe(TOPIC_ATTR)
        client.subscribe(TOPIC_ATTR_RESP)
        client.publish(TOPIC_ATTR, json.dumps({"connected_via": "pc-gateway", "gateway_fw": "1.0.0"}))
        client.publish(TOPIC_ATTR_REQ, json.dumps({"sharedKeys": "soil_min"}))
        self.flush_buffer()

    def on_disconnect(self, client, userdata, *rest):
        if self.connected:
            log.warning("[MQTT] mat ket noi CoreIOT -> chuyen sang che do cuc bo")
        self.connected = False

    def publish_tel(self, values: dict, ts_ms: int) -> None:
        payload = json.dumps({"ts": ts_ms, "values": values})
        if self.connected:
            info = self.client.publish(TOPIC_TEL, payload)
            if info.rc == mqtt.MQTT_ERR_SUCCESS:
                return
        self.buffer.append(payload)                     # chưa gửi được -> giữ lại để gửi bù

    def flush_buffer(self) -> None:
        if not self.buffer:
            return
        n = len(self.buffer)
        while self.buffer and self.connected:
            self.client.publish(TOPIC_TEL, self.buffer.popleft())
        log.info("[MQTT] da gui bu %d ban ghi telemetry", n - len(self.buffer))

    def on_message(self, client, userdata, msg):
        try:
            body = json.loads(msg.payload or b"{}")
        except json.JSONDecodeError:
            return
        if msg.topic.startswith("v1/devices/me/rpc/request/"):
            self.handle_rpc(msg.topic.rsplit("/", 1)[-1], body)
        elif msg.topic.startswith("v1/devices/me/attributes/response/"):
            self.apply_shared(body.get("shared", {}))
        elif msg.topic == TOPIC_ATTR:
            self.apply_shared(body)

    def apply_shared(self, attrs: dict) -> None:
        if isinstance(attrs.get("soil_min"), (int, float)):
            self.soil_min = float(attrs["soil_min"])
            log.info("[ATTR] soil_min = %.0f (dung cho luat tuoi cuc bo)", self.soil_min)

    def handle_rpc(self, req_id: str, body: dict) -> None:
        method, params = body.get("method"), body.get("params")
        log.info("[RPC] <- id=%s %s %s", req_id, method, params)
        if method == "setPump":
            if isinstance(params, dict):
                on, dur = bool(params.get("on", params.get("value", True))), int(params.get("dur", 20))
            else:
                on, dur = bool(params), 20
            ok = self.send_cmd(on, dur)
            resp = {"pump": int(on and ok) if on else 0, "ok": ok}
        elif method == "getPump":
            resp = bool(self.last_values.get("pump", 0))
        else:
            resp = {"error": "unknown method"}
        self.client.publish(f"v1/devices/me/rpc/response/{req_id}", json.dumps(resp))
        log.info("[RPC] -> id=%s %s", req_id, json.dumps(resp))

    # ------------------------------------------------------------ Bridge
    def send_cmd(self, on: bool, dur: int, wait_ack: bool = True) -> bool:
        """Gửi lệnh bơm xuống Bridge. wait_ack=False khi gọi từ vòng lặp chính
        (chính vòng lặp đó mới đọc được dòng ack, nên không được đứng chờ)."""
        self.ack_event.clear()
        line = json.dumps({"type": "cmd", "node": 1, "pump": int(on), "dur": dur})
        with self.write_lock:
            self.bridge.write_line(line)
        log.info("[BRIDGE] -> %s", line)
        if not wait_ack:
            return True
        if not self.ack_event.wait(ACK_TIMEOUT_SEC):
            log.warning("[BRIDGE] khong nhan duoc ack trong %.0f s", ACK_TIMEOUT_SEC)
            return False
        return bool(self.last_ack.get("ok"))

    def handle_bridge_line(self, line: str) -> None:
        if not line.startswith("{"):
            log.info("[BRIDGE-LOG] %s", line)            # dòng log của Bridge, không phải dữ liệu
            return
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            log.warning("[BRIDGE] dong JSON loi: %s", line)
            return
        kind = msg.get("type")
        if kind == "tel":
            self.process_tel(msg)
        elif kind == "ack":
            self.last_ack = msg
            self.ack_event.set()
            log.info("[BRIDGE] ack: %s", line)
        elif kind == "pong":
            log.info("[BRIDGE] pong: %s", line)

    def process_tel(self, msg: dict) -> None:
        seq = msg.get("seq")
        if self.last_seq is not None and isinstance(seq, int) and seq > self.last_seq + 1:
            self.stat_lost += seq - self.last_seq - 1         # seq bị hụt = gói mất trên ESP-NOW
        if isinstance(seq, int):
            self.last_seq = seq
        self.stat_received += 1

        values = {k: msg.get(k) for k in ("temp", "humi", "soil", "light", "pump", "rssi", "seq")}
        values["sensor_ok"] = msg.get("temp") is not None and msg.get("humi") is not None
        values = {k: v for k, v in values.items() if v is not None}   # bỏ trường null
        self.last_values = values
        self.publish_tel(values, int(time.time() * 1000))
        log.info("[TEL] %s %s", json.dumps(values), "" if self.connected else "(luu cho gui bu)")

        if not self.connected:
            self.local_rule(values)

    def local_rule(self, values: dict) -> None:
        """F04: mất Internet thì gateway tự tưới theo soil_min đã nhận gần nhất."""
        soil, pump = values.get("soil"), values.get("pump", 0)
        if soil is None or pump or time.time() < self.next_local_pump:
            return
        if soil < self.soil_min:
            log.warning("[LOCAL] mat ket noi CoreIOT, dat %.0f%% < %.0f%% -> tu tuoi %d s",
                        soil, self.soil_min, LOCAL_PUMP_SEC)
            self.send_cmd(True, LOCAL_PUMP_SEC, wait_ack=False)
            self.next_local_pump = time.time() + LOCAL_PUMP_SEC + LOCAL_COOLDOWN_SEC

    # ------------------------------------------------------------ vòng lặp chính
    def run(self) -> None:
        self.client.connect_async(self.host, self.port, keepalive=60)
        self.client.loop_start()
        with self.write_lock:
            self.bridge.write_line(json.dumps({"type": "ping"}))
        last_stat = time.time()
        try:
            while True:
                line = self.bridge.readline(timeout=1.0)
                if line:
                    self.handle_bridge_line(line)
                if time.time() - last_stat >= 60:
                    last_stat = time.time()
                    total = self.stat_received + self.stat_lost
                    loss = 100.0 * self.stat_lost / total if total else 0.0
                    log.info("[STAT] nhan %d goi, mat %d goi (%.1f%%), cho gui bu %d",
                             self.stat_received, self.stat_lost, loss, len(self.buffer))
        except KeyboardInterrupt:
            log.info("dung gateway")
        finally:
            self.client.loop_stop()
            self.client.disconnect()
            self.bridge.close()


def main() -> None:
    load_env(ROOT / ".env")
    ap = argparse.ArgumentParser(description="PC Gateway: Bridge (Serial) <-> CoreIOT (MQTT)")
    ap.add_argument("--mock", action="store_true", help="dung Bridge gia, khong can phan cung")
    ap.add_argument("--port", default=os.getenv("BRIDGE_PORT"), help="cong COM cua Bridge, vd COM5")
    ap.add_argument("--host", default=os.getenv("COREIOT_HOST", "app.coreiot.io"))
    ap.add_argument("--mqtt-port", type=int, default=int(os.getenv("COREIOT_PORT", "1883")))
    ap.add_argument("--token", default=os.getenv("NODE1_TOKEN"), help="access token cua gardenNode1")
    ap.add_argument("--soil-min", type=float, default=30.0, help="soil_min mac dinh truoc khi nhan tu CoreIOT")
    ap.add_argument("--period", type=float, default=10.0, help="(mock) chu ky gui telemetry, giay")
    ap.add_argument("--loss", type=float, default=0.0, help="(mock) ti le mat goi gia lap, 0..1")
    args = ap.parse_args()

    if not args.token:
        ap.error("thieu token: them --token hoac dien NODE1_TOKEN trong .env")
    setup_logging()

    if args.mock:
        from mock_bridge import MockBridge
        bridge = MockBridge(period=args.period, loss=args.loss)
        log.info("Dung Bridge GIA (mock)")
    else:
        if not args.port:
            ap.error("thieu cong COM: them --port hoac dien BRIDGE_PORT trong .env (hoac dung --mock)")
        bridge = SerialBridge(args.port)
        log.info("Mo cong %s", args.port)

    Gateway(bridge, args.host, args.mqtt_port, args.token, args.soil_min).run()


if __name__ == "__main__":
    main()