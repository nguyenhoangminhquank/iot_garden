#pragma once

// ---------- Mạng ----------
#define WIFI_SSID      "TEN_WIFI"          // chỉ Wi-Fi 2.4 GHz
#define WIFI_PASS      "MAT_KHAU_WIFI"

// ---------- CoreIOT ----------
#define COREIOT_HOST   "app.coreiot.io"
#define COREIOT_PORT   1883
#define DEVICE_TOKEN   "TOKEN_CUA_GARDENNODE3"

// ---------- Chân kết nối (sửa theo sơ đồ chân YoloUNO khi có board) ----------
#define PIN_SDA        11      // I2C SDA cho DHT20
#define PIN_SCL        12      // I2C SCL cho DHT20
#define PIN_LIGHT      1       // cảm biến ánh sáng – phải là ADC1 (GPIO1..GPIO10)
#define PIN_FAN        5       // relay quạt
#define PIN_LAMP       6       // relay đèn

// Relay kích mức thấp thì để LOW, kích mức cao thì đổi thành HIGH
#define RELAY_ACTIVE   LOW

// 1 nếu cảm biến cho giá trị CAO khi TỐI (thường gặp ở module quang trở), 0 nếu ngược lại
#define LIGHT_INVERT   1