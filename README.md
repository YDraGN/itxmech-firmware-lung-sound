# itxmech-respiratory-lung-sound

Firmware thu âm phổi cho RespiCare, chạy trên ESP32 38-pin với micro INMP441. Đây là bản thay thế cho `itxmech-respiratory-firmware` khi chuyển sang vi điều khiển mới. Firmware giữ nguyên hợp đồng với server: topic MQTT, các event, endpoint `upload-url` và `complete`. Phần xử lý tín hiệu là bộ lọc dải 20–2000 Hz ở mức biên độ giống dữ liệu huấn luyện ICBHI, thay cho chuỗi lọc thử nghiệm trong `test_INMP.ino` (lý do ở mục *Xử lý tín hiệu*).

```
Web ──HTTP──▶ MQTT gateway ──ASSIGN_VISIT (QoS1)──▶ ESP32
                                                     │ ghi 20 s WAV (I2S → DSP → LittleFS)
ESP32 ──POST upload-url──▶ Web API (Vercel)          │
ESP32 ──PUT WAV (stream)──▶ Supabase Storage         │
ESP32 ──POST complete────▶ Web API ──▶ phân tích AI ─┘
ESP32 ──event (QoS1)─────▶ MQTT gateway ──▶ Web cập nhật trạng thái
```

## Cấu trúc (phân lớp theo kiểu AIoT thông thường)

| Lớp | Thư mục | Vai trò | Test trên máy tính? |
|---|---|---|---|
| core | `src/core/` | Hằng số, timeout, chính sách retry/backoff | ✔ |
| dsp | `src/dsp/` | `LungFilter` (lọc dải Butterworth + gain), `GapConcealer` (bù mất mẫu bằng cách lặp lại) | ✔ |
| hal | `src/hal/` | Driver I2S cho INMP441, phát hiện tràn DMA | — |
| audio | `src/audio/` | Task thu âm, ring buffer, ghi WAV, WAV header | header ✔ |
| storage | `src/storage/` | 1 slot WAV trên LittleFS và checkpoint job trong NVS | — |
| net | `src/net/` | Wi-Fi + portal, MQTT TLS, uploader HTTPS streaming, API client | — |
| app | `src/app/` | State machine `RecordingFlow`, bố trí task, watchdog | — |
| debug | `src/debug/` | Chế độ stream PCM qua Serial/UDP cho bench test | — |
| tools | `tools/` | Script ghi WAV trên PC (`serial_record.py`, `udp_record.py`) | UDP ✔ |

### Task FreeRTOS

| Task | Core / prio | Công việc |
|---|---|---|
| `loopTask` | 1 / 1 | Wi-Fi, NTP, portal, vòng lặp MQTT, publish event |
| `worker` | 1 / 1 | Ghi âm → upload → complete → retry nền |
| `capture` | 1 / 5 | Chỉ tồn tại trong lúc ghi âm: đọc I2S, lọc, bù mất mẫu, đẩy vào ring buffer |

Các task chỉ giao tiếp với nhau qua queue. Vì vậy upload chậm không chặn keep-alive MQTT, và broker mất kết nối cũng không chặn việc ghi âm. Cả `loopTask` và `worker` đều nằm dưới task watchdog (120 s). Nếu một task bị treo, thiết bị sẽ reboot và tiếp tục từ checkpoint trong NVS.

## Xử lý tín hiệu

Chuỗi lọc của `LungFilter` theo thứ tự:

1. Chia giá trị I2S 32-bit cho 65536, tức là đưa full scale của micro về đúng full scale 16-bit.
2. High-pass Butterworth bậc 4 ở 20 Hz, chỉ để khử DC và trôi chậm.
3. Low-pass Butterworth bậc 4 ở 2000 Hz.
4. Gain ×2.
5. Soft limiter bắt đầu từ ±24000 và kẹp cứng ở ±30000. Đây chỉ là lớp an toàn.

**Mục tiêu là giống dữ liệu huấn luyện.** Mình đo bằng `tools/wav_report.py` trên các file ICBHI 2017 (ống nghe Meditron):

- RMS −24 tới −32 dBFS, peak −8 tới −18 dBFS, không có mẫu nào bị cắt.
- 89–99% năng lượng nằm dưới 150 Hz, gồm nhịp thở và tiếng tim truyền qua ống nghe.
- Trên 600 Hz gần như im lặng (−70 tới −86 dBFS).

INMP441 ở thang tự nhiên đã cho mức tương đương. Vì vậy high-pass phải để thấp: lọc 80 Hz sẽ xoá đúng phần model đã học, và model không tách được chu kỳ thở.

**Vì sao không giữ chuỗi lọc của `test_INMP.ino`:** chuỗi đó gồm `>>11`, ×3 và low-shelf ×2,4, cộng lại khoảng +40 dB so với thang tự nhiên. Trên bản ghi thật, 85% số mẫu đi vào vùng limiter, 51% chạm trần, crest factor chỉ 1,4 dB, và tiếng người nói trong phòng bị kéo lên đủ to để nghe rõ. Phần méo do bị cắt cũng bị model đọc thành crackle/wheeze.

Có ba tham số chỉnh qua `.env`: `MIC_GAIN` (mặc định 2), `MIC_HPF_HZ` (20) và `MIC_LPF_HZ` (2000). Hạ `MIC_LPF_HZ` xuống khoảng 1200 sẽ cắt bớt tiếng nói trong phòng, đổi lại mất một phần chi tiết crackle/wheeze. Cấu hình đang dùng được in ra lúc boot (`[DSP] band-pass ...`). Sau mỗi lần ghi, nếu quá 0,5% số mẫu bị quá tải thì log có `WARNING`, còn nếu `rms < 150` thì log có `NOTE`.

**Tạp âm môi trường** (tiếng người nói, nhạc) nằm trong cùng dải tần với tiếng thở, nên bộ lọc theo tần số không loại hết được. Cách hiệu quả nhất là làm kín ống nghe: micro gắn khít vào ống, không hở khí, có foam bọc quanh. Dùng `tools/wav_report.py` để so dải 600–2500 Hz giữa bản ghi của thiết bị và ICBHI; trong ICBHI dải này khoảng −70 dBFS.

Ngoài ra:

- 500 ms đầu (thời gian micro khởi động và bộ lọc ổn định) bị bỏ đi, không tính vào thống kê.
- Lúc boot, firmware tự kiểm tra cả hai kênh LEFT và RIGHT của I2S rồi dùng kênh có tín hiệu (`[I2S] self-test ...`).
- DMA dùng 16 × 512 mẫu (512 ms dự trữ). Ring buffer khoảng 1 s nằm giữa task thu âm và việc ghi flash, nên các lần xoá block LittleFS chậm không làm rơi mẫu. Hai bộ đệm này (khoảng 64 KB) chỉ được cấp trong lúc ghi âm và được trả lại trước khi upload, để còn đủ heap cho TLS.

### Khôi phục tín hiệu khi mất dữ liệu (lặp lại)

`GapConcealer` lấp chỗ trống bằng cách lặp lại 32 ms âm thanh tốt gần nhất, có fade-out tuyến tính. Sau 128 ms thì chuyển về im lặng. Nhờ vậy file luôn đúng thời lượng và không có tiếng click. Cơ chế này dùng ở hai chỗ:

1. **Trên thiết bị:** khi driver I2S báo `I2S_EVENT_RX_Q_OVF` (tức một buffer DMA bị bỏ), hoặc khi ring buffer đầy.
2. **Trên PC (`tools/udp_record.py`):** mỗi gói UDP có số thứ tự. Gói nào bị mất thì được lấp bằng cùng thuật toán này (Python mirror, có test đối chiếu với C++).

Log sau mỗi lần ghi cho biết chất lượng bản ghi:
```
[AUDIO] 640000/640000 bytes, concealed=0 in 0 gap(s) (dma=0 ring=0), limited=0, peak=9500, rms=1100, raw -9e7..7e7, flash worst=26ms slow=0
```
Nếu `peak=0` thì dữ liệu I2S thô là một hằng số: dây SD hở, micro mất nguồn, hoặc đọc sai kênh. Xem `[I2S] self-test` lúc boot. Bản ghi đó bị huỷ và firmware gửi `UPLOAD_FAILED`, không upload file rỗng.

## Upload: timeout, retry, chống treo

### Những điểm dễ gây nghẽn/treo trong firmware cũ (đọc từ code, chưa xác nhận trên phần cứng)

| # | Firmware cũ | Hậu quả | Firmware mới |
|---|---|---|---|
| 1 | `emit()` dùng `xQueueSend(..., portMAX_DELAY)` | Mất broker đủ lâu để queue 16 event đầy thì worker **treo vĩnh viễn** | Không chờ; nếu queue đầy thì bỏ event cũ nhất |
| 2 | Mọi lỗi đều về `ERROR` và `busy` mãi mãi | Thiết bị từ chối mọi lần khám mới cho tới khi nạp bản maintenance | Lỗi ghi âm: huỷ, gửi `UPLOAD_FAILED`, trở về IDLE. Lỗi mạng: chuyển sang *pending* và tự retry nền |
| 3 | `accepted.log` chặn trùng theo **visitId** | Không gán lại được cùng một lần khám sau khi lỗi | Chỉ chặn trùng theo **messageId** (16 cái gần nhất, trong NVS) |
| 4 | Không tự format LittleFS | **MCU mới chưa format thì boot báo lỗi và không hoạt động** | Tự format, nhưng chỉ khi mount thất bại *và* NVS không có job nào |
| 5 | Có WAV mồ côi (không có job) thì từ chối boot | Thiết bị đứng | Xoá WAV mồ côi (không thể giao file này cho lần khám nào) |
| 6 | `HTTPClient::sendRequest(PUT, stream)` một phát, timeout 60 s cho mỗi lần ghi | Không thấy tiến độ; mạng yếu có thể kẹt vài phút | Uploader tự viết: timeout cho từng giai đoạn, phát hiện stall, có deadline tổng, log tiến độ |
| 7 | Đọc I2S và ghi flash chung một vòng lặp, DMA chỉ 128 ms | Flash chậm làm mất mẫu mà không ai biết | Tách task + ring buffer, đếm và lấp mẫu mất |

### Giới hạn thời gian (`src/core/Config.h`)

| Giai đoạn | Giới hạn |
|---|---|
| TLS handshake | 15 s |
| Kết nối TCP / HTTP | 15 s |
| `upload-url` | 20 s |
| Mỗi lần ghi TLS, phát hiện stall khi không có byte nào đi được | 20 s |
| Thời gian tối đa cho body | 15 s + kích thước / 4 KB/s (khoảng 171 s cho 20 s âm thanh) |
| Chờ status line sau body | 30 s |
| `complete` (server kiểm tra WAV) | 60 s |
| Heap tối thiểu trước khi mở TLS | còn trống ≥ 40 KB và block liền ≥ 20 KB, thiếu thì retry thay vì crash |
| Task watchdog | 120 s |

### Chính sách retry

- Mỗi **phiên giao file** cho phép tối đa 4 lần thất bại, backoff 2 → 4 → 8 s (+ jitter), tổng ngân sách 5 phút. Mỗi lần thử đều xin URL ký mới.
- Phân loại mã lỗi: `2xx` là thành công. Lỗi mạng, `408`, `429`, `5xx` thì thử lại. Storage trả `400/401/403` thì xin URL mới. Các `4xx` khác là lỗi không thể sửa bằng retry: huỷ và gửi `UPLOAD_FAILED`.
- `409 RECORDING_ALREADY_UPLOADED` (PUT đã thành công nhưng thiết bị reboot trước khi kịp checkpoint) thì đi thẳng sang `complete`. `complete` trả `404` (Storage không có file) thì upload lại.
- Hết phiên mà vẫn chưa xong: WAV được **giữ lại** ở trạng thái *pending*, gửi `UPLOAD_FAILED` một lần, rồi retry nền với chu kỳ 1 → 2 → 4 … tối đa 10 phút. Retry chạy ngay khi MQTT kết nối lại, hoặc khi web gửi lệnh mới. Sau 24 h thì huỷ. Trong lúc pending, lệnh `ASSIGN_VISIT` mới bị từ chối (vì chỉ có 1 slot WAV).
- Mọi lần đổi phase đều được ghi vào NVS **trước** khi thực hiện hành động. Reboot giữa chừng sẽ tiếp tục đúng chỗ, không ghi âm lại và không upload sang recording ID khác. Reboot giữa lúc đang ghi âm thì huỷ bản ghi và gửi `UPLOAD_FAILED`.

### Đọc log upload

```
[API] upload-url -> 200 (ok)
[EVENT] UPLOAD_STARTED
[UPLOAD]  25%  163840/640044 B  48.2 KB/s  slowest write 210 ms  rssi -61
...
[UPLOAD] -> 200  (ok): 640044 B, connect 1830 ms, body 13250 ms (47.2 KB/s), response 420 ms, heap 142000/65524, rssi -61
[API] complete -> 200 (ok)
[FLOW] visit … delivered
[HEALTH] up 600s heap 150000 (min 98000, block 70000) wifi=1 mqtt=1 busy=0
```
- `connect` lớn hoặc `CONNECT_FAILED` kèm detail: DNS, CA hoặc NTP có vấn đề. Xem dòng `[NTP]`.
- `KB/s` thấp và `slowest write` cao: sóng yếu (xem `rssi`, cần > −70 dBm) hoặc AP bị nghẽn.
- `STALLED`: kết nối bị treo, không gửi được byte nào trong 20 s. Firmware tự huỷ và thử lại.
- `LOW_HEAP`: heap trống < 40 KB hoặc block liền < 20 KB. Hãy gửi lại log `[HEALTH]`.
- `[UPLOAD] detail:` là phần đầu của body lỗi từ Supabase, dùng để chẩn đoán URL hoặc token.

## Build và nạp

Trên máy này PlatformIO nằm trong venv của repo cũ: `../itxmech-respiratory-firmware/venv/Scripts/pio.exe`.

1. Sao chép `.env.example` thành `.env` rồi điền giá trị thật (lấy từ người quản lý, không commit).
2. Đặt file CA của broker MQTT vào `respicare-mqtt-ca.crt` (chỉ chứa chứng chỉ công khai).
3. Build và nạp (script build tự đọc `.env`, nên dùng được cả nút Build/Upload trong VS Code):
   ```bash
   pio run -e production -t upload
   pio device monitor -e production
   ```
   Không cần `uploadfs`: firmware tự format LittleFS trên MCU mới.

Nếu không đặt `WIFI_SSID`, thiết bị sẽ mở AP `RespiCare-Setup-<DEVICE_ID>`. Mật khẩu in ra Serial, trang cấu hình ở http://192.168.4.1/.

### Bench test cho team phần cứng

| Mục đích | Build | Ghi trên PC |
|---|---|---|
| Nghe thử qua USB, không cần mạng | `pio run -e stream_serial -t upload` | `python tools/serial_record.py --port COM11 --seconds 20` |
| Qua Wi-Fi giống `test_INMP.ino`, có bù gói mất | `WIFI_SSID=… WIFI_PASSWORD=… UDP_HOST=<IP máy PC> pio run -e stream_udp -t upload` | `python tools/udp_record.py --seconds 20` |

`udp_record.py` in ra tỉ lệ mất gói. Nếu trên 2% thì nên đổi vị trí hoặc kênh Wi-Fi trước khi đánh giá chất lượng âm.

### Yêu cầu Wi-Fi

- Chỉ dùng được **2.4 GHz** (802.11 b/g/n, kênh 1–13), bảo mật **WPA2-PSK** hoặc WPA/WPA2 mixed. Không hỗ trợ 5 GHz, WPA3-only, WPA2-Enterprise, WEP, và mạng có trang đăng nhập (captive portal).
- Mạng phải cho phép đi ra TCP 443 và cổng MQTT (8883).
- Lúc boot, firmware quét Wi-Fi và in `[WIFI-DIAG] RESULT`. Nếu dòng này báo SSID **không thấy trên 2.4 GHz** trong khi điện thoại vẫn thấy, thì mạng đó chỉ phát 5 GHz. Khi kết nối hỏng lặp lại, firmware quét lại. Mã lỗi ngắt kết nối luôn được in kèm giải thích.
- Sau khi có mạng, firmware in `[NET-DIAG]` cho broker, web và storage để phân biệt bị chặn cổng với lỗi chứng chỉ/mật khẩu. Nếu NTP bị chặn, firmware tự lấy giờ từ header `Date` của một phản hồi HTTP.
- Wi-Fi do cấu hình qua portal được lưu trong NVS và **ưu tiên hơn** `WIFI_SSID` trong `.env`.

## Kiểm thử

```bash
python tests/run_host_tests.py
```
Lệnh này biên dịch và chạy `tests/host/test_main.cpp` bằng g++ (đáp ứng tần số của bộ lọc dải, khả năng loại rung thấp mà không bão hoà, limiter, DC, concealment, WAV header, retry/backoff), sau đó chạy test Python cho `build_config.py` và `udp_record.py`.

## Khác biệt so với firmware cũ

- NVS namespace mới là `lungsound`, nên checkpoint của firmware cũ không bị đọc nhầm.
- Không còn bản build maintenance: các trạng thái từng phải sửa tay giờ được xử lý tự động (xem bảng ở trên).
- `RECORD_SECONDS` giới hạn trong 5–30 s, vì mỗi WAV phải vừa phân vùng LittleFS (~1,3 MB).
- `complete` gửi thêm metadata (`file_size_bytes`, `duration_ms`, `sample_rate_hz`, `bit_depth`, `channel_count`). Schema hiện tại của server đã chấp nhận các trường này.
