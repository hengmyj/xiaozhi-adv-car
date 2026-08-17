# v2.2.7 — M5Stack Cardputer ADV Car

小智 AI 语音助手 + 多页启动器，专用于 **M5Stack Cardputer ADV**（ESP32-S3、8MB Flash、无 PSRAM）。

- 板型 / OTA：`m5stack-cardputer-adv-car`
- 分区表：`partitions/v2/8m_adv_car.csv`（与旧版 8m 不兼容，须整包刷）
- 仓库：[hengmyj/xiaozhi-adv-car](https://github.com/hengmyj/xiaozhi-adv-car)

## 下载

| 文件 | 说明 |
|------|------|
| `xiaozhi-adv-car-v2.2.7.bin` | 整包合并镜像（约 7.8MB），推荐 |
| `xiaozhi-adv-car-v2.2.7.zip` | 同上，zip 内为 `merged-binary.bin` |

## 烧录

**芯片**：ESP32-S3 · **Flash** 8MB · DIO · 80MHz

### 方式一：M5Burner（最简单）

1. 打开 [M5Burner](https://burner.m5stack.com/)，USB 连接 Cardputer ADV
2. 选择「自定义」或拖入 `xiaozhi-adv-car-v2.2.7.bin`
3. 烧录地址 **0x0**，Flash 大小 **8MB**，开始烧录

### 方式二：esptool

```bash
# 安装：pip install esptool
python -m esptool --chip esp32s3 -b 460800 -p PORT \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x0 xiaozhi-adv-car-v2.2.7.bin
```

`PORT` 替换为串口，例如：
- macOS：`/dev/cu.usbmodem101`
- Linux：`/dev/ttyACM0`
- Windows：`COM3`

烧录慢可改 `-b 115200`。

### 注意

- **首次刷机或分区表变更后必须整包刷**，不能只 OTA 升级
- 刷完后若串口无输出，拔插 USB 或按复位

## 首次使用

1. **开机** → 默认进入 **小智 Chat** 页
2. **配网**：开机过程中短按 **BOOT**，或 Chat 页按 **W** 打开 WiFi 列表
   - `;` / `↑` 、`.` / `↓` 移动，`Enter` 选中，`ESC` 取消
   - 选网络后键盘输入密码，Enter 确认
3. 连上 WiFi 后即可语音对话（需配置小智服务端，见上游 xiaozhi 文档）

## 按键速查

全局（任意页）：

| 按键 | 作用 |
|------|------|
| **Fn+1** | 回到小智 Chat |
| **Fn+2** | 打开启动器 Launcher |

启动器内 **1–9** 进入分页面：

| 键 | 页面 | 简介 |
|----|------|------|
| 1 | Car | 麦轮小车遥控 + MQTT |
| 2 | Spider | 蜘蛛机器人 IR 控制 |
| 3 | IceBox | 空调 IR |
| 4 | Clock | 时钟 |
| 5 | Rain | 字幕雨 |
| 6 | Music | 拾音柱状图 |
| 7 | Radio | 网络电台（HTTP MP3） |
| 8 | Snake | 贪吃蛇 |
| 9 | Dino | 小恐龙 |

Chat 页常用键：`Enter` / BOOT 切换聆听；`;` `.` 音量；`,` `/` 背光。

页内车控 / IR **无需 Fn**，按键即时发码。

详细说明：[pages.md](https://github.com/hengmyj/xiaozhi-adv-car/blob/master/main/boards/m5stack-cardputer-adv-car/docs/pages.md)

## 本版更新

- 多页启动器：Car / Spider / IceBox / Clock / Rain / Music / Radio / Snake / Dino
- Radio：MP3 + AAC/TS 精简解码，无 PSRAM 低码率 HTTP 流
- 键盘机上 WiFi 配网（overlay，不依赖手机网页）
- 红外车控 / 空调 / 蜘蛛机器人

## 问题反馈

[GitHub Issues](https://github.com/hengmyj/xiaozhi-adv-car/issues)
