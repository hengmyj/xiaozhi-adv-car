# xiaozhi-ADV-car

Cardputer ADV 小智 fork + 多页启动器。

**文档索引**：[docs/README.md](docs/README.md)

## Learned User Preferences

- 启动器/机载 UI 在 CJK 会乱码处用 ASCII 标签（如 Car / SpiderBot / IceBox）
- Cardputer ADV 页面：Fn 仅用于切页；页内车控/IR 键无需 Fn；红外按键即时发码（无需 Enter）
- Cardputer 已有键盘：配网优先机上扫列表+输密码，勿默认依赖 SoftAP/手机网页配网
- `./flash.sh` + `./monitor.sh`（Radio 改 8m 分区表需整包烧录）；卡住监视器用 `./monitor.sh kill` 或 Ctrl+C
- `./monitor.sh` 用 openpty() 给 idf_monitor 真 PTY（Cursor 集成终端也能看日志，不依赖 stdin 的 tcgetattr）；Ctrl+C 结束；idf_monitor 立刻失败则 pyserial 只读降级。包装脚本用 fd 3 heredoc，勿把 stdin 变成管道
- 不宜 ESP-Brookesia；构建 ESP-IDF ≥5.4（`~/.espressif/v5.5.3/esp-idf`）

## Learned Workspace Facts

- 硬件：Cardputer ADV（S3FN8、8MB、无 PSRAM）；板型/OTA `m5stack-cardputer-adv-car`
- 页面与按键：[docs/architecture/pages-and-keys.md](docs/architecture/pages-and-keys.md)；MQTT：[docs/mqtt/car-mqtt-control.md](docs/mqtt/car-mqtt-control.md)
- 键盘配网：`wifi_config_ui` 用 overlay，禁止 `lv_obj_clean(lv_scr_act())`；W/S 等重操作经 `Application::Schedule` 到主任务，否则易重启
- Radio：OnEnter 勿阻塞开流（`Schedule StartStream`）；进页先 `CaptureAudioExclusive` 再 BuildPanel；离页 StopStream + **DestroyPanel**（24 柱不可 hidden 常驻）。进 Radio 前 PageManager **sweep Destroy 所有其他独占页（含 Launcher）**，只留 Chat+Radio。`largest < 20KB` 只告警，以 decoder `MEM_LACK` 为准。离独占页勿立刻 `RestoreAudioModels`（Fn+1 进 Chat 且 `ShowPage` 完成后再 `ScheduleChatAudioRestore`：仅曾经 `ReleaseAudioModels` 才 `RecycleDevice` + 成对重建 encoder+decoder；开机 / 未 Release 跳过，勿对刚 Initialize 的 codec close+I2S-disable）；开机 `PageManager::Initialize` 只 `ShowChatUi`，勿走 Chat `OnEnter`（此时 `AudioService::codec_` 仍为空）。无 PSRAM 下只走 HTTP MP3 低码率。Radio↔Car/Music 二次无声见 [docs/audio/radio.md](docs/audio/radio.md)。Music Leave 须关麦、释放 mic_buf_ 并 DestroyPanel；**Car/Spider/IceBox 离页去 Launcher/Chat/Radio 时 ReleaseResidentUi（Destroy 仪表盘）**，Clock/Matrix Leave 一律 DestroyPanel（canvas 是 BSS，Destroy 只放 LVGL 对象）；Car/Spider/IceBox 互切仍复用 panel，避免 IceBox→Car 卡死。勿在 esp_timer Tick 里并发 close codec
- 音频内存（无 PSRAM）：进 Radio 仅剩 27KB → MP3 解码器 `MEM_LACK`；`AudioService` 常驻 Opus 占 43KB，`ReleaseAudioModels()` 后回到 70KB。本板 `CONFIG_WAKE_WORD_DISABLED=y` 无 AFE，释放唤醒词模型无效
- `esp_audio_simple_dec_open()` 惰性：只占 148B 即返回成功，真解码器等第一帧才分配——「open 成功」≠ 能解码
- Radio 流任务必须 **core 0 / prio 3**（`taskLVGL` 在 core1/prio1，抢了就触发 `task_wdt: CPU 1: taskLVGL`）
- `cyberwisk/M5Cardputer_WebRadio` 不可照搬：原版 Cardputer 有 8MB PSRAM，其 HTTPS+128kbps 在 ADV 上跑不起来
- 拿不到解码器格式信息时宁可静音；把未解码 MP3 字节当 PCM 播就是滋啦噪音来源
- 勿设 `CONFIG_ESP_CONSOLE_NONE=y`：panic 无处输出，重启循环看不到 backtrace
- 串口出现 `Device not configured` / reconnect 等待：USB CDC 常已断开，需拔插并确认 `/dev/cu.usbmodem*`；macOS Cmd+C 不会停 idf_monitor
- 远程仓库：`https://github.com/hengmyj/xiaozhi-adv-car`
