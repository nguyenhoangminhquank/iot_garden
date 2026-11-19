"""Bridge giả: đóng vai cả Node 1 lẫn Bridge, nói ĐÚNG giao thức Serial JSON của firmware node2_bridge.

Dùng để chạy PC Gateway khi chưa có phần cứng:  python gateway/gateway.py --mock
Giao diện giống SerialBridge (readline / write_line / close), nên gateway không cần biết đang nói với
board thật hay bản giả.
"""
import json
import queue
import random
import threading
import time


class MockBridge:
    def __init__(self, period: float = 10.0, loss: float = 0.0):
        self.period = period          # chu kỳ gửi telemetry (giây), giống SEND_INTERVAL_MS của Node 1
        self.loss = loss              # tỉ lệ giả lập mất gói ESP-NOW (0..1)
        self._out: "queue.Queue[str]" = queue.Queue()
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self.soil, self.temp, self.humi, self.light = 45.0, 30.0, 70.0, 60.0
        self.pump_until = 0.0
        self.seq = 0
        self.cmd_seq = 0
        self._out.put("[INFO] === mock-bridge (Node 1 + Bridge gia lap) ===")
        threading.Thread(target=self._run, daemon=True).start()

    # ------------------------------------------------------------ Node 1 giả
    @property
    def pump(self) -> int:
        return 1 if time.time() < self.pump_until else 0

    def _send_tel(self) -> None:
        with self._lock:
            self.seq += 1
            if random.random() < self.loss:      # gói này "mất" trên đường ESP-NOW
                return
            pump = self.pump
            left = max(0, int(self.pump_until - time.time() + 0.999)) if pump else 0
            line = {
                "type": "tel", "node": 1, "seq": self.seq,
                "temp": round(self.temp + random.uniform(-0.2, 0.2), 1),
                "humi": round(self.humi + random.uniform(-0.5, 0.5), 1),
                "soil": round(self.soil, 1),
                "light": round(self.light + random.uniform(-2, 2), 1),
                "pump": pump, "dur": left, "rssi": random.randint(-70, -45),
            }
        self._out.put(json.dumps(line))

    def _run(self) -> None:
        last, was_pumping = 0.0, False
        while not self._stop.is_set():
            # mô hình đất: đang bơm +0.6 %/s, không bơm -0.3 %/s, giới hạn 5..95
            self.soil = min(95.0, max(5.0, self.soil + (0.6 if self.pump else -0.3)))
            if was_pumping and not self.pump:     # bơm vừa tự tắt -> báo trạng thái ngay
                self._send_tel()
            was_pumping = bool(self.pump)
            if time.time() - last >= self.period:
                last = time.time()
                self._send_tel()
            time.sleep(1)

    # ------------------------------------------------------------ giao diện giống SerialBridge
    def readline(self, timeout: float = 1.0):
        try:
            return self._out.get(timeout=timeout)
        except queue.Empty:
            return None

    def write_line(self, line: str) -> None:
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            self._out.put(f"[ERR] khong doc duoc JSON: {line}")
            return
        if msg.get("type") == "cmd":
            on = bool(msg.get("pump", 0))
            dur = min(int(msg.get("dur", 20) or 20), 60)          # PUMP_MAX_SEC = 60
            self.pump_until = time.time() + dur if on else 0.0
            self.cmd_seq += 1
            self._out.put(json.dumps({"type": "ack", "node": msg.get("node", 1),
                                      "seq": self.cmd_seq, "ok": True}))
            self._send_tel()                                       # Node 1 báo trạng thái mới
        elif msg.get("type") == "ping":
            self._out.put(json.dumps({"type": "pong", "fw": "mock", "mac": "MO:CK:00:00:00:02",
                                      "node_known": True}))

    def close(self) -> None:
        self._stop.set()