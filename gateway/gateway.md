# PC Gateway (`gateway/gateway.py`) – Quy trình hoạt động

`gateway.py` là **cầu nối hai chiều** giữa Bridge (USB Serial, mỗi dòng một JSON) và CoreIOT (MQTT).
Đối với CoreIOT, gateway **đóng vai thiết bị `gardenNode1`**: kết nối bằng token của `gardenNode1`,
gửi telemetry, nhận RPC như một thiết bị kết nối trực tiếp.

```
Bridge (thật hoặc giả) <--- 1 dòng JSON ---> gateway.py <--- MQTT ---> CoreIOT (gardenNode1)
```

Tiêu chí phục vụ: **T08** (tính năng F03, F04). Là nền cho T15, T10, T09.

---

## 1. Cấu trúc file

| Phần | Vai trò |
| --- | --- |
| Hằng số `TOPIC_*` | 5 topic MQTT, giống Node 3 và các script giả lập |
| `LOCAL_PUMP_SEC`, `LOCAL_COOLDOWN_SEC`, `ACK_TIMEOUT_SEC` | Thời gian tưới cục bộ (20 s), nghỉ giữa hai lượt (60 s), chờ Bridge xác nhận (2 s) |
| `load_env()` | Đọc `.env`: `COREIOT_HOST`, `COREIOT_PORT`, `NODE1_TOKEN`, `BRIDGE_PORT` |
| `setup_logging()` | Ghi log có mili giây ra màn hình và vào `logs/gateway.log` |
| `make_client()` | Tạo MQTT client, chạy được với cả paho-mqtt 1.x và 2.x |
| `SerialBridge` | Bridge thật qua cổng COM, cùng giao diện `readline / write_line / close` với `MockBridge` |
| `Gateway` | Toàn bộ logic: chiều lên, chiều xuống, chế độ offline, thống kê |
| `main()` | Đọc tham số, chọn Bridge thật hay giả, khởi động `Gateway` |

---

## 2. Khởi động

```
main()
 ├─ 1. load_env(".env")
 ├─ 2. đọc tham số dòng lệnh: --mock, --port, --soil-min, --period, --loss ...
 ├─ 3. kiểm tra có token -> thiếu thì dừng
 ├─ 4. setup_logging()
 ├─ 5. chọn Bridge:  --mock -> MockBridge  |  ngược lại -> SerialBridge(COMx)
 └─ 6. Gateway(...).run()
```

`Gateway.__init__`:

- Tạo trạng thái: `connected`, `buffer` (hàng đợi gửi bù, tối đa 5000 bản ghi), `soil_min`,
  `last_values`, biến đếm gói nhận / mất.
- Tạo MQTT client, username = token, gắn `on_connect`, `on_disconnect`, `on_message`.
- `reconnect_delay_set(1, 30)`: mất kết nối thì tự nối lại, thời gian chờ tăng dần 1 → 30 s.

`run()`:

1. `connect_async(...)`: bắt đầu kết nối CoreIOT nhưng không đứng chờ. Không có Internet lúc khởi động
   thì gateway vẫn đọc Bridge ngay.
2. `loop_start()`: bật **luồng nền** của paho, lo toàn bộ phần MQTT.
3. Gửi `{"type":"ping"}` xuống Bridge để kiểm tra Bridge còn hoạt động.
4. Vào vòng lặp chính.

---

## 3. Hai luồng chạy song song

| Luồng | Làm gì | Hàm |
| --- | --- | --- |
| **Vòng lặp chính** (`run`) | Đọc từng dòng từ Bridge, xử lý; mỗi 60 s in thống kê | `handle_bridge_line`, `process_tel`, `local_rule` |
| **Luồng nền paho** | Giữ kết nối MQTT, nhận tin từ CoreIOT, tự nối lại | `on_connect`, `on_disconnect`, `on_message`, `handle_rpc` |

Cần hai luồng vì lệnh từ CoreIOT có thể đến bất cứ lúc nào, kể cả khi vòng lặp chính đang chờ dòng
Serial tiếp theo. Hai luồng phối hợp qua:

- `write_lock`: không cho hai luồng cùng ghi xuống Bridge, tránh hai lệnh bị trộn.
- `ack_event`: luồng nền gửi lệnh rồi chờ; vòng lặp chính đọc được dòng `ack` thì báo cho luồng nền.

---

## 4. Chiều lên: Node 1 → CoreIOT

```
Bridge in: {"type":"tel","node":1,"seq":12,"temp":30.1,...,"rssi":-56}
   │
   ▼ run(): readline()
handle_bridge_line(line)
   ├─ không bắt đầu bằng "{" -> log của Bridge: ghi [BRIDGE-LOG], dừng
   ├─ JSON hỏng              -> cảnh báo, bỏ qua
   └─ "type" = "tel"         -> process_tel(msg)
         ├─ 1. so seq với seq trước: hụt bao nhiêu -> cộng vào stat_lost
         ├─ 2. lấy temp, humi, soil, light, pump, rssi, seq
         ├─ 3. thêm sensor_ok = (temp và humi không null)
         ├─ 4. bỏ trường null
         ├─ 5. publish_tel(values, ts = thời điểm hiện tại, ms)
         │      ├─ đang kết nối, gửi thành công -> xong
         │      └─ chưa kết nối / gửi lỗi        -> đưa vào buffer
         └─ 6. đang mất kết nối -> local_rule(values)
```

- **Gửi dạng `{"ts": ..., "values": {...}}`**: dữ liệu gửi bù sau khi mất mạng vẫn hiện đúng thời điểm
  đo trên biểu đồ. Không có `ts` thì CoreIOT gán thời điểm nhận và mọi bản ghi gửi bù dồn vào một giây.
- **`sensor_ok` do gateway suy ra**: firmware Node 1 không gửi trường này. Khi DHT20 lỗi, Bridge đổi
  `NAN` thành `null`; gateway thấy `temp` là `null` thì đặt `sensor_ok = false`, nên alarm `Loi cam bien`
  của profile `GardenIrrigation` hoạt động được.

---

## 5. Chiều xuống: CoreIOT → Node 1 (RPC)

```
CoreIOT (dashboard hoặc RuleChain) publish:
  v1/devices/me/rpc/request/42   {"method":"setPump","params":{"on":true,"dur":20}}
   │
   ▼ luồng nền: on_message -> handle_rpc("42", body)
      ├─ đọc params: {on, dur} hoặc true/false
      ├─ send_cmd(on, dur):
      │    ├─ xóa ack_event
      │    ├─ (giữ write_lock) ghi xuống Bridge: {"type":"cmd","node":1,"pump":1,"dur":20}
      │    └─ chờ ack_event tối đa 2 s ...
      │          vòng lặp chính đọc {"type":"ack","ok":true} -> last_ack, set ack_event
      │    └─ trả True/False theo last_ack["ok"]
      └─ publish v1/devices/me/rpc/response/42   {"pump":1,"ok":true}
```

- Gateway **chờ ack rồi mới trả lời**, nên CoreIOT biết lệnh có thật sự tới Node 1. Không có ack trong
  2 s thì trả `{"ok": false}`.
- `getPump` trả trạng thái bơm trong gói telemetry gần nhất (`last_values`).
- Node 1 gửi ngay một gói telemetry có `pump: 1` sau khi nhận lệnh; gói này đi theo chiều lên, nên nút
  bơm trên dashboard cập nhật.

---

## 6. Nhận ngưỡng `soil_min`

`on_connect` mỗi lần kết nối:

1. Subscribe RPC, `attributes`, `attributes/response/+`.
2. Gửi client attribute `connected_via = pc-gateway`.
3. Hỏi `soil_min` hiện tại: `{"sharedKeys":"soil_min"}`.
4. Gửi bù dữ liệu còn trong `buffer`.

`apply_shared` cập nhật `self.soil_min` khi nhận câu trả lời lúc kết nối, hoặc khi `soil_min` bị sửa
trên dashboard. Đây là lý do `soil_min` là **shared attribute**: gateway phải biết ngưỡng trước khi mất
mạng để còn tự tưới khi offline.

---

## 7. Chế độ mất Internet (F04)

```
Mất mạng
  └─ luồng nền: on_disconnect -> connected = False
       └─ paho tự thử nối lại (1 s, 2 s, ... tối đa 30 s)

Trong lúc mất mạng, mỗi gói tel:
  ├─ publish_tel -> buffer (tối đa 5000 bản ghi, cũ nhất bị bỏ khi đầy)
  └─ local_rule:
       ├─ bơm đang chạy hoặc chưa hết thời gian nghỉ -> bỏ qua
       └─ soil < soil_min -> send_cmd(True, 20 s, wait_ack=False)
                              lượt sau ít nhất 20 + 60 = 80 s nữa

Có mạng lại
  └─ on_connect -> subscribe lại, hỏi lại soil_min, flush_buffer() gửi hết buffer
```

- **`wait_ack=False`**: `local_rule` chạy trong vòng lặp chính, mà chính vòng lặp chính mới đọc được dòng
  `ack`. Đứng chờ ở đây thì luôn hết 2 s và báo thất bại. RPC chạy ở luồng nền nên chờ được.
- **Nghỉ 60 s** giữa hai lượt tưới cục bộ: nước cần thời gian ngấm, tưới liên tục sẽ gây ngập.

---

## 8. Thống kê và kết thúc

- Mỗi 60 s: `[STAT] nhan N goi, mat M goi (x.x%), cho gui bu K` – số liệu mất gói ESP-NOW cho Chương 4.
- Mọi dòng log có thời điểm đến mili giây trong `logs/gateway.log` – so với thời điểm trên CoreIOT để đo
  độ trễ.
- **Ctrl+C**: dừng luồng nền, ngắt MQTT, đóng Bridge.

---

## 9. Một vòng hoạt động hoàn chỉnh

```
t=0     run(): connect_async, loop_start, ping Bridge
t=0.5   [luồng nền] on_connect: subscribe, hỏi soil_min -> 30
t=10    [chính] tel soil 27 -> publish
        RuleChain: 27 < 30 -> RPC setPump {on, dur 20}
        [luồng nền] handle_rpc -> send_cmd: ghi cmd, chờ ack
        [chính] đọc ack -> set ack_event; đọc tel pump 1 -> publish
        [luồng nền] trả RPC {"pump":1,"ok":true}
t=30    [chính] tel pump 0 (bơm tự tắt) -> publish
t=60    Wi-Fi tắt -> on_disconnect: connected = False
t=70    [chính] tel soil 28 -> buffer; local_rule: 28 < 30 -> tự tưới 20 s
t=150   Wi-Fi bật -> on_connect: flush_buffer gửi bù kèm ts gốc
```

---

## 10. Liên hệ tiêu chí chấm điểm

| Yêu cầu T08 | Phần code |
| --- | --- |
| Tổng hợp dữ liệu từ thiết bị local | `handle_bridge_line` → `process_tel` |
| Gửi lên CoreIOT | `publish_tel`, `flush_buffer` |
| Nhận lệnh điều khiển từ CoreIOT | `on_message` → `handle_rpc` |
| Phân phối lệnh cho thiết bị trong mạng local | `send_cmd` → Bridge → ESP-NOW → Node 1 |

Dùng lại cho các mục nâng cao:

- **T15**: đổi `publish_tel`, `handle_rpc` sang Gateway API (`v1/gateway/...`, kèm tên thiết bị con).
- **T10**: giao diện web của gateway đọc `last_values`.
- **T09**: chuỗi telemetry đi qua `process_tel` làm đầu vào cho mô hình phát hiện bất thường.

---

## 11. Bridge giả (`gateway/mock_bridge.py`)

Đóng vai Node 1 + Bridge, cùng giao diện `readline / write_line / close` với `SerialBridge`:

- Luồng nền mỗi giây: đất −0.3 % (không bơm) hoặc +0.6 % (đang bơm), giới hạn 5–95 %; gửi telemetry
  mỗi `period` giây và ngay khi bơm tự tắt.
- `--loss`: bỏ ngẫu nhiên một phần gói (seq vẫn tăng) để giả lập mất gói ESP-NOW.
- `write_line`: nhận `cmd` → đặt thời gian bơm (tối đa 60 s), trả `ack`, gửi ngay telemetry;
  nhận `ping` → trả `pong`.
- Không mô phỏng độ trễ thật của ESP-NOW/USB, lỗi cổng COM, nhiễu motor: các số đo độ trễ và tầm xa
  vẫn phải đo trên board thật.
