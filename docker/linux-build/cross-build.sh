#!/usr/bin/env bash
# cross-build.sh —— 在本机架构的 Linux 容器里交叉编译出 amd64 二进制并打 amd64 .deb
#
# 适用场景：Docker 守护进程无法拉取「目标架构」的基础镜像（例如代理只放行 HTTP / 有主机白名单，
#           导致 `docker build --platform linux/amd64` 在 FROM 阶段就失败），但：
#             • 本机架构的容器已经存在并装好了 qt6 工具链；
#             • 容器内 apt（走 HTTP 代理）可用，能下载目标架构的 Qt6 与交叉编译器（multiarch）。
#           此时不必拉目标架构基础镜像，直接在本机架构容器里交叉编译即可。
#
# 关键点（踩坑记录）：
#   1) amd64（x86_64）的 Ubuntu 包在 archive.ubuntu.com（主归档）；ports.ubuntu.com 只放 arm64 等
#      「移植架构」。而交叉编译器 gcc-x86-64-linux-gnu 本身是 arm64 包（运行在 arm64、产出 amd64），
#      来自 ports。所以 sources 里 arm64→ports、amd64→archive 两套都要有。
#   2) 不能用本机 arm64 的 qmake（会把链接指向 arm64 的 Qt）。用架构无关的 lrelease/rcc/moc +
#      显式 x86_64-linux-gnu-g++ + amd64 头文件/库路径。
#   3) Ubuntu 22.04 的 Qt6 不提供 pkg-config .pc 文件，必须手写 -I / -l 路径。
#   4) rcc 对多个 .qrc 必须用 `--name` 区分，否则 qInitResources() 符号重复冲突。
#   5) moc/rcc 在 /usr/lib/qt6/libexec/（/usr/bin 下是 qtchooser 包装脚本，本镜像未配置 qt6 → 报错）。
#
# 用法（在本机架构容器内执行）:
#   bash cross-build.sh <项目目录> <输出目录> [VERSION]
set -euo pipefail

SRC="$1"; OUT="$2"
# 默认从 package.json 读 version（单一版本号来源）
[ -n "${3:-}" ] && VERSION="$3" || VERSION="$(sed -n 's/^  *"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$SRC/package.json" 2>/dev/null | head -1)"
[ -n "$VERSION" ] || VERSION="0.0.0"
[ -d "$SRC" ] || { echo "用法: $0 <项目目录> <输出目录> [VERSION]" >&2; exit 1; }
[ -n "$OUT" ] || { echo "缺少输出目录" >&2; exit 1; }
mkdir -p "$OUT"
cd "$SRC"

export DEBIAN_FRONTEND=noninteractive
PROXY="${HTTP_PROXY:-http://host.docker.internal:17897}"
export HTTP_PROXY="$PROXY" HTTPS_PROXY="$PROXY" http_proxy="$PROXY" https_proxy="$PROXY"

echo ">>> 目标架构: amd64 ；apt 代理: $PROXY"

# 若镜像里已预置交叉工具链（例如已 commit 的 popball2-linux-build:*-cross），直接跳过 apt，免网络。
if command -v x86_64-linux-gnu-g++ >/dev/null 2>&1 && dpkg -s qt6-base-dev:amd64 >/dev/null 2>&1; then
    echo ">>> 交叉工具链已就绪（预置镜像），跳过 apt"
else
    echo ">>> 启用 amd64 架构；arm64 包走 ports.ubuntu.com，amd64 包走 archive.ubuntu.com"
    dpkg --add-architecture amd64
    cat > /etc/apt/sources.list <<'EOF'
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy main universe
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-updates main universe
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-security main universe
deb [arch=amd64] http://archive.ubuntu.com/ubuntu jammy main universe
deb [arch=amd64] http://archive.ubuntu.com/ubuntu jammy-updates main universe
deb [arch=amd64] http://archive.ubuntu.com/ubuntu jammy-security main universe
EOF
    rm -f /etc/apt/sources.list.d/* 2>/dev/null || true

    apt-get update -o Acquire::Retries=10
    # gcc/g++-x86-64-linux-gnu：运行在 arm64、产出 amd64 的交叉编译器（arm64 包，来自 ports）；
    # amd64 的目标库（Qt6/X11）来自 archive；libc6-dev:amd64 提供 amd64 的 crt/libc。
    # Acquire::Retries + --fix-missing：镜像/代理偶发 502 时自动重试，避免整个打包失败。
    apt-get install -y --no-install-recommends -o Acquire::Retries=10 --fix-missing \
        gcc-x86-64-linux-gnu g++-x86-64-linux-gnu libc6-dev:amd64 \
        qt6-base-dev:amd64 libx11-dev:amd64 libgl1:amd64 libglu1-mesa-dev:amd64 \
        libxkbcommon-dev:amd64 libfontconfig1-dev:amd64 libfreetype6-dev:amd64 \
        pkg-config dpkg-dev ca-certificates
fi

# 架构无关工具（用真二进制，绕开 qtchooser 包装脚本）
LRELEASE=/usr/lib/qt6/bin/lrelease
RCC=/usr/lib/qt6/libexec/rcc
MOC=/usr/lib/qt6/libexec/moc
[ -x "$LRELEASE" ] || LRELEASE="$(command -v lrelease)"
[ -x "$RCC" ] || RCC="$(command -v rcc)"
[ -x "$MOC" ] || MOC="$(command -v moc)"

# amd64 的 Qt6 头/库位置
QTINC=/usr/include/x86_64-linux-gnu/qt6
QTI="-I$QTINC -I$QTINC/QtCore -I$QTINC/QtGui -I$QTINC/QtWidgets"
export PKG_CONFIG_PATH=/usr/lib/x86_64-linux-gnu/pkgconfig

echo ">>> 翻译 .ts -> .qm"
"$LRELEASE" popball2_zh_CN.ts -qm popball2_zh_CN.qm

echo ">>> 资源 .qrc -> .cpp（--name 区分，避免 qInitResources 符号冲突）"
"$RCC" --name default_config default_config.qrc -o qrc_default_config.cpp
"$RCC" --name resources      resources.qrc      -o qrc_resources.cpp
printf '<RCC><qresource prefix="/translations"><file>popball2_zh_CN.qm</file></qresource></RCC>' > trans.qrc
"$RCC" --name trans          trans.qrc          -o qrc_trans.cpp

echo ">>> moc（Q_OBJECT 头文件：settingsdialog.h / widget.h）"
"$MOC" $QTI settingsdialog.h -o moc_settingsdialog.cpp
"$MOC" $QTI widget.h -o moc_widget.cpp

echo ">>> 交叉编译 amd64 二进制"
X11_CFLAGS="$(pkg-config --cflags x11 2>/dev/null || true)"
X11_LIBS="$(pkg-config --libs x11 2>/dev/null || true)"
DEFINES=""
[ -n "$X11_LIBS" ] && DEFINES="-DPOPBALL_HAVE_X11"
x86_64-linux-gnu-g++ -fPIC -std=c++17 $DEFINES $QTI $X11_CFLAGS \
    config.cpp main.cpp settingsdialog.cpp sysInfo.cpp widget.cpp \
    moc_settingsdialog.cpp moc_widget.cpp \
    qrc_default_config.cpp qrc_resources.cpp qrc_trans.cpp \
    -o popball2 -lQt6Widgets -lQt6Gui -lQt6Core $X11_LIBS
echo ">>> 产物架构（应为 x86-64）:"
readelf -h popball2 | grep -E "Machine:|Class:" || file popball2
echo ">>> 需要的动态库（应含 amd64 的 libQt6*）:"
readelf -d popball2 | grep -i NEEDED

echo ">>> 打包 amd64 .deb"
STAGE="$(mktemp -d)"
mkdir -p "$STAGE/usr/bin" "$STAGE/usr/share/applications" "$STAGE/usr/share/icons/hicolor/512x512/apps" "$STAGE/DEBIAN"
install -m 0755 popball2 "$STAGE/usr/bin/popball2"
cat > "$STAGE/usr/share/applications/popball2.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=PopBall
Name[zh_CN]=PopBall 系统监控球
GenericName=System Monitor
Comment=Floating desktop ball showing memory, swap, CPU and network usage
Exec=popball2
Icon=popball2
Terminal=false
Categories=Utility;System;Monitor;
EOF
if [ -f resources/popball2.png ]; then
    install -m 0644 resources/popball2.png "$STAGE/usr/share/icons/hicolor/512x512/apps/popball2.png"
fi
SIZE="$(du -sk "$STAGE" | awk '{print $1}')"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: popball2
Version: $VERSION
Section: utils
Priority: optional
Architecture: amd64
Maintainer: popball2 <noreply@example.com>
Installed-Size: $SIZE
Depends: libc6, libstdc++6, libqt6core6 | libqt6core6t64, libqt6gui6 | libqt6gui6t64, libqt6widgets6 | libqt6widgets6t64, libx11-6
Description: Floating desktop system monitor ball
 A small always-on-top desktop ball that visualises memory and swap usage
 as area charts, plus CPU usage, CPU temperature and network throughput.
EOF
dpkg-deb --root-owner-group --build "$STAGE" "$OUT/popball2_${VERSION}_amd64.deb"
rm -rf "$STAGE" qrc_default_config.cpp qrc_resources.cpp qrc_trans.cpp trans.qrc popball2_zh_CN.qm moc_settingsdialog.cpp moc_widget.cpp popball2
echo ">>> 完成: $OUT/popball2_${VERSION}_amd64.deb"
