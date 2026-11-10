**Bài 3 — Điều khiển thiết bị an toàn**

File trong repo này
- `control.ino` — firmware máy trạng thái DỪNG/CHẠY/LỖI (có ISR + timer)
- `fsm_diagram.png` — sơ đồ trạng thái
- `test_case_1.mp4` / `test_case_2.mp4` — video 2 ca thử trên mô hình nhà kính
---

4.1 Cấu hình phần cứng
Vi điều khiển trung tâm: Doit ESP32 Devkit V1.

Sơ đồ không nối bơm, quạt, heater hay dải LED trực tiếp vào chân GPIO. GPIO chỉ phát tín hiệu điều khiển; dòng tải đi qua driver công suất. Các mạch động cơ dùng cầu H A4950, còn heater và một số dải LED dùng driver N531 + MOSFET 30N06. Sơ đồ không thể hiện relay.

| Tải | GPIO / tín hiệu điều khiển | Tầng công suất và đầu nối tải | Nguồn tải |
|---|---|---|---|
| Bơm / Motor 1 | ESP32 GPIO19 → MOTOR1_INT1, GPIO18 → MOTOR1_INT2 | R37/R53 22 Ω → U9 A4950; tải nối tại J9, MOTOR 01. Sơ đồ tổng thể gọi đây là động cơ DC JGA25; nếu cụm bơm của nhóm dùng động cơ này thì đây là kênh bơm. | Chân VBB của A4950 lấy từ bus VDC. |
| Motor 2 | ESP32 GPIO23 → MOTOR2_INT1, GPIO22 → MOTOR2_INT2 | R33/R34 22 Ω → U3 A4950; tải nối tại J10. | VDC. |
| Dải LED phía ESP32 | ESP32 GPIO13 (`ARRAY_LED3`), GPIO12 (`ARRAY_LED4`) | Tầng driver N531 + MOSFET 30N06 trên trang ESP32; tải nối qua các đầu ra dải LED. | VDC ở phía tải. |
| Dải LED phía Arduino Uno | Uno D3 (`PWM_LED_U1`), D5 (`PWM_LED_U2`) | U5/U6 N531 + Q7/Q8 30N06; đầu tải lần lượt tại J12/J6. | VDC. |
| Heater | Uno D13 (`PWM_HEATER`) | U8 N531 + Q9 30N06; tải tại J7. Đây là công tắc MOSFET, không phải relay. | VDC. |
| Servo mái che SG90 | Uno D9 (`PWM_SG`) | Tín hiệu qua R77 22 Ω tới đầu nối servo J16; không có cầu H riêng vì SG90 đã có mạch điều khiển bên trong. | +5 V và GND. |
| LED báo đỏ/xanh | ESP32 GPIO26 (`LED_R`), GPIO27 (`LED_G`) | Transistor NPN MMBT3904, điện trở base 1 kΩ và kéo xuống 10 kΩ. | LED lấy từ +5 V. |
| Buzzer | ESP32 GPIO21 (`BUZZ`) | Transistor NPN MMBT3904, điện trở base 1 kΩ và kéo xuống 10 kΩ. | Buzzer lấy từ +5 V. |


Bơm và động cơ DC
GPIO của ESP32 chỉ nối vào chân logic IN1/IN2 của U9, không cấp dòng cho motor. Motor nối tại J9 vào OUT1/OUT2 của A4950. Theo bảng trạng thái của A4950:
| IN1 | IN2 | Trạng thái cầu H |
|---:|---:|---|
| LOW | LOW | Motor không được chủ động cấp điện |
| HIGH | LOW | Quay một chiều |
| LOW | HIGH | Quay chiều ngược lại |
| HIGH | HIGH | Phanh |

<img width="650" height="700" alt="image" src="https://github.com/user-attachments/assets/65c21f41-c404-4a33-98a6-fcb22d93eac8" />


<img width="1400" height="720" alt="image" src="https://github.com/user-attachments/assets/db5aaf5f-5bb2-41b5-8f8e-85c04d6c5f9b" />


<img width="1450" height="720" alt="image" src="https://github.com/user-attachments/assets/cdccdf20-053e-436d-9596-9abe1fff5291" />


**Nguồn công suất cho tải**

Ở trang nguồn, adapter đi qua công tắc, cầu chì tự phục hồi và khối DC/DC. Khối TPS54331 được ghi 12,6 V / 3 A, tạo rail +12C. Nguồn pin +BATT và rail +12C đi qua hai diode SS54 D5/D4 tới bus VDC. Vì vậy, bơm/motor, heater và tải dải LED lấy điện từ VDC của bo nguồn.
<img width="2600" height="1200" alt="image" src="https://github.com/user-attachments/assets/4f534aeb-6c2f-402b-9513-b006d6dafe44" />

Với Motor 1, schematic còn có điện trở cảm biến dòng R63 = 0,2 Ω và chân VREF nối +5VA. Theo hệ số cảm nhận dòng của A4950, ngưỡng giới hạn danh định xấp xỉ 2,5 A với các giá trị này; đây không thay thế việc kiểm tra dòng kẹt của motor và khả năng tản nhiệt của IC. Chọn bơm sao cho điện áp phù hợp VDC và dòng khởi động/kẹt không vượt giới hạn thiết kế.


Trạng thái khi vừa cấp nguồn
- GPIO ESP32/Arduino: trước khi sketch cấu hình, các chân điều khiển chưa được thiết lập làm output. Với Motor 1/2, điện trở kéo xuống nội tại của A4950 giữ IN1/IN2 ở LOW; trạng thái LOW/LOW là coast/standby, nên motor không được chủ động chạy. Datasheet A4950 xác nhận các điện trở kéo xuống.
- Firmware bơm: IN1 = GPIO19, IN2 = GPIO18. Khi khởi tạo, firmware ghi LOW vào hai chân trước khi chuyển chúng thành OUTPUT.
- LED báo và buzzer: các transistor điều khiển có điện trở kéo xuống 10 kΩ ở base; theo mạch thể hiện, chúng được giữ tắt khi chưa có tín hiệu điều khiển.
- Heater và dải LED qua N531: schematic không thể hiện rõ điện trở kéo xuống ở đầu vào điều khiển các N531. Vì vậy không chắc các tải này luôn OFF trong mọi tình huống reset chỉ dựa vào sơ đồ.
- Servo: D9 chưa phát xung PWM trước khi chương trình khởi tạo.
---

4.2 Ngắt ngoài

Ba cơ chế dừng/điều khiển

| Cơ chế | Hành vi mong muốn | Tình trạng |
|---|---|---|
| **Điều khiển dừng theo ý muốn** | Bơm tự bật/tắt theo điều khiển của người sử dụng.|Nút bấm trên Web gửi lệnh Bật/Tắt qua giao thức MQTT/HTTP xuống ESP32 để xuất tín hiệu ra chân GPIO đóng/ngắt Relay điều khiển máy bơm.
| **Giới hạn thời gian** | Khi bơm chạy đủ **20 giây**, tự chuyển về `STOP` và tắt đầu ra, kể cả khi không nhận được lệnh dừng từ web. |`updateState()` so sánh thời gian bằng phép trừ `millis()`. Khi hết hạn, `pumpRequested` bị xóa và hai đầu vào cầu H về LOW. |
| **Ngưỡng cảm biến** | Tắt bơm khi giá trị cảm biến đạt ngưỡng dừng đã hiệu chuẩn. Ngưỡng phải thuộc đúng loại cảm biến và chiều đo của nó. |`autoThreshold = 30 °C` (web chấp nhận 0–60 °C) hiện chỉ dùng để đổi LED cảnh báo khi nhiệt độ đạt/vượt ngưỡng. Và người dùng hoàn toàn có thể chọn ngưỡng
---
4. Kết quả
Hệ thống được thiết kế theo máy trạng thái hữu hạn (FSM) gồm 3 trạng thái: `DỪNG (IDLE)`, `CHẠY (RUNNING)` và `LỖI (FAULT)`, trong đó mọi nhánh sự cố hoặc hết giờ đều có độ ưu tiên cao nhất để đưa tải về trạng thái an toàn (`PWM = 0`).

Ca kiểm thử (Test Case)


**Ca 1**: Điều khiển máy bơm (`test_case_1.mp4`)	Nhiệt độ đo được hợp lệ ($31,5^\circ\text{C} > 30^\circ\text{C}$) hoặc nhận lệnh BẬT hợp lệ; bấm nút dừng trước khi hết giờ (tại $t = 5\text{s}$).	Hệ thống chuyển từ `DỪNG` $\rightarrow$ `CHẠY`, bật tải PWM ở mức 60%. Khi bấm nút ngắt ngoài tại giây thứ 5, ISR kích hoạt cờ dừng, hệ thống lập tức ngắt PWM về `0` và chuyển về `DỪNG`.

<img width="667" height="50" alt="image" src="https://github.com/user-attachments/assets/989c16a6-2243-4230-92c1-5fdc11bd076c" />



**Ca 2**: Quá thời gian cho phép - Timeout (`test_case_2.mp4`)	Kích hoạt cho hệ thống vào trạng thái `CHẠY` và để chạy liên tục vượt quá ngưỡng thời gian an toàn tối đa ($T_{\text{max}} = 20\text{ giây}$).	Đúng tại mốc $20.000\text{ ms}$, Timer phần cứng báo hết giờ (Timeout). Hệ thống tự động cưỡng bức tắt tải (`PWM = 0`), in cảnh báo `[TIMEOUT]` ra Serial và chuyển về trạng thái `DỪNG` (hoặc khóa an toàn chờ xác nhận).	

<img width="619" height="36" alt="image" src="https://github.com/user-attachments/assets/edf0113f-6bdb-4f6e-b8dd-4c805ef4ef74" />


```
