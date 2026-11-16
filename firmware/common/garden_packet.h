// Gói tin ESP-NOW giữa Node 1 và Bridge – DÙNG CHUNG cho cả hai firmware.
#pragma once
#include <stdint.h>

enum PacketType : uint8_t {
  PKT_TELEMETRY = 1,   // Node 1 -> Bridge: số đo và trạng thái bơm
  PKT_COMMAND   = 2,   // Bridge -> Node 1: lệnh bật/tắt bơm
};

typedef struct __attribute__((packed)) {
  uint8_t  type;       // PacketType
  uint8_t  node_id;    // 1 = Node 1
  uint16_t seq;        // số thứ tự gói, tăng dần – dùng để đo tỉ lệ mất gói
  float    temp;       // °C, NAN nếu DHT20 lỗi
  float    humi;       // %, NAN nếu DHT20 lỗi
  float    soil;       // % độ ẩm đất (0..100)
  float    light;      // % ánh sáng (0..100)
  uint8_t  pump;       // telemetry: trạng thái bơm; command: 1 = bật, 0 = tắt
  uint16_t dur;        // telemetry: số giây bơm còn chạy; command: số giây muốn bơm
} GardenPacket;

static_assert(sizeof(GardenPacket) == 23, "GardenPacket phai dung 23 byte o ca hai dau");