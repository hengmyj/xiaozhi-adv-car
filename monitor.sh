#!/bin/bash
# monitor.sh — 串口监视（仅监视，不编译/烧录）
#
# 需先 source IDF 或使用默认 IDF_PATH（同 flash.sh）。
#
# 用 PTY 包装 idf.py monitor：miniterm.Console 必须对 stdin 做 tcgetattr。
# Cursor/VSCode 集成终端里，本机 stdin 常「看起来像 TTY」但 tcgetattr 报 EIO
# （接到 /dev/tty 也一样）。因此给子进程一个 openpty() 出来的真 PTY，
# 不再要求调用方 stdin 是完美 TTY。
#
# 退出方式：
#   Ctrl+C              本脚本结束监视器（推荐）
#   Ctrl+]              idf_monitor 官方退出键（按键能转发到 PTY 时）
#   ./monitor.sh kill   强制结束卡住的监视器进程
#
# 若 idf_monitor 仍立刻失败：自动降级为 pyserial 只读打印固件日志。
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
  pkill -f 'esp-idf/.*/tools/idf_monitor.py' 2>/dev/null || true
  pkill -f 'idf_monitor.py -p /dev/cu\.' 2>/dev/null || true
  pkill -f 'idf\.py .*-p /dev/cu\..* monitor' 2>/dev/null || true
  pkill -f 'xiaozhi-adv-monitor' 2>/dev/null || true
  for pid in $(pgrep -f '[.]/monitor\.sh' 2>/dev/null || true); do
    [ "$pid" = "$$" ] && continue
    [ "$pid" = "$PPID" ] && continue
    kill -TERM "$pid" 2>/dev/null || true
  done
  sleep 0.2
  if pgrep -f 'idf_monitor\.py' >/dev/null 2>&1 || pgrep -f 'xiaozhi-adv-monitor' >/dev/null 2>&1; then
    echo "[monitor] 仍有残留，改用 SIGKILL ..."
    pkill -9 -f 'idf_monitor\.py' 2>/dev/null || true
    pkill -9 -f 'idf\.py .*monitor' 2>/dev/null || true
    pkill -9 -f 'xiaozhi-adv-monitor' 2>/dev/null || true
  fi
  echo "[monitor] 完成。用 ls /dev/cu.usbmodem* 确认设备是否还在。"
}

if [ "${1:-}" = "kill" ] || [ "${1:-}" = "--kill" ]; then
  kill_stuck_monitors
  exit 0
fi

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

if [ ! -f "$IDF_PATH/export.sh" ]; then
  echo "[ERROR] 找不到 export.sh，请设置 IDF_PATH（当前: ${IDF_PATH}）"
  exit 1
fi
# shellcheck disable=SC1090
. "$IDF_PATH/export.sh"

echo "串口监视 ${BAUD}"
echo "Port: $PORT"
echo "退出: Ctrl+C（本脚本会结束进程）  |  或 Ctrl+]  |  卡住时: ./monitor.sh kill"
echo "提示: macOS 的 Cmd+C 是复制，不会中断进程。"
echo ""

# fd 3 = 本包装脚本，避免 stdin heredoc 把调用方 stdin 变成管道。
# 子进程 stdin 接到 openpty() 的 slave，miniterm 才能 tcgetattr 成功。
python3 /dev/fd/3 --xiaozhi-adv-monitor "$PORT" "$BAUD" 3<<'PY'
import os
import pty
import select
import signal
import subprocess
import sys
import termios
import time
import tty

argv = [a for a in sys.argv[1:] if a != "--xiaozhi-adv-monitor"]
port, baud = argv[0], argv[1]
cmd = ["idf.py", "-p", port, "-b", baud, "monitor"]


def serial_fallback(reason):
    print("[monitor] %s" % reason, flush=True)
    print("[monitor] 降级为直接读取串口（只看日志，无交互 Console）...", flush=True)
    try:
        import serial
    except ImportError:
        print("[ERROR] 当前 Python 没有 pyserial，无法降级。", file=sys.stderr, flush=True)
        sys.exit(1)
    ser = serial.Serial(port, int(baud), timeout=0.2)

    def _stop(signum, _frame):
        print("\n[monitor] 收到信号，结束串口读取 ...", flush=True)
        try:
            ser.close()
        except Exception:
            pass
        sys.exit(128 + signum)

    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)
    print("[monitor] pyserial %s %s" % (port, baud), flush=True)
    try:
        while ser.is_open:
            data = ser.read(4096)
            if data:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
    except KeyboardInterrupt:
        _stop(signal.SIGINT, None)
    finally:
        try:
            ser.close()
        except Exception:
            pass
    sys.exit(0)


def run_idf_with_pty():
    master_fd, slave_fd = pty.openpty()
    try:
        proc = subprocess.Popen(
            cmd,
            stdin=slave_fd,
            start_new_session=True,
        )
    except FileNotFoundError:
        os.close(master_fd)
        os.close(slave_fd)
        print("[ERROR] 找不到 idf.py，请确认已 source IDF export.sh。", file=sys.stderr, flush=True)
        sys.exit(1)
    os.close(slave_fd)

    orig_mode = None
    stdin_fd = None
    try:
        stdin_fd = sys.stdin.fileno()
        orig_mode = termios.tcgetattr(stdin_fd)
        tty.setraw(stdin_fd)
    except (termios.error, OSError, ValueError):
        orig_mode = None

    stop = {"done": False}

    def restore():
        if orig_mode is not None and stdin_fd is not None:
            try:
                termios.tcsetattr(stdin_fd, termios.TCSAFLUSH, orig_mode)
            except termios.error:
                pass

    def _stop(signum, _frame):
        if stop["done"]:
            return
        stop["done"] = True
        restore()
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
        try:
            os.close(master_fd)
        except OSError:
            pass
        sys.exit(128 + signum)

    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)

    try:
        fds = [master_fd]
        if stdin_fd is not None:
            fds.append(stdin_fd)
        while proc.poll() is None:
            try:
                rfds, _, _ = select.select(fds, [], [], 0.25)
            except InterruptedError:
                continue
            except OSError:
                break
            if master_fd in rfds:
                try:
                    data = os.read(master_fd, 4096)
                except OSError:
                    data = b""
                if data:
                    try:
                        os.write(sys.stdout.fileno(), data)
                    except OSError:
                        pass
            if stdin_fd is not None and stdin_fd in rfds:
                try:
                    data = os.read(stdin_fd, 1024)
                except OSError:
                    fds = [fd for fd in fds if fd != stdin_fd]
                    continue
                if not data:
                    fds = [fd for fd in fds if fd != stdin_fd]
                    continue
                if b"\x03" in data:
                    _stop(signal.SIGINT, None)
                try:
                    os.write(master_fd, data)
                except OSError:
                    break
    finally:
        restore()
        try:
            os.close(master_fd)
        except OSError:
            pass

    rc = proc.wait()
    return 0 if rc is None else rc


t0 = time.time()
rc = run_idf_with_pty()
if rc >= 128:
    sys.exit(rc)
elapsed = time.time() - t0
if rc != 0 and elapsed < 8:
    serial_fallback("idf.py monitor 很快退出（码 %s，%.1fs）" % (rc, elapsed))
sys.exit(rc)
PY
