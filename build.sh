#!/usr/bin/env bash
#
# popball2 —— 一键构建并打包
#
# 本脚本是 package.sh 的薄封装：解析参数后【全部转发】给同目录的 package.sh，
# 由它完成「本机构建 + Docker 多架构打包(x86_64 + arm64) + 容错」。
#
# 这样 Docker 多架构 / 平台判断 / 容错逻辑只有一份实现（在 package.sh 里），
# 不会出现 build.sh 与 package.sh 各写一套、互相漂移的问题。
#
# 平台与方式（由 package.sh 决定）：
#   • macOS 宿主：macOS 包本机构建；Linux 的 x86_64(amd64) 与 aarch64(arm64) .deb 用 Docker 构建。
#   • Linux 宿主：不打包 macOS；Linux 的 x86_64 与 aarch64 .deb 默认用 Docker 构建
#                 （无 Docker 或 --no-docker 时退回本机架构原生构建）。
#   • Windows 宿主（Git Bash / MSYS2）：打出 zip（windeployqt 内置 Qt 运行库，
#                 并附带 WinRing0 驱动与 README-Windows.txt）。Qt / MinGW 会自动探测。
#
# 容错（由 package.sh 保证）：任一目标失败都只记录并跳过，其余目标继续执行。
#
# 所有参数原样透传给 package.sh，详见 ./package.sh --help
#
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG="$SCRIPT_DIR/package.sh"
[ -f "$PKG" ] || { echo "错误: 找不到同目录下的 package.sh" >&2; exit 1; }

# 透传所有参数给 package.sh（它会做真正的构建与打包）
exec "$PKG" "$@"
