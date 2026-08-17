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
BIN="firmware/m5stack-cardputer-adv-car/xiaozhi-adv-car-${VER}.bin"
ZIP="firmware/m5stack-cardputer-adv-car/xiaozhi-adv-car-${VER}.zip"
NOTES="firmware/m5stack-cardputer-adv-car/RELEASE_${VER}.md"
[[ -f "$BIN" ]] || { echo "缺少 $BIN"; exit 1; }
if [[ ! -f "$NOTES" ]]; then
  echo "缺少 Release 说明 $NOTES"
  exit 1
fi

gh auth status
gh release delete "$VER" -y 2>/dev/null || true
gh release create "$VER" \
  --title "${VER} - M5Stack Cardputer ADV Car" \
  --notes-file "$NOTES" \
  "$BIN" "$ZIP"
echo "OK: https://github.com/hengmyj/xiaozhi-adv-car/releases/tag/${VER}"
