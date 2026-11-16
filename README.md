# Vườn rau thông minh – BTL IoT

Hệ thống giám sát và tưới tự động cho vườn rau mini, kiến trúc hybrid:
- Nhánh qua gateway: Node 1 (tưới) –ESP-NOW→ Bridge –USB Serial→ PC Gateway –MQTT→ CoreIOT
- Nhánh trực tiếp: Node 3 (khí hậu) –Wi-Fi/MQTT→ CoreIOT

## Cấu trúc thư mục
- `firmware/node3_climate/` – firmware Node 3 (PlatformIO)
- `firmware/node1_garden/`, `firmware/bridge/` – làm ở Giai đoạn 3
- `gateway/` – PC Gateway (Python), Giai đoạn 4
- `coreiot/` – script RuleChain, file export dashboard/rule chain
- `tools/sim_node1.py` – giả lập Node 1 để thử CoreIOT
- `docs/` – sơ đồ, ảnh, ghi chú chân cắm

## Chạy nhanh
```
python -m venv venv
venv\Scripts\activate
pip install -r requirements.txt
copy .env.example .env      # rồi điền host, token
python tools/sim_node1.py --token <NODE1_TOKEN> --host <COREIOT_HOST>
```
