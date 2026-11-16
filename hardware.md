# Cấu hình phần cứng – Vườn rau thông minh

Tài liệu này mô tả linh kiện và cách đấu nối của từng node. Số chân GPIO dưới đây là **giá trị dự kiến**,
khớp với `config.example.h` của từng firmware. Khi có board, đối chiếu sơ đồ chân YoloUNO, chạy
`firmware/hw_test` (nếu có) rồi cập nhật lại cả file này lẫn `config.h`.

Sơ đồ tổng quát:

```
Node 1 (tưới)  --ESP-NOW-->  Node 2 (Bridge)  --USB Serial-->  PC Gateway  --MQTT-->  CoreIOT
Node 3 (khí hậu)  --------------------------- Wi-Fi / MQTT ---------------------------->  CoreIOT
```

---

## 1. Node 1 – Node tưới (kết nối qua gateway, không dùng Wi-Fi)

Firmware: `firmware/node1_garden_controls`  –  Thiết bị trên CoreIOT: `gardenNode1`

### 1.1. Linh kiện

| # | Linh kiện | SL | Điện áp | Ghi chú |
| --- | --- | --- | --- | --- |
| 1 | YoloUNO (ESP32-S3) | 1 | 5 V qua USB-C | Cấp nguồn bằng sạc USB khi chạy độc lập |
| 2 | DHT20 (nhiệt độ, độ ẩm không khí) | 1 | 3.3 V | I2C, địa chỉ 0x38 |
| 3 | Cảm biến độ ẩm đất **điện dung** v1.2 / v2.0 | 1 | 3.3 V | Ngõ ra analog AOUT. Không dùng loại 2 chân điện trở (bị ăn mòn) |
| 4 | Module cảm biến ánh sáng (quang trở, có chân AO) | 1 | 3.3 V | Ngõ ra analog |
| 5 | Module relay 1 kênh 5 V (opto cách ly, kích được bằng 3.3 V) | 1 | 5 V | Điều khiển bơm |
| 6 | Bơm chìm mini 5 V + ống 6–8 mm + hộp nước | 1 bộ | 5 V | Dòng khoảng 0.2–0.5 A |
| 7 | Nguồn 5 V 2 A riêng (adapter + jack DC) | 1 | 5 V | Dùng chung với Node 3 nếu đặt gần nhau |

### 1.2. Đấu nối

| Linh kiện | Chân linh kiện | Nối vào | GPIO dự kiến | Macro `config.h` |
| --- | --- | --- | --- | --- |
| DHT20 | VCC / GND | 3V3 / GND của board | – | – |
| DHT20 | SDA / SCL | Cổng I2C của board | 11 / 12 | `PIN_SDA` / `PIN_SCL` |
| Cảm biến đất | VCC / GND | 3V3 / GND | – | – |
| Cảm biến đất | AOUT | Chân **ADC1** | 2 | `PIN_SOIL` |
| Cảm biến ánh sáng | VCC / GND | 3V3 / GND | – | – |
| Cảm biến ánh sáng | AO | Chân **ADC1** | 1 | `PIN_LIGHT` |
| Relay bơm | VCC / GND | 5V / GND của board | – | – |
| Relay bơm | IN | Chân digital | 5 | `PIN_PUMP` |
| Bơm | + | Nguồn 5 V riêng (+) → COM relay → NO relay → bơm (+) | – | – |
| Bơm | − | Nguồn 5 V riêng (−), **nối chung GND với board** | – | – |

### 1.3. Hiệu chuẩn cảm biến đất

1. Để cảm biến ngoài không khí (khô hoàn toàn), ghi giá trị ADC thô → `SOIL_RAW_DRY`.
2. Nhúng cảm biến vào cốc nước tới vạch giới hạn, ghi giá trị ADC thô → `SOIL_RAW_WET`.
3. Firmware đổi giá trị thô sang 0–100 % bằng hai mốc này. Cảm biến điện dung cho giá trị **cao khi khô**, **thấp khi ướt**.

---

## 2. Node 2 – Bridge (cầu nối ESP-NOW ↔ USB Serial)

Firmware: `firmware/node2_bridge`  –  Không có thiết bị riêng trên CoreIOT (dữ liệu đi qua PC Gateway dưới tên `gardenNode1`)

Bridge không đo và không điều khiển gì. Nó nhận gói ESP-NOW từ Node 1, đổi sang một dòng JSON gửi lên
laptop qua USB, và chiều ngược lại. Phần cứng chỉ gồm board và cáp, nhưng **vị trí đặt** và **kết nối USB**
quyết định độ ổn định của cả nhánh gateway.

### 2.1. Linh kiện

| # | Linh kiện | SL | Ghi chú |
| --- | --- | --- | --- |
| 1 | YoloUNO (ESP32-S3) | 1 | Không cắm thêm cảm biến hay relay |
| 2 | Cáp USB-C **có truyền dữ liệu** | 1 | Cáp chỉ sạc cấp nguồn được nhưng laptop không thấy cổng COM |
| 3 | Cáp nối dài USB 1–2 m (tùy chọn) | 1 | Đưa Bridge ra xa laptop, về phía Node 1, để sóng tốt hơn |

### 2.2. Kết nối

| Kết nối | Cách làm | Ghi chú |
| --- | --- | --- |
| Nguồn + dữ liệu | Cắm USB-C của Bridge vào laptop chạy PC Gateway | Một sợi cáp vừa cấp nguồn 5 V vừa truyền Serial 115200 baud |
| Cổng COM | Windows: **Device Manager → Ports (COM & LPT)**, rút ra cắm lại để biết cổng nào là Bridge | Ghi số cổng (ví dụ `COM5`) vào `.env` của PC Gateway: `BRIDGE_PORT=COM5` |
| Không gian sóng với Node 1 | ESP-NOW, kênh `ESPNOW_CHANNEL` = 1, không qua router | Node 1 và Bridge phải cùng kênh; Bridge **không** kết nối Wi-Fi router |

### 2.3. Vị trí đặt

| Quy tắc | Lý do |
| --- | --- |
| Đặt Bridge ở chỗ **nhìn thẳng** về phía Node 1 càng nhiều càng tốt | Tường bê tông, cửa kim loại, bể nước làm suy giảm sóng 2.4 GHz mạnh |
| Không đặt sát mặt kim loại hoặc phía sau màn hình laptop | Kim loại che và phản xạ sóng của anten trên board |
| Đặt cao hơn mặt bàn hoặc mặt đất một chút (kê lên hộp) | Giảm hấp thụ sóng từ mặt sàn và vật cản thấp |
| Khoảng cách thử nghiệm: 1 m, 5 m, 10 m, 20 m, qua 1 tường | Dùng cho phép đo tầm xa và tỉ lệ mất gói ở Chương 4 |
| Kiểm tra bằng **RSSI** trong dòng JSON của Bridge (nếu có): từ −70 dBm trở lên là tốt, dưới −85 dBm là yếu | Xem mục 2.5 |

### 2.4. Cài đặt laptop chạy PC Gateway

| Cài đặt | Lý do |
| --- | --- |
| Tắt chế độ ngủ khi cắm sạc (**Settings → System → Power**) | Laptop ngủ thì cổng COM đóng, Gateway mất dữ liệu |
| Tắt **USB selective suspend** (Control Panel → Power Options → Change advanced power settings → USB settings) | Windows có thể tự ngắt nguồn cổng USB khi rảnh, làm Bridge mất kết nối |
| Không mở Serial Monitor của PlatformIO khi Gateway đang chạy | Một cổng COM chỉ một chương trình mở được tại một thời điểm |

### 2.5. RSSI – cường độ tín hiệu nhận

RSSI (Received Signal Strength Indicator) là cường độ sóng mà **bên nhận** đo được khi nhận một gói,
đơn vị dBm, luôn là số âm: càng gần 0 càng mạnh.

| RSSI | Đánh giá |
| --- | --- |
| −30 đến −60 dBm | Rất tốt (đặt gần) |
| −60 đến −70 dBm | Tốt |
| −70 đến −85 dBm | Yếu, bắt đầu mất gói khi có nhiễu |
| Dưới −85 dBm | Rất yếu, mất gói thường xuyên |

Bridge đo RSSI của mỗi gói telemetry Node 1 gửi tới và ghi vào trường `rssi` của dòng JSON. Chỉ các
phiên bản nền tảng ESP32 dùng ESP-IDF 5 trở lên mới cung cấp RSSI trong callback ESP-NOW; với phiên bản
cũ hơn, dòng JSON không có trường `rssi`.

### 2.6. Ghi nhận khi lắp đặt

1. Nạp firmware, mở Serial Monitor, ghi dòng `MAC Bridge = ...` vào mục 5 và vào `BRIDGE_MAC` của Node 1.
2. Ghi số cổng COM vào mục 5 và vào `.env` (`BRIDGE_PORT`).
3. Gõ `{"type":"ping"}` → Bridge trả `{"type":"pong",...}`.
4. Đóng Serial Monitor trước khi chạy PC Gateway.

---

## 3. Node 3 – Node khí hậu (kết nối trực tiếp CoreIOT qua Wi-Fi)

Firmware: `firmware/node3_climate_monitors`  –  Thiết bị trên CoreIOT: `gardenNode3`

### 3.1. Linh kiện

| # | Linh kiện | SL | Điện áp | Ghi chú |
| --- | --- | --- | --- | --- |
| 1 | YoloUNO (ESP32-S3) | 1 | 5 V qua USB-C | |
| 2 | DHT20 | 1 | 3.3 V | I2C, địa chỉ 0x38 |
| 3 | Module cảm biến ánh sáng (có chân AO) | 1 | 3.3 V | Đặt tránh hướng chiếu của đèn (tránh vòng phản hồi) |
| 4 | Module relay 2 kênh 5 V (hoặc 2 module 1 kênh) | 1 | 5 V | Kênh 1: quạt, kênh 2: đèn |
| 5 | Quạt 5 V 40–50 mm | 1 | 5 V | |
| 6 | Đèn LED 5 V (dải LED hoặc module LED) | 1 | 5 V | Giả lập đèn trồng cây |

### 3.2. Đấu nối

| Linh kiện | Chân linh kiện | Nối vào | GPIO dự kiến | Macro `config.h` |
| --- | --- | --- | --- | --- |
| DHT20 | VCC / GND | 3V3 / GND | – | – |
| DHT20 | SDA / SCL | Cổng I2C | 11 / 12 | `PIN_SDA` / `PIN_SCL` |
| Cảm biến ánh sáng | VCC / GND / AO | 3V3 / GND / chân **ADC1** | 1 | `PIN_LIGHT` |
| Relay kênh 1 (quạt) | IN1 | Chân digital | 5 | `PIN_FAN` |
| Relay kênh 2 (đèn) | IN2 | Chân digital | 6 | `PIN_LAMP` |
| Relay | VCC / GND | 5V / GND của board | – | – |
| Quạt, đèn | + / − | Nguồn 5 V riêng qua COM–NO của relay, chung GND với board | – | – |

---

## 4. Lưu ý chung khi chọn chân và đấu nối (ESP32-S3)

| Quy tắc | Lý do |
| --- | --- |
| Cảm biến analog chỉ cắm vào **ADC1 (GPIO1–GPIO10)** | ADC2 không đọc được khi Wi-Fi hoặc ESP-NOW đang chạy |
| Tránh GPIO0, GPIO3, GPIO45, GPIO46 | Chân strapping, ảnh hưởng chế độ khởi động |
| Tránh GPIO19, GPIO20 | Dùng cho USB của chip |
| Tránh GPIO26–GPIO32 (và 33–37 trên module có PSRAM octal) | Nối với bộ nhớ flash / PSRAM bên trong module |
| Cảm biến dùng **3.3 V**, không cấp 5 V | Chân GPIO của ESP32-S3 chỉ chịu 3.3 V |
| Bơm, quạt, đèn dùng **nguồn 5 V riêng**, nối **chung GND** với board | Motor kéo sụt áp làm board reset; chung GND để tín hiệu có cùng mốc 0 V |
| Relay luôn khởi động ở trạng thái **tắt** | Firmware đặt relay tắt ngay trong `setup()`; kiểm tra lại `RELAY_ACTIVE` (LOW/HIGH) |
| Bơm có **giới hạn thời gian chạy** trên chính Node 1 | Mất liên lạc giữa chừng thì bơm vẫn tự tắt, không làm ngập chậu |

---

## 5. Bảng ghi nhận khi có phần cứng (điền sau)

| Node | Mục | Giá trị thực tế | Ngày kiểm tra |
| --- | --- | --- | --- |
| Node 1 | GPIO SDA / SCL / SOIL / LIGHT / PUMP | | |
| Node 1 | `SOIL_RAW_DRY` / `SOIL_RAW_WET` | | |
| Node 1 | `RELAY_ACTIVE` | | |
| Node 2 | Địa chỉ MAC | | |
| Node 2 | Cổng COM trên laptop | | |
| Node 2 | RSSI trung bình tại vị trí lắp đặt / khoảng cách tới Node 1 | | |
| Node 3 | GPIO SDA / SCL / LIGHT / FAN / LAMP | | |
| Node 3 | `RELAY_ACTIVE` / `LIGHT_INVERT` | | |
