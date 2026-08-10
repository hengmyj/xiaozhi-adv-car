# M5Stack Cardputer Adv Car（xiaozhi-ADV-car Fork）

本目录为 **xiaozhi-ADV-car** 专用板型，OTA 板名：`m5stack-cardputer-adv-car`。

## 与官方 ADV 的区别

- 基于 `m5stack-cardputer-adv` 拷贝，独立 OTA 名称，可与官方固件并存升级
- **Fn+1** 聊天页、**Fn+2** 麦轮小车、**Fn+3** 蜘蛛（EMQX `car/cmd` + `foc/cmd`，独立于小智云 MQTT）
- 车控页：**;** 前进、**.** 停止、**,** 左转、**/``** 右转（无需按 Fn；Fn 仅用于 Fn+1/2/3 切页）
- Broker：`broker-cn.emqx.io:1883`，订阅 `car/state` 显示运行状态
- **Fn** 为键盘左下独立键（row2 col0，`KEY_MOD_FN`），**不是** Opt 键；需按住 Fn 再按数字
- 切页使用 `lv_obj HIDDEN` 隐藏聊天 UI，不销毁 LVGL 树

## 硬件

与 M5Stack Cardputer Adv 相同：ESP32-S3、8MB Flash、无 PSRAM、ST7789 240×135、TCA8418 键盘。

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

`flash.sh` 会自动 `set-target esp32s3` 并写入本板 `sdkconfig`：`CONFIG_BOARD_TYPE_M5STACK_CARDPUTER_ADV_CAR=y`、`CONFIG_SPIRAM=n`、`CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`。

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
