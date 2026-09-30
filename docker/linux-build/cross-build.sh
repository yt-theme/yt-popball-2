#!/usr/bin/env bash
# cross-build.sh —— 在本机架构的 Linux 容器里交叉编译出 amd64 二进制并打 amd64 的 .deb / .rpm
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
#   bash cross-build.sh <项目目录> <输出目录> [VERSION] [FORMAT]
#   FORMAT: deb（默认） | rpm | appimage | all（同时打 deb + rpm + appimage）
set -euo pipefail

SRC="$1"; OUT="$2"
FORMATS="${4:-deb}"
# 默认从 package.json 读 version（单一版本号来源）
[ -n "${3:-}" ] && VERSION="$3" || VERSION="$(sed -n 's/^  *"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$SRC/package.json" 2>/dev/null | head -1)"
[ -n "$VERSION" ] || VERSION="0.0.0"
[ -d "$SRC" ] || { echo "用法: $0 <项目目录> <输出目录> [VERSION] [FORMAT]" >&2; exit 1; }
[ -n "$OUT" ] || { echo "缺少输出目录" >&2; exit 1; }
case "$FORMATS" in deb|rpm|appimage|all) ;; *) echo "未知 FORMAT: $FORMATS（可选 deb / rpm / appimage / all）" >&2; exit 1 ;; esac
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
        pkg-config dpkg-dev ca-certificates rpm
fi

# 打 rpm 需要 rpmbuild（主镜像 Dockerfile 已装 rpm；早期预置的 *-cross 交叉镜像可能没有，
# 缺了就按当前 sources.list 补装 —— 仅打 rpm/all 时才装，打 deb/appimage 不引入额外依赖）。
if { [ "$FORMATS" = rpm ] || [ "$FORMATS" = all ]; } && ! command -v rpmbuild >/dev/null 2>&1; then
    echo ">>> 安装 rpm 打包工具（rpmbuild）"
    apt-get update -o Acquire::Retries=5
    apt-get install -y --no-install-recommends -o Acquire::Retries=5 rpm
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

echo ">>> moc（Q_OBJECT 头文件：settingsdialog.h / widget.h / popdock.h / clipstore.h）"
"$MOC" $QTI settingsdialog.h -o moc_settingsdialog.cpp
"$MOC" $QTI widget.h -o moc_widget.cpp
"$MOC" $QTI popdock.h -o moc_popdock.cpp
"$MOC" $QTI clipstore.h -o moc_clipstore.cpp

echo ">>> 交叉编译 amd64 二进制"
X11_CFLAGS="$(pkg-config --cflags x11 2>/dev/null || true)"
X11_LIBS="$(pkg-config --libs x11 2>/dev/null || true)"
DEFINES=""
[ -n "$X11_LIBS" ] && DEFINES="-DPOPBALL_HAVE_X11"
# 与 .pro 的 qtHaveModule(sql) 守卫一致：仅当交叉环境里有 amd64 的 QtSql 头文件时
# 才启用剪贴板历史持久化（POPBALL2_HAVE_QT_SQL）；否则 clipstore 退化为不落盘
# （预置交叉镜像常只带 qt6-base-dev 核心头，没有 QtSql）。
SQL_LIBS=""
if [ -f "$QTINC/QtSql/QSqlDatabase" ]; then
    DEFINES="$DEFINES -DPOPBALL2_HAVE_QT_SQL"
    SQL_LIBS="-lQt6Sql"
    # clipstore.cpp 用 `#include <QSqlDatabase>`（不带 QtSql/ 前缀），
    # 必须把 QtSql 的 include 目录加进编译路径，否则 "QSqlDatabase: No such file or directory"。
    QTI="$QTI -I$QTINC/QtSql"
    echo ">>> QtSql 头文件已找到 → 启用剪贴板历史持久化"
else
    echo ">>> WARN: 未找到 $QTINC/QtSql/QSqlDatabase → 剪贴板历史退化为内存模式（不落盘）"
fi
x86_64-linux-gnu-g++ -fPIC -std=c++17 $DEFINES $QTI $X11_CFLAGS \
    clipstore.cpp config.cpp main.cpp popdock.cpp settingsdialog.cpp sysInfo.cpp widget.cpp \
    moc_popdock.cpp moc_settingsdialog.cpp moc_widget.cpp moc_clipstore.cpp \
    qrc_default_config.cpp qrc_resources.cpp qrc_trans.cpp \
    -o popball2 -lQt6Widgets -lQt6Gui $SQL_LIBS -lQt6Core $X11_LIBS
echo ">>> 产物架构（应为 x86-64）:"
readelf -h popball2 | grep -E "Machine:|Class:" || file popball2
echo ">>> 需要的动态库（应含 amd64 的 libQt6*）:"
readelf -d popball2 | grep -i NEEDED

# ---------------------------------------------------------------- 打包公共件
# desktop 文件内容固定、无变量，两种包格式共用
write_desktop() {   # <目标文件>
    cat > "$1" <<'EOF'
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
}

# ---------------------------------------------------------------- 打包 deb
package_deb() {
    echo ">>> 打包 amd64 .deb"
    local STAGE
    STAGE="$(mktemp -d)"
    mkdir -p "$STAGE/usr/bin" "$STAGE/usr/share/applications" "$STAGE/usr/share/icons/hicolor/512x512/apps" "$STAGE/DEBIAN"
    install -m 0755 popball2 "$STAGE/usr/bin/popball2"
    write_desktop "$STAGE/usr/share/applications/popball2.desktop"
    if [ -f resources/popball2.png ]; then
        install -m 0644 resources/popball2.png "$STAGE/usr/share/icons/hicolor/512x512/apps/popball2.png"
    fi
    local SIZE
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
    rm -rf "$STAGE"
    echo ">>> 完成: $OUT/popball2_${VERSION}_amd64.deb"
}

# ---------------------------------------------------------------- 打包 rpm
# 与 package.sh 的 pkg_rpm 等价（spec 内 %build 为空，只安装已交叉编译好的 x86_64 二进制）。
# rpmbuild 只是打包器（arm64 原生运行即可），RPM 的 elf 依赖生成器直接读 ELF 头，
# 能正确产出 x86_64 的 libQt6*.so.6()(64bit) 依赖，不需要在 x86_64 环境执行。
package_rpm() {
    echo ">>> 打包 x86_64 .rpm"
    command -v rpmbuild >/dev/null 2>&1 || { echo "!!! 缺少 rpmbuild，无法打 rpm" >&2; return 1; }
    local TOP payload
    TOP="$(mktemp -d)/rpmbuild"
    mkdir -p "$TOP"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
    payload="$TOP/SOURCES/popball2-${VERSION}"
    mkdir -p "$payload"
    install -m 0755 popball2 "$payload/popball2"
    write_desktop "$payload/popball2.desktop"
    if [ -f resources/popball2.png ]; then
        install -m 0644 resources/popball2.png "$payload/popball2.png"
    fi
    ( cd "$TOP/SOURCES" && tar -czf "popball2-${VERSION}.tar.gz" "popball2-${VERSION}" )

    cat > "$TOP/SPECS/popball2.spec" <<EOF
Name:           popball2
Version:        $VERSION
Release:        1%{?dist}
Summary:        Floating desktop system monitor ball

License:        GPLv2
URL:            https://gitee.com/
Source0:        %{name}-%{version}.tar.gz

Requires:       qt6-qtbase

%global debug_package %{nil}
%global _build_id_links none
# 跳过 brp-strip/brp-compress 等安装后处理：避免本机(aarch64) strip 处理交叉(x86_64)二进制；
# deb 路径本来也不 strip，产物体积保持一致。
%global __os_install_post %{nil}

%description
A small always-on-top desktop ball that visualises memory and swap usage
as area charts, plus CPU usage, CPU temperature and network throughput.

%prep
%setup -q

%build
# x86_64 二进制已在交叉编译阶段构建完成，这里只做安装

%install
rm -rf %{buildroot}
install -d %{buildroot}%{_bindir}
install -d %{buildroot}%{_datadir}/applications
install -d %{buildroot}%{_datadir}/icons/hicolor/512x512/apps
install -m 0755 %{name} %{buildroot}%{_bindir}/%{name}
install -m 0644 %{name}.desktop %{buildroot}%{_datadir}/applications/%{name}.desktop
%{!?_without_icon:install -m 0644 %{name}.png %{buildroot}%{_datadir}/icons/hicolor/512x512/apps/%{name}.png}

%files
%{_bindir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/512x512/apps/%{name}.png

%changelog
* $(LC_ALL=C date '+%a %b %d %Y') packager - $VERSION-1
- Initial package
EOF

    # --target x86_64：声明产物架构（payload 里的二进制本身已是 x86_64）
    rpmbuild -bb --define "_topdir $TOP" --target x86_64 "$TOP/SPECS/popball2.spec"
    local found
    found="$(find "$TOP/RPMS" -name '*.rpm' | head -1)"
    if [ -z "$found" ]; then
        echo "!!! rpmbuild 成功但没找到 .rpm" >&2
        rm -rf "$TOP"
        return 1
    fi
    cp -f "$found" "$OUT/"
    echo ">>> 完成: $OUT/$(basename "$found")"
    rm -rf "$TOP"
}

# ---------------------------------------------------------------- 打包 AppImage
# 难点：linuxdeploy / appimagetool 是「按架构」的工具 —— linuxdeploy-x86_64 要在 x86_64
# 环境运行并对二进制跑 ldd，QEMU 模拟下会误判，不能用。
# 办法：手工组装 AppDir（递归 readelf NEEDED 收集 amd64 库 + patchelf 设 rpath），
# 合成仍用本机架构(aarch64)的 appimagetool —— 它只做 mksquashfs + 拼接 runtime，
# 与包内文件架构无关，runtime 单独用 x86_64 的 type2-runtime。
AI_ARCH=x86_64
AI_SYSROOT_DIRS=(/lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu/qt6/lib)
# glibc 本体/加载器一律不打包（与目标机 glibc 强绑定，打包反而崩，linuxdeploy 同样排除）
AI_GLIBC_SKIP='^(ld-linux-x86-64|libc|libm|libdl|libpthread|librt|libresolv|libutil|libanl|libcrypt|libBrokenLocale|libmvec|libnsl|libnss_[a-z]+)\.so'

ai_find_lib() {   # <soname> -> 找到则输出路径
    local soname="$1" d
    for d in "${AI_SYSROOT_DIRS[@]}"; do
        if [ -e "$d/$soname" ]; then echo "$d/$soname"; return 0; fi
    done
    return 1
}

package_appimage() {
    echo ">>> 打包 x86_64 AppImage"
    command -v patchelf >/dev/null 2>&1 || { echo "!!! 缺少 patchelf（主镜像应已安装）" >&2; return 1; }
    command -v readelf  >/dev/null 2>&1 || { echo "!!! 缺少 readelf（binutils）" >&2; return 1; }

    local APPDIR TOOLDIR HOSTTUPLE tool rt
    APPDIR="$(mktemp -d)/AppDir"
    TOOLDIR="$(mktemp -d)"
    mkdir -p "$APPDIR"/{usr/bin,usr/lib,usr/share/applications,usr/share/icons/hicolor/512x512/apps}

    # ---------- 二进制 + desktop + 图标 ----------
    install -m 0755 popball2 "$APPDIR/usr/bin/popball2"
    write_desktop "$APPDIR/usr/share/applications/popball2.desktop"
    if [ -f resources/popball2.png ]; then
        install -m 0644 resources/popball2.png "$APPDIR/usr/share/icons/hicolor/512x512/apps/popball2.png"
    fi

    # ---------- Qt 插件（amd64）----------
    # 只收 qt6-base 自带、依赖可自洽的插件；跳过需额外包的（qsvgicon→Qt6Svg、
    # platformthemes→GTK3、sqldrivers→sqlite、wayland→qt6-wayland，均不在交叉镜像内）。
    local PLUGSRC=/usr/lib/x86_64-linux-gnu/qt6/plugins sub so
    for sub in platforms imageformats xcbglintegrations; do
        for so in "$PLUGSRC/$sub"/*.so; do
            [ -e "$so" ] || continue
            install -D -m 0644 "$so" "$APPDIR/usr/plugins/$sub/$(basename "$so")"
        done
    done
    [ -e "$APPDIR/usr/plugins/platforms/libqxcb.so" ] \
        || { echo "!!! 缺少 platforms/libqxcb.so（无 X11 后端，AppImage 无法启动 GUI）" >&2; return 1; }

    # ---------- 递归收集 amd64 动态库（不能用本机 ldd：架构不同）----------
    echo ">>> 递归收集 amd64 运行库（readelf NEEDED）"
    declare -A AI_SEEN=()
    ai_collect() {   # <amd64 ELF>
        local elf="$1" needed path
        readelf -h "$elf" 2>/dev/null | grep -q 'X86-64' || return 0
        while read -r needed; do
            [ -z "$needed" ] && continue
            [[ "$needed" =~ $AI_GLIBC_SKIP ]] && continue
            [ -n "${AI_SEEN[$needed]:-}" ] && continue
            if path="$(ai_find_lib "$needed")"; then
                AI_SEEN[$needed]=1
                # 统一以 soname 名落盘（运行时按 soname 查找，无需保留版本符号链接）
                cp -L "$path" "$APPDIR/usr/lib/$needed"
                chmod 0644 "$APPDIR/usr/lib/$needed"
                ai_collect "$APPDIR/usr/lib/$needed"
            else
                echo ">>> WARN: 找不到 amd64 库 $needed（$(basename "$elf") 需要），跳过"
            fi
        done < <(readelf -d "$elf" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p')
    }
    ai_collect "$APPDIR/usr/bin/popball2"
    for so in "$APPDIR"/usr/plugins/*/*.so; do ai_collect "$so"; done
    echo ">>> 收集到 ${#AI_SEEN[@]} 个运行库"

    # ---------- rpath：让二进制/库/插件都能找到 usr/lib ----------
    # 注意库里的文件以 soname 命名（libfoo.so.6 而非 libfoo.so），glob 要用 *.so*
    patchelf --set-rpath '$ORIGIN/../lib' "$APPDIR/usr/bin/popball2"
    local f
    for f in "$APPDIR"/usr/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$f"; done
    for f in "$APPDIR"/usr/plugins/*/*.so; do patchelf --set-rpath '$ORIGIN/../../lib' "$f"; done

    # ---------- qt.conf：告诉 Qt 插件在 usr/plugins ----------
    cat > "$APPDIR/usr/bin/qt.conf" <<'EOF'
[Paths]
Prefix = ..
EOF

    # ---------- AppRun（shell 版，架构无关；官方惯例写法）----------
    cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/usr/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
export XDG_DATA_DIRS="$HERE/usr/share${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"
export XDG_CONFIG_DIRS="$HERE/usr/etc/xdg${XDG_CONFIG_DIRS:+:$XDG_CONFIG_DIRS}"
exec "$HERE/usr/bin/popball2" "$@"
EOF
    chmod 0755 "$APPDIR/AppRun"

    # ---------- AppDir 根的 desktop / 图标（appimagetool 硬性要求）----------
    cp -f "$APPDIR/usr/share/applications/popball2.desktop" "$APPDIR/popball2.desktop"
    if [ -f resources/popball2.png ]; then
        cp -f resources/popball2.png "$APPDIR/popball2.png"
        cp -f resources/popball2.png "$APPDIR/.DirIcon"
    fi

    # ---------- 准备 appimagetool（用「本机架构」版）与 x86_64 runtime ----------
    case "$(uname -m)" in
        aarch64|arm64) HOSTTUPLE=aarch64 ;;
        x86_64|amd64)  HOSTTUPLE=x86_64 ;;
        *) echo "!!! 交叉 AppImage 未测试的主机架构: $(uname -m)" >&2; return 1 ;;
    esac
    if [ -x "$SRC/tools/appimagetool-${HOSTTUPLE}.AppImage" ]; then
        tool="$SRC/tools/appimagetool-${HOSTTUPLE}.AppImage"
    else
        tool="$TOOLDIR/appimagetool"
        echo ">>> 下载 appimagetool-${HOSTTUPLE}"
        curl -fL --retry 2 -o "$tool" \
            "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-${HOSTTUPLE}.AppImage" \
            || { echo "!!! appimagetool 下载失败" >&2; return 1; }
        chmod +x "$tool"
    fi
    if [ -f "$SRC/tools/runtime-${AI_ARCH}" ]; then
        rt="$SRC/tools/runtime-${AI_ARCH}"
    else
        rt="$TOOLDIR/runtime-${AI_ARCH}"
        echo ">>> 下载 runtime-${AI_ARCH}"
        curl -fL --retry 2 -o "$rt" \
            "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-${AI_ARCH}" \
            || { echo "!!! runtime-${AI_ARCH} 下载失败" >&2; return 1; }
    fi

    # ---------- 合成（容器内无 FUSE：extract-and-run；-n 跳过 appstream 校验）----------
    local out="$OUT/popball2-${VERSION}-${AI_ARCH}.AppImage"
    rm -f "$out"
    echo ">>> appimagetool 合成 $out"
    if ! APPIMAGE_EXTRACT_AND_RUN=1 ARCH="$AI_ARCH" \
            "$tool" --runtime-file "$rt" -n "$APPDIR" "$out"; then
        echo "!!! appimagetool 合成失败" >&2
        rm -rf "$APPDIR" "$TOOLDIR"
        return 1
    fi
    chmod 0755 "$out"
    # POPBALL2_KEEP_APPDIR=1：保留组装好的 AppDir（排错用，由 package.sh --keep-stage 透传）。
    # 打成单个 tar.gz：package.sh 回拷用 cp -f（不带 -R），目录会被略过；tar 还保留权限与符号链接。
    if [ "${POPBALL2_KEEP_APPDIR:-0}" = 1 ]; then
        rm -f "$OUT/AppDir-amd64.tar.gz"
        tar czf "$OUT/AppDir-amd64.tar.gz" -C "$(dirname "$APPDIR")" "$(basename "$APPDIR")"
        echo ">>> AppDir 已保留（排错）: $OUT/AppDir-amd64.tar.gz"
    fi
    rm -rf "$APPDIR" "$TOOLDIR"
    echo ">>> 完成: $out"
}

# ---------------------------------------------------------------- 按 FORMAT 分发
case "$FORMATS" in
    deb)      package_deb ;;
    rpm)      package_rpm ;;
    appimage) package_appimage ;;
    all)      package_deb; package_rpm; package_appimage ;;
esac

# 清理编译中间产物（二进制已打进包）
rm -rf qrc_default_config.cpp qrc_resources.cpp qrc_trans.cpp trans.qrc popball2_zh_CN.qm \
       moc_settingsdialog.cpp moc_widget.cpp moc_popdock.cpp moc_clipstore.cpp popball2
echo ">>> 全部完成，产物目录: $OUT"
