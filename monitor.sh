#!/bin/bash
# monitor.sh — 串口监视（仅监视，不编译/烧录）
#
# 需先 source IDF 或使用默认 IDF_PATH（同 flash.sh）。
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

PORT="${PORT:-}"
BAUD="${BAUD:-115200}"

if [ -z "$PORT" ]; then
  for p in /dev/cu.usbmodem* /dev/cu.wchusb* /dev/cu.usbserial* /dev/cu.SLAB*; do
    [ -e "$p" ] && PORT="$p" && break
  done
fi

echo "串口监视 115200（Ctrl+] 退出）"
[ -n "$PORT" ] && echo "Port: $PORT"

if [ -n "$PORT" ]; then
  idf.py -p "$PORT" -b "$BAUD" monitor
else
  idf.py -b "$BAUD" monitor
fi
