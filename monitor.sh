#!/bin/bash
# monitor.sh — 串口监视（仅监视，不编译/烧录）
#
# 需先 source IDF 或使用默认 IDF_PATH（同 flash.sh）。
#
# 退出方式：
#   Ctrl+]              idf_monitor 官方退出键（推荐）
#   Ctrl+T 然后 Ctrl+X  菜单退出
#   Ctrl+C              本脚本包装后会结束监视器（原生 idf_monitor 不会退出，
#                       而是把 Ctrl+C 发给芯片；macOS 的 Cmd+C 是复制，无效）
#   ./monitor.sh kill   强制结束卡住的监视器进程
#
# 若出现 "Device not configured" / "Waiting for the device to reconnect"：
#   USB CDC 已断开（复位/崩溃/拔线常见）→ 拔插 USB，确认 ls /dev/cu.usbmodem*
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

IDF_PATH="${IDF_PATH:-$HOME/.espressif/v5.5.3/esp-idf}"
PORT="${PORT:-}"
BAUD="${BAUD:-115200}"

kill_stuck_monitors() {
  echo "[monitor] 结束卡住的 idf_monitor / idf.py monitor ..."
  # 仅针对本机常见监视器；排除本脚本自身 PID
  pkill -f 'esp-idf/.*/tools/idf_monitor.py' 2>/dev/null || true
  pkill -f 'idf_monitor.py -p /dev/cu\.' 2>/dev/null || true
  pkill -f 'idf\.py .*-p /dev/cu\..* monitor' 2>/dev/null || true
  # 其它 monitor.sh（不要杀当前 kill 子命令）
  for pid in $(pgrep -f '[.]/monitor\.sh' 2>/dev/null || true); do
    [ "$pid" = "$$" ] && continue
    [ "$pid" = "$PPID" ] && continue
    kill -TERM "$pid" 2>/dev/null || true
  done
  sleep 0.2
  if pgrep -f 'idf_monitor\.py' >/dev/null 2>&1; then
    echo "[monitor] 仍有残留，改用 SIGKILL ..."
    pkill -9 -f 'idf_monitor\.py' 2>/dev/null || true
    pkill -9 -f 'idf\.py .*monitor' 2>/dev/null || true
  fi
  echo "[monitor] 完成。用 ls /dev/cu.usbmodem* 确认设备是否还在。"
}

if [ "${1:-}" = "kill" ] || [ "${1:-}" = "--kill" ]; then
  kill_stuck_monitors
  exit 0
fi

if [ ! -f "$IDF_PATH/export.sh" ]; then
  echo "[ERROR] 找不到 export.sh，请设置 IDF_PATH（当前: ${IDF_PATH}）"
  exit 1
fi
# shellcheck disable=SC1090
. "$IDF_PATH/export.sh"

if [ -z "$PORT" ]; then
  for p in /dev/cu.usbmodem* /dev/cu.wchusb* /dev/cu.usbserial* /dev/cu.SLAB*; do
    [ -e "$p" ] && PORT="$p" && break
  done
fi

if [ -z "$PORT" ] || [ ! -e "$PORT" ]; then
  echo "[ERROR] 未找到 USB 串口（/dev/cu.usbmodem* 等）。"
  echo "  设备可能已掉线（复位/Radio 崩溃后 USB-Serial/JTAG 常见）。"
  echo "  请拔插 USB 后重试；或指定 PORT=/dev/cu.usbmodemXXX ./monitor.sh"
  echo "  若监视器卡住：./monitor.sh kill"
  exit 1
fi

echo "串口监视 ${BAUD}"
echo "Port: $PORT"
echo "退出: Ctrl+]  |  或 Ctrl+C（本脚本会结束进程）  |  卡住时: ./monitor.sh kill"
echo "提示: macOS 的 Cmd+C 是复制，不会中断进程；原生 idf_monitor 会吞掉 Ctrl+C 并转发给芯片。"
echo ""

# 用独立 session 跑 idf.py：终端 Ctrl+C 打到本包装进程，再杀整棵监视器树。
# （前台直接跑 idf_monitor 时，Ctrl+C 被其吞掉并打印 “please use Ctrl+]”，无法退出。）
python3 - "$PORT" "$BAUD" <<'PY'
import os
import signal
import subprocess
import sys

port, baud = sys.argv[1], sys.argv[2]
cmd = ["idf.py", "-p", port, "-b", baud, "monitor"]

proc = subprocess.Popen(cmd, preexec_fn=os.setsid)


def _stop(signum, _frame):
    print("\n[monitor] 收到信号，结束监视器进程组 ...", flush=True)
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    sys.exit(128 + signum)


signal.signal(signal.SIGINT, _stop)
signal.signal(signal.SIGTERM, _stop)
rc = proc.wait()
sys.exit(rc if rc is not None else 0)
PY
