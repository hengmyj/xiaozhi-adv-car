#!/bin/bash
# flash.sh — 编译 + 烧录 M5Stack Cardputer ADV Car（ESP-IDF，不自动开监视器）
#
# ESP-IDF：项目要求 >= 5.4（见 README_zh.md）。推荐本机：
#   export IDF_PATH=~/.espressif/v5.5.3/esp-idf
#   首次缺 Python 环境：$IDF_PATH/tools/idf_tools.py install-python-env
# 勿将 IDF_PATH 指到 ~/Documents/esp/esp-idf（v5.0-dev，idf5.0_py3.12_env 不存在会报错）。
#
# 用法：
#   ./flash.sh
#   PORT=/dev/cu.usbmodem101 ./flash.sh
#   BAUD=115200 ./flash.sh
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

IDF_PATH="${IDF_PATH:-$HOME/.espressif/v5.5.3/esp-idf}"
if [ ! -f "$IDF_PATH/export.sh" ]; then
  echo "[ERROR] 找不到 export.sh，请设置 IDF_PATH（当前: ${IDF_PATH}）"
  exit 1
fi
# shellcheck disable=SC1090
. "$IDF_PATH/export.sh"

BOARD_TYPE="m5stack-cardputer-adv-car"
BOARD_NAME="m5stack-cardputer-adv-car"
PORT="${PORT:-}"
BAUD="${BAUD:-460800}"

detect_port() {
  for p in /dev/cu.usbmodem* /dev/cu.wchusb* /dev/cu.usbserial* /dev/cu.SLAB*; do
    [ -e "$p" ] && echo "$p" && return 0
  done
  return 1
}

ensure_board_sdkconfig() {
  if ! grep -q "CONFIG_BOARD_TYPE_M5STACK_CARDPUTER_ADV_CAR=y" sdkconfig 2>/dev/null; then
    echo "[INFO] 首次配置板型：esp32s3 / 8MB Flash / 无 PSRAM"
    idf.py set-target esp32s3
    {
      echo ""
      echo "# Append by flash.sh"
      echo "CONFIG_BOARD_TYPE_M5STACK_CARDPUTER_ADV_CAR=y"
      echo "CONFIG_SPIRAM=n"
      echo "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y"
      echo 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/v2/8m_adv_car.csv"'
    } >> sdkconfig
  fi
  # Keep partition + slim audio codecs in sync (radio: MP3 + AAC/TS).
  if ! grep -q 'partitions/v2/8m_adv_car.csv' sdkconfig 2>/dev/null; then
    echo 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/v2/8m_adv_car.csv"' >> sdkconfig
  fi
  # 顺序有意义：choice 里旧的 =y 必须先置 n，否则 kconfig 会用靠后的那条。
  for kv in \
    'CONFIG_ESP_CONSOLE_NONE=n' \
    'CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y' \
    'CONFIG_LOG_DEFAULT_LEVEL_NONE=n' \
    'CONFIG_LOG_DEFAULT_LEVEL_INFO=y' \
    'CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y' \
    'CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS=5' \
    'CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=y' \
    'CONFIG_AUDIO_DECODER_MP3_SUPPORT=y' \
    'CONFIG_AUDIO_DECODER_G711_SUPPORT=n' \
    'CONFIG_AUDIO_DECODER_AMRNB_SUPPORT=n' \
    'CONFIG_AUDIO_DECODER_AMRWB_SUPPORT=n' \
    'CONFIG_AUDIO_DECODER_FLAC_SUPPORT=n' \
    'CONFIG_AUDIO_DECODER_VORBIS_SUPPORT=n' \
    'CONFIG_AUDIO_DECODER_ADPCM_SUPPORT=n' \
    'CONFIG_AUDIO_DECODER_ALAC_SUPPORT=n' \
    'CONFIG_AUDIO_DECODER_PCM_SUPPORT=n' \
    'CONFIG_AUDIO_DECODER_SBC_SUPPORT=n' \
    'CONFIG_AUDIO_DECODER_LC3_SUPPORT=n' \
    'CONFIG_AUDIO_DECODER_AAC_SUPPORT=y' \
    'CONFIG_AUDIO_SIMPLE_DEC_WAV_SUPPORT=n' \
    'CONFIG_AUDIO_SIMPLE_DEC_M4A_SUPPORT=n' \
    'CONFIG_AUDIO_SIMPLE_DEC_TS_SUPPORT=y' \
    'CONFIG_ESP_TLS_INSECURE=y' \
    'CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y'
  do
    key="${kv%%=*}"
    if grep -q "^# ${key} is not set" sdkconfig 2>/dev/null; then
      sed -i.bak "s|^# ${key} is not set|${kv}|" sdkconfig && rm -f sdkconfig.bak
    elif grep -q "^${key}=" sdkconfig 2>/dev/null; then
      sed -i.bak "s|^${key}=.*|${kv}|" sdkconfig && rm -f sdkconfig.bak
    else
      echo "$kv" >> sdkconfig
    fi
  done
}

if [ -z "$PORT" ]; then
  PORT="$(detect_port || true)"
fi

echo "╔══════════════════════════════════════╗"
echo "║  编译 + 烧录 Cardputer ADV Car        ║"
echo "║  ESP-IDF / $BOARD_NAME"
echo "╚══════════════════════════════════════╝"
echo "IDF: $(idf.py --version 2>/dev/null | head -1)"
[ -n "$PORT" ] && echo "串口: $PORT" || echo "[WARN] 未检测到 USB 串口；可 PORT=/dev/cu.usbmodemXXX ./flash.sh"
echo "upload baud: ${BAUD}（慢速: BAUD=115200 ./flash.sh）"
echo "烧录后串口: ./monitor.sh（不自动开监视；卡住时 ./monitor.sh kill）"
echo ""

# 释放可能占用串口的监视器，避免 flash 失败或留下 reconnect 死循环
if pgrep -f 'idf_monitor\.py|idf\.py .*monitor' >/dev/null 2>&1; then
  echo "[INFO] 发现正在运行的串口监视器，先结束以免占口..."
  "$SCRIPT_DIR/monitor.sh" kill || true
fi

ensure_board_sdkconfig

SECONDS=0
idf.py -DBOARD_NAME="$BOARD_NAME" -DBOARD_TYPE="$BOARD_TYPE" build
if [ -n "$PORT" ]; then
  idf.py -p "$PORT" -b "$BAUD" flash
else
  idf.py -b "$BAUD" flash
fi
echo "[OK] 烧录完成 (${SECONDS}s)"
