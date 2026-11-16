#pragma once

// ---------- ESP-NOW ----------
// Địa chỉ MAC của Bridge (Node 2). Chưa biết thì để FF:FF:FF:FF:FF:FF (gửi broadcast)
// để thử trước; khi đã có MAC thật thì điền vào để có xác nhận nhận gói (ACK).
#define BRIDGE_MAC        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
#define ESPNOW_CHANNEL    1        // Node 1 và Bridge phải dùng CÙNG kênh
#define NODE_ID           1

// ---------- Chu kỳ và an toàn ----------
#define SEND_INTERVAL_MS  10000    // 10 s gửi một gói telemetry
#define MAX_RETRIES       3        // gửi lại tối đa 3 lần nếu không có ACK
#define PUMP_MAX_SEC      60       // bơm không bao giờ chạy quá 60 s mỗi lần, dù lệnh yêu cầu bao lâu

// ---------- Chân kết nối (sửa theo sơ đồ chân YoloUNO khi có board) ----------
#define PIN_SDA           11       // I2C SDA cho DHT20
#define PIN_SCL           12       // I2C SCL cho DHT20
#define PIN_SOIL          2        // cảm biến độ ẩm đất – phải là ADC1 (GPIO1..GPIO10)
#define PIN_LIGHT         1        // cảm biến ánh sáng – phải là ADC1
#define PIN_PUMP          5        // relay bơm

#define RELAY_ACTIVE      LOW      // relay kích mức thấp: LOW; kích mức cao: HIGH
#define LIGHT_INVERT      1        // 1 nếu cảm biến cho giá trị CAO khi TỐI

// ---------- Hiệu chuẩn cảm biến đất (đo khi có phần cứng, xem docs/hardware.md mục 1.3) ----------
#define SOIL_RAW_DRY      3000     // giá trị ADC khi để ngoài không khí (khô = 0 %)
#define SOIL_RAW_WET      1200     // giá trị ADC khi nhúng trong nước (ướt = 100 %)