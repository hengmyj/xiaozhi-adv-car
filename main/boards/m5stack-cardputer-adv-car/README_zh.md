# M5Stack Cardputer Adv Car（xiaozhi-ADV-car Fork）

本目录为 **xiaozhi-ADV-car** 专用板型，OTA 板名：`m5stack-cardputer-adv-car`。

## 目录结构

```
m5stack-cardputer-adv-car/
  m5stack_cardputer_adv_car.cc   # 板级入口
  config.h / config.json         # 引脚与 sdkconfig 追加
  tca8418_keyboard.*             # 键盘
  wifi_config_ui.*               # WiFi 配置 UI
  common/                        # 页面框架、MQTT、车控基类、LCD 封装
    page.h / page_id.h / page_manager.*
    vehicle_control_page.* / emqx_* / car_state.h
    cardputer_adv_lcd_display.*
  pages/
    launcher/                    # Fn+5 启动器（顶栏中央时间）
    car/                         # 麦轮小车
    spider/                      # SpiderBot
    icebox/                      # 三菱空调 MJ（mj_ac_page）
    clock/                       # 像素时钟
    matrix/                      # Rain 字幕下落（淡出拖尾）
    cursor/                      # Music 拾音柱状图（文件名仍为 cursor）
    radio/                       # 央广 CNR1 WiFi 直播
  ir/                            # 板载红外（GPIO44），与 pages/icebox 配合
```

## 与官方 ADV 的区别

- 基于 `m5stack-cardputer-adv` 拷贝，独立 OTA 名称，可与官方固件并存升级
- **切页（按住 Fn）**

  | 组合 | 页面 |
  |------|------|
  | **Fn+1** | 聊天（小智） |
  | **Fn+2** | 麦轮小车 |
  | **Fn+3** | SpiderBot |
  | **Fn+4** | IceBox（三菱空调 MJ） |
  | **Fn+5** | 启动器（Sparks 风格菜单，logo **点阵镂空 M + YJ**） |

- 启动器（ASCII 标签，避免中文乱码）按数字进入；顶栏中央显示系统时间（HH:MM:SS，SNTP）：

  | 键 | 标签 | 页面 |
  |----|------|------|
  | **1** | Car | 麦轮小车 |
  | **2** | SpiderBot | 蜘蛛 |
  | **3** | IceBox | 三菱空调 |
  | **4** | Clock | 像素时钟（系统时间 / SNTP） |
  | **5** | Rain | Matrix 字幕下落（淡出拖尾） |
  | **6** | Music | 拾音器柱状图（ES8311 mic；原 Cursor） |
  | **7** | Radio | 央广 CNR1 中国之声（HLS/TS AAC → ES8311） |
- 车控页：**;** 前进、**.** 停止、**,** 左转、**/** 右转（无需 Fn）
- IceBox 空调页：针对 **三菱电机 ZFJ 系列 MSZ-ZFJ12VA（KFR-36GW/BpU / ZFJ12）**
  - **P** 电源、**M** 模式、**F** 风速、**;**/**.** 温度 — 每次按键立即改 UI 并发红外（无需 Enter）
  - **S** 强制重发；协议默认 **`MITSUBISHI_AC`**（GPIO 44），见 `ir/mitsubishi_ir.h` 的 `MJ_AC_IR_PROTOCOL`
- Broker：`broker-cn.emqx.io:1883`，订阅 `car/state` 显示运行状态
- **Fn** 为键盘左下独立键（row2 col0，`KEY_MOD_FN`），**不是** Opt 键
- 切页使用 `lv_obj HIDDEN` 隐藏聊天 UI，不销毁 LVGL 树

## MQTT 车控

本板通过独立 EMQX 通道控制麦轮 / 蜘蛛（与小智云 MQTT 无关）：

| Topic | 作用 |
|-------|------|
| `car/cmd` | `{"run":0\|1,"speed":0-100}`（无 `dir`） |
| `foc/cmd` | 转向 `{"dir":1\|-1,"speed":…}`（`1` 左 / `-1` 右） |
| `car/state` | 反馈 `{"run","speed","pwm"}`（本板订阅） |

完整协议、架构图与网页仪表盘映射见项目文档：**[docs/car-mqtt-control.md](../../../docs/car-mqtt-control.md)**。

## 硬件

与 M5Stack Cardputer Adv 相同：ESP32-S3、8MB Flash、无 PSRAM、ST7789 240×135、TCA8418 键盘、**板载红外发射 GPIO 44**（与 Sparks 固件一致；红外光人眼不可见，看左上角粉色 TX 点）。

空调红外代码已放在本板目录：

```
main/boards/m5stack-cardputer-adv-car/ir/
  mitsubishi_ir.cc / .h     # ZFJ12 封装，默认 MITSUBISHI_AC，发送两遍
  arduino_shim/             # Arduino API 薄封装（仅本板 IR 用）
  IRremoteESP8266/          # 精简依赖的 IRremoteESP8266 源码树（v2.8.6）
  ir_utils_stub.cpp
```

若 `ir/IRremoteESP8266` 目录为空，可重新拉取：

```bash
git clone --depth 1 --branch v2.8.6 \
  https://github.com/crankyoldgit/IRremoteESP8266.git \
  main/boards/m5stack-cardputer-adv-car/ir/IRremoteESP8266
```

## 构建

**ESP-IDF 版本**：与上游 xiaozhi-esp32 2.2.x 相同，需 **5.4 或以上**（本机已验证 **v5.5.3**）。

```bash
# 推荐 IDF（勿用 ~/Documents/esp/esp-idf 的旧 v5.0-dev）
export IDF_PATH=~/.espressif/v5.5.3/esp-idf
. "$IDF_PATH/export.sh"
# 若提示 Python 虚拟环境不存在：
# $IDF_PATH/tools/idf_tools.py install-python-env

cd ~/Documents/esp/xiaozhi-ADV-car
```

### 一键脚本（推荐）

在项目根目录：

```bash
./flash.sh              # 编译 + 烧录（不自动开监视器）
./monitor.sh            # 仅串口监视
PORT=/dev/cu.usbmodem101 ./flash.sh
```

`flash.sh` 会自动 `set-target esp32s3` 并写入本板 `sdkconfig`：`CONFIG_BOARD_TYPE_M5STACK_CARDPUTER_ADV_CAR=y`、`CONFIG_SPIRAM=n`、`CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`、分区表 `partitions/v2/8m_adv_car.csv`（OTA 略放大以容纳 AAC 收音机；assets 1.5MB）、以及精简 `esp_audio_codec`（仅 AAC/TS）。

**注意**：分区偏移相对旧 `8m.csv` 有变，首次刷机请整包 flash（含 partition table / assets），不能仅 OTA 升级。

### 官方 release 脚本

```bash
python3 scripts/release.py m5stack-cardputer-adv-car --name m5stack-cardputer-adv-car
```

### 手动 idf.py

```bash
idf.py set-target esp32s3
# 在 sdkconfig 末尾追加 config.json 中的 sdkconfig_append，或 menuconfig 选择本板型
idf.py -DBOARD_NAME=m5stack-cardputer-adv-car -DBOARD_TYPE=m5stack-cardputer-adv-car build
idf.py -p PORT flash
idf.py -p PORT monitor
```

## 上游

Fork 自 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)，车控代码仅在本 board 目录。
