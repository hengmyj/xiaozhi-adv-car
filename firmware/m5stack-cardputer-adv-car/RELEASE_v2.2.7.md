# v2.2.7 — M5Stack Cardputer ADV Car

小智 AI 语音助手 + 多页启动器，专用于 **M5Stack Cardputer ADV**（ESP32-S3、8MB Flash、无 PSRAM）。

- 板型 / OTA：`m5stack-cardputer-adv-car`
- 分区表：`partitions/v2/8m_adv_car.csv`（与旧版 8m 不兼容，**须整包刷**）
- 仓库：[hengmyj/xiaozhi-adv-car](https://github.com/hengmyj/xiaozhi-adv-car)

---

## 下载

| 文件 | 说明 |
|------|------|
| `xiaozhi-adv-car-v2.2.7.bin` | 整包合并镜像（约 7.8MB），**推荐**，从地址 `0x0` 烧录 |
| `xiaozhi-adv-car-v2.2.7.zip` | 同上，zip 内为 `merged-binary.bin` |

---

## 烧录

**芯片** ESP32-S3 · **Flash** 8MB · DIO · 80MHz

### M5Burner（推荐）

1. 打开 [M5Burner](https://burner.m5stack.com/)，USB 连接 Cardputer ADV
2. 拖入 `xiaozhi-adv-car-v2.2.7.bin`
3. 烧录地址 **0x0**，Flash **8MB**，开始烧录

### esptool

```bash
pip install esptool
python -m esptool --chip esp32s3 -b 460800 -p PORT \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x0 xiaozhi-adv-car-v2.2.7.bin
```

`PORT`：macOS `/dev/cu.usbmodem*` · Linux `/dev/ttyACM0` · Windows `COM3`  
烧录慢可改 `-b 115200`。

> **注意**：首次刷机或分区表变更后必须整包刷，不能只 OTA。刷完无串口输出时拔插 USB 或按复位。

---

## 首次使用

1. **开机** → 默认进入 **小智 Chat** 页
2. **配网**：开机短按 **BOOT**，或 Chat 页按 **W** 扫 WiFi
   - `;` / `↑` 、`.` / `↓` 移动列表，`Enter` 选中，`ESC` 取消
   - 键盘输入密码后 `Enter` 确认
   - 配网 overlay 不整屏清空，可随时 `ESC` 退出
3. 连上 WiFi 后语音对话（需配置小智服务端，见上游 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 文档）

---

## 全局按键

| 按键 | 作用 |
|------|------|
| **Fn+1** | 回到小智 Chat（任意页可用） |
| **Fn+2** | 打开启动器 Launcher |

Fn 是键盘 **左下角独立键**，不是 Opt。

```
Fn+1 小智主页
Fn+2 启动器 ──1── Car 麦轮
              ├─2── Spider 蜘蛛
              ├─3── IceBox 空调
              ├─4── Clock 时钟
              ├─5── Rain 字幕
              ├─6── Music 拾音
              ├─7── Radio 电台
              ├─8── Snake 贪吃蛇
              └─9── Dino 小恐龙
```

---

## 小智 Chat 页

| 操作 | 作用 |
|------|------|
| BOOT 短按 / Enter | 切换聆听 / 说话 |
| `;` / `↑` | 音量 + |
| `.` / `↓` | 音量 - |
| `/` / `→` | 背光 +（最低 30%） |
| `,` / `←` | 背光 - |
| **W** | 打开 WiFi 扫网 + 输密码 |
| **S** | 已存 WiFi 列表 |

---

## 启动器 Launcher

3×3 菜单，顶栏 M+YJ logo、系统时间、WiFi 图标。按 **1–9** 进入对应页面（标签为 ASCII，避免缺字乱码）。

---

## 分页面操作

### 1 · Car 麦轮小车（MQTT）

| 操作 | 作用 |
|------|------|
| `;` / `↑` | 前进 |
| `.` / `↓` | 停止 |
| `,` / `←` | 左转 |
| `/` / `→` | 右转 |

### 2 · Spider 蜘蛛

与 Car 相同按键，MQTT 遥控蜘蛛机器人。

### 3 · IceBox 三菱空调（红外）

| 操作 | 作用 |
|------|------|
| **P** | 电源 |
| **M** | 模式循环 |
| **F** | 风速循环 |
| `;` / `↑` | 温度 +1℃ |
| `.` / `↓` | 温度 -1℃ |
| **S** / Enter / 空格 | 强制重发 |

红外按键 **即时发码，无需 Enter**。左上角粉色点表示正在发射。

### 4 · Clock 时钟

像素风格 `HH:MM:SS` + 日期，无页内按键。

### 5 · Rain 字幕雨

黑客帝国风下落字幕，无页内按键。

### 6 · Music 拾音柱

对着麦克风说话或放音乐，24 根柱状图响应，无页内按键。

### 7 · Radio 网络电台

HTTP MP3 低码率流（无 PSRAM 优化）。

| 操作 | 作用 |
|------|------|
| **1** / **N** / `,` | 中国之声 |
| **2** / **M** / `/` | 北京音乐广播（默认） |
| **P** / 空格 | 暂停 / 继续 |
| `;` / `↑` | 音量 +5 |
| `.` / `↓` | 音量 -5 |

### 8 · Snake 贪吃蛇

| 操作 | 作用 |
|------|------|
| Enter / 空格 | 开局 / 重开 / 暂停后继续 |
| **P** | 暂停 |
| `;` W `↑` | 上 |
| `.` S `↓` | 下 |
| `,` A `←` | 左 |
| `/` D `→` | 右 |

### 9 · Dino 小恐龙

| 操作 | 作用 |
|------|------|
| 空格 / Enter / W / `↑` / J | 开跑 / 重开 / 跳跃 |
| **P** | 暂停 |

---

## 本版亮点

- 1 主页 + 启动器 + 9 分页面
- Radio：MP3 + AAC/TS 精简解码，HTTP 低码率
- 键盘机上 WiFi 配网（overlay，不依赖手机网页）
- 红外：车控 / 空调 / 蜘蛛；MQTT 麦轮小车
- Snake / Dino 小游戏

页内车控 / IR **无需 Fn**，按键即时响应。

更完整文档：[pages.md](https://github.com/hengmyj/xiaozhi-adv-car/blob/master/main/boards/m5stack-cardputer-adv-car/docs/pages.md) · [架构说明](https://github.com/hengmyj/xiaozhi-adv-car/blob/master/main/boards/m5stack-cardputer-adv-car/docs/architecture.md)

---

## 问题反馈

[GitHub Issues](https://github.com/hengmyj/xiaozhi-adv-car/issues)
