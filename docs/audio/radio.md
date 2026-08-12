# Radio 页：无 PSRAM 下的网络电台

Cardputer ADV（ESP32-S3FN8 + 8MB flash，**无 PSRAM**）上 Radio 页从「卡死无声」到「稳定播放」的调试结论。相关代码：

- [`main/boards/m5stack-cardputer-adv-car/pages/radio/radio_page.cc`](../../main/boards/m5stack-cardputer-adv-car/pages/radio/radio_page.cc)
- [`main/audio/audio_service.cc`](../../main/audio/audio_service.cc)

## 症状演变

卡死无声 → 滋啦噪音 + 重启 → 滴一声后静音 → 正常播放

## 根因：内部 SRAM 不足

本板没有 PSRAM（esptool 实测确认），所有音频缓冲只能进内部 SRAM。

| 现象 | 直接原因 |
|------|----------|
| 无声 | 进 Radio 时可用内部 SRAM 仅 **27.3KB**，MP3 解码器初始化失败。`ESP_MP3_DEC: There is no memory for MP3 required`；`ret 10` = `ESP_AUDIO_ERR_MEM_LACK` |
| 设备重启 | 内存进一步耗尽到 **268 字节**。`getaddrinfo()` 返回 `202`（`EAI_MEMORY`）；`HTTP_CLIENT: Allocation failed` |

## 修复：释放常驻 Opus（43KB）

`AudioService` 的 Opus 编/解码器常驻占用 **43KB**，而 Radio 页直连 codec 播 PCM，根本用不到 Opus。进页时 `AudioService::ReleaseAudioModels()` 关掉这一对，离页 `RestoreAudioModels()` 重建。

| 阶段 | 内部 SRAM 可用 |
|------|----------------|
| 进页前（Opus 仍常驻） | **27.3KB** |
| 释放 Opus 后 | **70.4KB** |
| 播放稳定态 | **15–20KB** |

对应日志：

```
released audio models: internal heap 27340 -> 70432
```

总空闲低于 `kMinHeapToStream`（12KB）才拒绝开流并提示 `low memory`。`largest < 20KB` **只告警仍尝试**，以 decoder 真失败（`MEM_LACK`）为准，避免切几下页面后碎片化误杀。

## 关键坑

1. **惰性 open**

   `esp_audio_simple_dec_open()` 只占 148～360 字节就返回成功，真正的解码器要等内置 parser 找到第一帧才分配。「open 成功」**不代表能解码**，这个假象误导了多轮排查。判断标准要看是否真的产出 PCM。

2. **WebRadio 因 PSRAM 不可照搬**

   [cyberwisk/M5Cardputer_WebRadio](https://github.com/cyberwisk/M5Cardputer_WebRadio) 能播，是因为**原版 Cardputer 有 8MB PSRAM**；它用的 ESP32-audioI2S 依赖 PSRAM 做大 ring buffer，那套「HTTPS + 128kbps」在无 PSRAM 的 ADV 上物理上跑不起来。本页选择**纯 HTTP + 64kbps 低码率**才是正确解（TLS 握手还要额外吃调用任务约 8-10KB 栈）。

3. **本板无 AFE**

   本板 `CONFIG_WAKE_WORD_DISABLED=y`，**没有 AFE 实例**，释放唤醒词模型是无效方向。实测释放前后 27392 -> 27392，一字节没省。省内存只能从 Opus 下手。

4. **流任务必须 core 0 / 优先级 3**

   `taskLVGL` 在 **core 1 / 优先级 1**，流任务若放 core1/prio4 会直接饿死 LVGL，触发 `task_wdt: CPU 1: taskLVGL`。循环里用 `vTaskDelay(1)` 而非 `taskYIELD()`，否则 IDLE 任务拿不到 CPU。

5. **整块喂入，按 `raw.consumed` 推进**

   按库标准用法喂解码器：整块喂入、按 `raw.consumed` 推进缓冲。不要自己按 MPEG 帧头切帧，那会和库内置 parser 抢活导致失步。

6. **保护逻辑过激**

   早期 `consec_bad > 64` 会在第一次 512 字节读取后就掐断流，这正是「滴一声就没了」的原因。现放宽为「连续 2000 次失败且从未产出任何 PCM」才放弃，另有 48KB 入流仍无 PCM 的兜底。

7. **440Hz 自检音**

   进页 440Hz / 350ms 自检音很有价值：响了即说明 ES8311 + 功放 + I2S 时钟格式正常，能一秒区分硬件问题和网络/解码问题。

8. **44100 → 24000 重采样**

   解码输出 44100Hz / 2ch / 16bit，codec 是 24000Hz mono，用最近邻重采样 + 相位累积（`resample_pos`）降混。

9. **宁可静音，不送未解码字节**

   格式不是 16bit、或拿不到解码器 info 时一律不输出。把未解码的 MP3 原始字节当 PCM 播，正是滋啦噪音的来源。

## 串口日志对照

正常播放时应依次出现：

```
released audio models: internal heap 27340 -> 70432
mp3 decoder open ok: used 148B
decoded PCM: 44100Hz 2ch 16bit -> codec 24000Hz mono
pcm chunks=200 44100->24000Hz ch=2 vol=85 heap=...
```

异常日志速查：

| 日志 | 含义 / 处理 |
|------|-------------|
| `ESP_MP3_DEC: There is no memory for MP3 required`、ret `10` | 内存不足，Opus 未释放或余量被别处吃掉 |
| `refusing to stream: only ...B free` / `StartStream refuse: heap` | 总空闲低于 12KB，主动不开流（UI 提示 `low memory`） |
| `StartStream: largest ... < 20KB (trying decoder anyway)` | 最大块偏小但仍尝试；随后看 `mp3 decoder open ok` 或 `MEM_LACK` |
| `getaddrinfo() returns 202` / `HTTP_CLIENT: Allocation failed` | 内存已见底，随后大概率重启 |
| `decoder info not ready (N) - holding output` | parser 还没锁到帧，正在静音等待（少量正常） |
| `unsupported bits_per_sample=...; refusing to play` | 格式不符，主动静音而非播噪音 |
| `task_wdt: CPU 1: taskLVGL` | 流任务抢了 core 1，检查 `kTaskCore` / `kTaskPrio` |
| `MP3 unavailable (register=... check=...)` | `CONFIG_AUDIO_DECODER_MP3_SUPPORT` 没开 |

每次退出都会打印 `session end [原因]`，原因取值：

| 原因 | 含义 |
|------|------|
| `user stop / page change` | 用户停止或切页 |
| `http read error` | HTTP 读失败 |
| `stream idle (EOF or timeout)` | 流空闲（EOF 或超时） |
| `decoder rejected/failed` | 解码器拒绝或失败 |
| `no PCM produced` | 从未产出 PCM |

## 配置要点

- 台源用**纯 HTTP + 64kbps** MP3，不要 HTTPS / 高码率。
- `CONFIG_AUDIO_DECODER_MP3_SUPPORT=y` 必须开，其余 codec（G711/AMR 等）关掉省 flash；见 [`main/boards/m5stack-cardputer-adv-car/config.json`](../../main/boards/m5stack-cardputer-adv-car/config.json) 与 [`flash.sh`](../../flash.sh)（kconfig choice 里旧的 `=y` 必须先置 `n`，顺序有意义）。**不要在 `flash.sh` 里把 MP3 decoder 关掉。**
- 控制台必须保持开启（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` + `LOG_DEFAULT_LEVEL_INFO`）：早期 `CONFIG_ESP_CONSOLE_NONE=y` 使 panic handler 无处输出，重启循环里连 backtrace 都看不到。配合 `ESP_SYSTEM_PANIC_PRINT_REBOOT` 与 5 秒延迟，让 USB-CDC 有时间冲出 backtrace。
- 改分区表后需整包烧录：`./flash.sh`，串口另开 `./monitor.sh`（见 [build/flash.md](../build/flash.md)）。

## 进出页二次进入无声

### 现象

- 开机直接进 Radio → 可播放
- **从 Car / Music / Launcher 切回 Radio** → 无声 / 一直 connecting / `low memory`
- 离开后再回 Chat，TTS/语音应仍可用

### 根因（两层）

1. **HTTP 僵尸任务（旧）**：`StopStream` 在 `esp_http_client` 阻塞时过早清空句柄并 `RestoreAudioModels`，MP3 与 Opus 双占用把无 PSRAM 堆吃光。已用 `AbortActiveHttp` + 严格 join + `enter_gen_` 处理。

2. **独占页互切仍 Rebuild Opus（本次主因）**：用户路径是 Radio ↔ Car / Music，不是单纯 Radio→Chat。旧逻辑在 **每次** Radio `OnLeave` 都 `RestoreAudioModels`（~43KB），下一页（Car 不占音频；Music 只抢麦）根本用不到 Opus；再进 Radio 又 `Release`。无 PSRAM 上这轮 Restore→Release **碎片化最大空闲块**，二次进页 MP3 真解码（首帧分配）失败或开流被拒。Music `OnLeave` 再调 `RestoreAudioRouting` 会加重：重建 Opus + 可能留下 `EnableInput(true)` 的 duplex RX。

### 修复（`fix/radio-reenter-audio`）

**流任务 / join（保留）**

- `AbortActiveHttp()` + join 上限 `kHttpTimeoutMs + 2000`；超时不提前 Restore Opus
- `enter_gen_` 作废陈旧 `StartStream`；`StartStream` 先 drain 残留任务

**独占页音频状态机（本次）**

- Radio `ReleaseAudioExclusive`：**只**关掉 `SetExternalPlaybackActive` / 关 input，**不** `RestoreAudioModels`
- Music `ReleaseMicExclusive`：`EnableInput(false)`，**不** `RestoreAudioRouting`
- `Application::RestoreAudioRouting`（Chat `OnEnter`，不含开机 Initialize）：仅当 `ReleaseAudioModels` 曾跑过才 `RestoreAudioModels`，再按设备状态重绑唤醒词/拾音
- 开机 `PageManager::Initialize` 只 `ShowChatUi`，不调 Chat `OnEnter`：`SetupUI` 早于 `AudioService::Initialize`，无条件 Restore 会空指针复位（闪一下黑屏）
- Radio `CaptureAudioExclusive`：强制 `EnableInput(false)` + `ReleaseAudioModels`（幂等）+ 外部播放 hold
- `ReleaseAudioModels` 先清空编解码队列，再关 Opus，避免队列缓冲残留占堆

串口应能看到：`OnLeave` → `audio exclusive OFF (models deferred)` →（Car/Music 无 Opus restore）→ 再进 `OnEnter` → `released audio models`（常为 enc=0 dec=0）→ `mp3 decoder open ok` → `self-test tone` → `pcm chunks=…`。回 Chat 时应有 `RestoreAudioRouting` + `restored audio models`。

### Music → Launcher → Radio 仍无声

Music 路径已修（DestroyPanel + 放麦）。**1–4 / 5 经 Launcher 进 Radio 仍会无声**——见下一节。原先只有经过 Music 再经 Launcher 进 Radio 会失败。Music 没有 FFT / `SetReadSampleRate`，频谱是 512 sample 时域能量。真正占内部 SRAM 的是：

1. **`mic_buf_` 离页不释放**（512×2B），以及 24 根柱 + 网格的 LVGL 面板只 hidden、不 Destroy。Launcher 既不 Release 也不 Restore，Music 残留一直在；再叠 Radio 面板后最大空闲块不够 MP3 真解码。
2. **Tick 在 `esp_timer` 上阻塞 `esp_codec_dev_read`**，主任务 `OnLeave` 可并发 `EnableInput(false)` 关掉 `dev_`（Read 原先不加锁）。未完成的 RX 会把 DMA/软件缓冲留在堆上。
3. **ES8311 `UpdateDeviceState` 只 close 不 delete**：Music 开麦（IN_OUT `esp_codec_dev_new`）再关麦后句柄泄漏；15s 省电门还会反复 new/close（Music 不走 `ReadAudioData`，`last_input_time_` 不刷新）。

**修复**

- Music `OnLeave`：等 in-flight capture → `EnableInput(false)` → 释放 `mic_buf_` → **DestroyPanel**；打 heap / largest
- `NotifyInputActivity()` 防止省电门中途关 RX
- ES8311：`Read`/`Write` 与 `EnableInput` 同锁；close 后 `esp_codec_dev_delete`
- Radio：`OnEnter` / Capture 打 free+largest；`open`/`process` 失败打 `MEM_LACK`（`ESP_AUDIO_ERR_MEM_LACK=-2`）并立刻放弃，不再空转 connecting

串口对照：`OnLeave Music after mic release` → `after panel destroy` → `codec device closed+deleted` → Radio `OnEnter ... heap= ... largest=` → `mp3 decoder open ok` → `pcm chunks=`。


### 1–4 / 5 → Launcher → Radio 无声

启动器数字：1 Car / 2 SpiderBot / 3 IceBox / 4 Clock / 5 Rain / 6 Music / 7 Radio。

**能播：** 6 Music → 导航(Launcher) → 7 Radio（Music Leave 已 DestroyPanel）
**不能播（修前）：** 1–4 任意页 → 导航 → 7 Radio。5 Rain 同构（Matrix canvas），一并修。

#### 根因

PageManager `ShowPage` 对独占页 Leave **只 hidden、不 Destroy**（为防 IceBox→Car 同一次切页里 Destroy+重建卡死 LVGL）。经 Launcher 进 Radio 时，旧 panel 仍占内部 SRAM，再叠 Launcher（复用）+ Radio（24 柱），**最大空闲块**不够 MP3 真解码（`esp_audio_simple_dec_open` 仍可能成功，第一帧才 `MEM_LACK`）。

MQTT 客户端开机常驻，离页不断开。IceBox IR worker（4KB 栈 + `IRMitsubishiAC`）第一次进页后常驻，`Cancel` 只停发送。Clock/Matrix 的 RGB565 **canvas 是成员 BSS**（Clock 182×54×2 ≈ 19.6KB，Matrix 120×68×2 ≈ 16.3KB），开机就占着，Destroy panel **腾不出**这块——只能释放 LVGL 对象。

| 页 | 离页（修前） | 堆上大约（LVGL，进页才有） | 常驻（与是否进页无关） |
|----|--------------|------------------------------|------------------------|
| 1 Car | hidden | ~30 个对象（仪表盘+车身+轮辐），约 **8–15KB** | MQTT 客户端（全局） |
| 2 Spider | hidden | ~24 个对象，约 **8–12KB** | 同上 MQTT |
| 3 IceBox | hidden + IR Cancel | ~30 个对象（6 键×3 + 左右栏），约 **8–15KB** | 首次进页后 IR task 4KB 栈 |
| 4 Clock | hidden | panel+canvas 对象约 **1–3KB** | canvas BSS **19.6KB** |
| 5 Rain | hidden | 同上约 **1–3KB** | canvas BSS **16.3KB** |
| 6 Music | DestroyPanel | 24 柱+网格，约 **10–15KB**（已释放） | — |
| Launcher | hidden 复用（进 Radio 前 Destroy） | MYJ 点阵+7 按钮，较重；与 Radio 24 柱叠在一起会压垮 largest | — |

Car/IceBox 的 hidden 仪表盘才是 1–4→Radio 的主因；Clock/Matrix 对象虽小，hidden panel 仍碎片化最大块，故同样 Destroy。

#### 修复

- **Clock / Matrix**：`OnLeave` 一律 `DestroyPanel`（重建便宜；canvas BSS 仍在）
- **Car / Spider / IceBox**：`PageManager` 在 Leave 之后调用 `ReleaseResidentUi`（`lv_obj_del`），**仅当下一页不是 Car/Spider/IceBox**。仪表盘互切仍复用 panel，避免 IceBox→Car 卡死回归
- Launcher 在进 **非 Radio** 页时仍 hidden 复用；**进 Radio 前 sweep Destroy**（含 Launcher）
- Radio 离页 **DestroyPanel**（不再 hidden 常驻 24 柱）
- 日志：各页 Leave 后 `heap`/`largest`；Radio `OnEnter` / Capture / `MEM_LACK`；`StartStream` 若 `largest < 20KB` **只告警仍尝试**，真失败看 decoder `MEM_LACK`

串口对照：`release resident UI` → `ReleaseResidentUi dashboard heap A->B largest C->D`（或 Clock/Matrix/IceBox `DestroyPanel`）→ Radio `OnEnter ... largest=` → `heap after capture` → `mp3 decoder open ok` → `pcm chunks=`。

#### 测试矩阵

- [ ] 1 Car → Launcher → 7 Radio
- [ ] 2 Spider → Launcher → 7 Radio
- [ ] 3 IceBox → Launcher → 7 Radio
- [ ] 4 Clock → Launcher → 7 Radio
- [ ] 5 Rain → Launcher → 7 Radio
- [ ] 6 Music → Launcher → 7 Radio（回归仍能播）
- [ ] 开机直进 7 Radio
- [ ] Radio → Chat 语音仍可用
- [ ] IceBox → Launcher → Car（切页不卡死）
- [ ] 来回切 1–7 与 Launcher **至少 10 次** 后再进 Radio，仍能播（不应再 `stream err / low memory`）

### 切几下就 stream err / low memory

#### 现象

开机 Radio 能播；在 1–7 与 Launcher 之间切几次再进 Radio，UI 显示 **stream err / low memory**，无法继续播放。

#### 根因（门槛误杀 + 面板常驻碎片化，不是 decoder/HTTP 泄漏）

1. **Radio / Launcher 离页只 hidden、不 Destroy**。Radio 24 柱 + Launcher MYJ 点阵各约 8–15KB LVGL 对象，第一次进过之后一直占着内部 SRAM。Car/Clock/Music 在旁边反复 Build/Destroy，把 **最大空闲块** 碎片化到 20KB 以下。
2. **`kMinLargestToStream = 20KB` 预检过严**：切几次后 `largest` 掉到 12–18KB 就直接 `low memory`，其实 Helix 仍可能解码成功。decoder / HTTP / stream task / `vis_buf`（任务栈上）进出页是成对的，没有叠开第二路流。
3. `BuildPanel` 在 `panel_ != nullptr` 时直接 return，**不会每次进 Radio 新建 24 柱**；问题是旧面板从不释放。

`heap_caps_malloc_extmem_enable` 与本板无关（无 PSRAM）。

#### 修复

- Radio `OnLeave`：`StopStream` 后 **DestroyPanel**（decoder/http/task 仍由 StopStream 成对关闭）
- 进 Radio 前 `PageManager::ReleaseOtherExclusiveUi`：**Destroy 所有其他独占页**（含 Launcher）；只留 Chat UI + 当前 Radio
- 进 Radio 时先 `CaptureAudioExclusive`（释放 Opus）再 BuildPanel，让后续 MP3 拿到更大连续块
- `largest < 20KB` 改为告警，**仍尝试开流**；只有总空闲 < 12KB 或 decoder `MEM_LACK` / `xTaskCreate` 失败才报错

串口对照：`sweep destroy page 5`（Launcher）→ `DestroyPanel launcher heap/largest` → Radio `OnEnter` → `audio exclusive ON` → `mp3 decoder open ok` → `pcm chunks=`。切页离开应有 `DestroyPanel radio`。

## 遗留项

- 稳定态仅剩 **15-20KB** 内部 SRAM，余量不宽裕。
- 「进 Radio 到离页再聊天语音」依赖 Chat `RestoreAudioRouting`→`RestoreAudioModels`（独占页互切不再离页重建）；二次进 Radio 见上文「进出页二次进入无声」。
- 目前只验证 Music 台 `http://lhttp.qtfm.cn/live/332/64k.mp3`；News 台未逐项确认。
- 24kHz 最近邻重采样高频有混叠，音质一般但可用。
