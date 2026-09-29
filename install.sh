#!/usr/bin/env bash
#
# popball2 —— 安装脚本
#
# 解决的问题：当 .deb 位于挂载盘（如 /media/...）时，apt 以系统用户 _apt 读取
# 会因无权遍历该路径而触发沙盒提权提示（"注意: ... 无法被用户 '_apt' 访问...
# 已脱胇沙盒并提权为根用户"）。虽然它只是提示、不影响安装结果，但对调用方不友好。
#
# 本脚本先把 .deb 暂存到 /tmp（任何用户都可读）再用 sudo apt 安装，从而彻底绕开该提示。
#
# 用法:
#   ./install.sh                  # 安装 dist/ 下最新的 .deb
#   ./install.sh /path/x.deb      # 安装指定 .deb
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_NAME="popball2"

# ---------------- 定位 .deb ----------------
DEB="${1:-}"
if [ -z "$DEB" ]; then
    DEB="$(ls -1t "$SCRIPT_DIR"/dist/${APP_NAME}_*.deb 2>/dev/null | head -1 || true)"
fi
[ -n "$DEB" ] && [ -f "$DEB" ] || {
    echo "错误: 找不到 .deb（用法: ./install.sh [xxx.deb]，缺省取 dist/ 下最新）" >&2
    exit 1
}
DEB="$(cd "$(dirname "$DEB")" && pwd)/$(basename "$DEB")"
echo "==> 使用 $DEB"

# ---------------- 暂存到可读路径再安装 ----------------
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/${APP_NAME}-install.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT

STAGED="$STAGE/${APP_NAME}.deb"
if command -v install >/dev/null 2>&1; then
    install -m 0644 "$DEB" "$STAGED"
else
    cp -f "$DEB" "$STAGED"
fi

echo "==> 已暂存到 $STAGED（避免 _apt 读取挂载盘时的沙盒提示）"
echo "==> 正在安装 ..."
sudo apt install "$STAGED"