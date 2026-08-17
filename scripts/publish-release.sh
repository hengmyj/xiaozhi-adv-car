#!/bin/bash
# 在本机用已登录的 hengmyj 账号发布 Release（Cloud Agent 无法用你的身份发布）
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VER="${1:-}"
if [[ -z "$VER" ]]; then
  echo "用法: $0 <版本号，如 v2.2.7>"
  exit 1
fi
VER_NUM="${VER#v}"
BIN="firmware/m5stack-cardputer-adv-car/xiaozhi-adv-car-${VER}.bin"
ZIP="firmware/m5stack-cardputer-adv-car/xiaozhi-adv-car-${VER}.zip"
[[ -f "$BIN" ]] || { echo "缺少 $BIN"; exit 1; }

gh auth status
gh release delete "$VER" -y 2>/dev/null || true
gh release create "$VER" \
  --title "${VER} - M5Stack Cardputer ADV Car" \
  --notes "## M5Stack Cardputer ADV Car

板型: \`m5stack-cardputer-adv-car\` | Flash 8MB | ESP32-S3

\`\`\`bash
python -m esptool --chip esp32s3 -b 460800 -p PORT \\
  --before default_reset --after hard_reset \\
  write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m \\
  0x0 xiaozhi-adv-car-${VER}.bin
\`\`\`" \
  "$BIN" "$ZIP"
echo "OK: https://github.com/hengmyj/xiaozhi-adv-car/releases/tag/${VER}"
