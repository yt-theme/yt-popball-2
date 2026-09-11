#!/usr/bin/env bash
#
# popball2 —— 打包脚本
#
#   支持产出：deb / rpm / AppImage / macOS(dmg + zip)
#   在 Linux 上默认打 deb + rpm + AppImage；在 macOS 上默认打 dmg + zip
#
# 用法与示例见 ./package.sh --help
#
set -euo pipefail

# ---------------------------------------------------------------- 基本路径
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
APP_NAME="popball2"
PRO_FILE="$PROJECT_DIR/${APP_NAME}.pro"
ICON_PNG="$PROJECT_DIR/resources/${APP_NAME}.png"
ICON_ICNS="$PROJECT_DIR/resources/${APP_NAME}.icns"

# ---------------------------------------------------------------- 默认参数
VERSION=""
OUT_DIR="$PROJECT_DIR/dist"
PREFIX="/usr"
ARCH_OVERRIDE=""
JOBS=""
DO_BUILD=1
KEEP_STAGE=0
FORCE_CROSS=0
TARGETS=()

# ---------------------------------------------------------------- 输出样式
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_R=$'\033[0m'; C_B=$'\033[1m'
    C_G=$'\033[32m'; C_Y=$'\033[33m'; C_RD=$'\033[31m'; C_C=$'\033[36m'
else
    C_R=; C_B=; C_G=; C_Y=; C_RD=; C_C=
fi
step() { printf '\n%s==> %s%s\n' "$C_B$C_C" "$*" "$C_R"; }
info() { printf '    %s\n' "$*"; }
ok()   { printf '    %s✓%s %s\n' "$C_G" "$C_R" "$*"; }
warn() { printf '    %s!%s %s\n' "$C_Y" "$C_R" "$*"; }
err()  { printf '    %s✗ %s%s\n' "$C_RD" "$*" "$C_R" >&2; }
die()  { err "$*"; exit 1; }

usage() {
    cat <<EOF
${C_B}popball2 打包脚本${C_R}

用法: ./package.sh [目标...] [选项]

目标（可多选，缺省=当前平台支持的全部）:
  deb         Debian/Ubuntu 的 .deb
  rpm         Fedora/RHEL/openSUSE 的 .rpm
  appimage    通用 Linux AppImage
  mac         macOS 的 .dmg 与 .zip

选项:
      --version V    版本号            (默认: 读取 .pro 里的 VERSION)
      --arch ARCH    目标架构          (默认: 本机架构)
      --out DIR      产物输出目录      (默认: <项目>/dist)
      --prefix DIR   deb/rpm 安装前缀  (默认: /usr)
      --no-build     复用已有构建产物，不重新编译
      --jobs N       并行编译任务数
      --keep-stage   保留中间打包目录（排错用）
      --skip-platform-check
                     跳过平台校验。仅用于验证打包流程本身
                     （产物内是当前平台的可执行文件，不能直接分发）
  -h, --help         显示帮助

示例:
  ./package.sh                       # 本平台全部格式
  ./package.sh deb rpm               # 只要 deb 和 rpm
  ./package.sh mac --version 1.2.0
  ./package.sh all --out ~/pkgs
EOF
}

# ---------------------------------------------------------------- 参数解析
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)     usage; exit 0 ;;
        --version)     VERSION="${2:?--version 需要一个参数}"; shift ;;
        --arch)        ARCH_OVERRIDE="${2:?--arch 需要一个参数}"; shift ;;
        --out)         OUT_DIR="${2:?--out 需要一个参数}"; shift ;;
        --prefix)      PREFIX="${2:?--prefix 需要一个参数}"; shift ;;
        --jobs)        JOBS="${2:?--jobs 需要一个参数}"; shift ;;
        --no-build)    DO_BUILD=0 ;;
        --keep-stage)  KEEP_STAGE=1 ;;
        --skip-platform-check) FORCE_CROSS=1 ;;
        deb|rpm|appimage|mac|all) TARGETS+=("$1") ;;
        *) die "未知参数或目标: ${1}（用 --help 查看用法）" ;;
    esac
    shift
done

# ---------------------------------------------------------------- 环境识别
OS_KIND=""
case "$(uname -s)" in
    Linux)  OS_KIND=linux ;;
    Darwin) OS_KIND=macos ;;
    *)      die "不支持的平台: $(uname -s)（本脚本仅支持 Linux 与 macOS）" ;;
esac

# 读 .pro 里的版本号
[ -n "$VERSION" ] || VERSION="$(sed -n 's/^VERSION *= *//p' "$PRO_FILE" 2>/dev/null | head -1)"
[ -n "$VERSION" ] || VERSION="0.0.0"

# 架构映射
MACH="$(uname -m)"
if [ -n "$ARCH_OVERRIDE" ]; then MACH="$ARCH_OVERRIDE"; fi
case "$MACH" in
    x86_64|amd64)     DEB_ARCH=amd64;  RPM_ARCH=x86_64;  AI_ARCH=x86_64;  MAC_ARCH=x86_64 ;;
    aarch64|arm64)    DEB_ARCH=arm64;  RPM_ARCH=aarch64; AI_ARCH=aarch64; MAC_ARCH=arm64 ;;
    armv7l|armhf)     DEB_ARCH=armhf;  RPM_ARCH=armv7hl; AI_ARCH=armhf;   MAC_ARCH=arm ;;
    i386|i686)        DEB_ARCH=i386;   RPM_ARCH=i686;    AI_ARCH=i686;    MAC_ARCH=x86 ;;
    *)                DEB_ARCH="$MACH"; RPM_ARCH="$MACH"; AI_ARCH="$MACH"; MAC_ARCH="$MACH" ;;
esac

# 缺省目标
if [ ${#TARGETS[@]} -eq 0 ]; then
    if [ "$OS_KIND" = macos ]; then TARGETS=(mac); else TARGETS=(deb rpm appimage); fi
fi
# all 展开
EXPANDED=()
for t in "${TARGETS[@]}"; do
    if [ "$t" = all ]; then
        if [ "$OS_KIND" = macos ]; then EXPANDED+=(mac); else EXPANDED+=(deb rpm appimage); fi
    else
        EXPANDED+=("$t")
    fi
done
TARGETS=("${EXPANDED[@]}")
# 去重
UNIQ=()
for t in "${TARGETS[@]}"; do
    dup=0
    for u in ${UNIQ[@]+"${UNIQ[@]}"}; do [ "$u" = "$t" ] && dup=1; done
    [ "$dup" -eq 0 ] && UNIQ+=("$t")
done
TARGETS=("${UNIQ[@]}")

# 平台校验
for t in "${TARGETS[@]}"; do
    if [ "$FORCE_CROSS" -eq 1 ]; then
        warn "已跳过平台校验（--skip-platform-check）：产物里是当前平台的可执行文件，仅供验证打包流程"
        break
    fi
    if [ "$t" = mac ] && [ "$OS_KIND" != macos ]; then
        die "mac 包只能在 macOS 上构建（当前: ${OS_KIND}）"
    fi
    if [ "$t" != mac ] && [ "$OS_KIND" != linux ]; then
        die "$t 包只能在 Linux 上构建（当前: ${OS_KIND}）"
    fi
done

printf '%s%s %s%s\n' "$C_B" "$APP_NAME" "$VERSION" "$C_R"
info "平台    : $OS_KIND ($MACH)"
info "打包目标: ${TARGETS[*]}"
info "输出目录: $OUT_DIR"

# ---------------------------------------------------------------- 查找 qmake
QMAKE=""
find_qmake() {
    local c
    for c in qmake6 qmake-qt6; do
        if command -v "$c" >/dev/null 2>&1; then QMAKE="$(command -v "$c")"; return 0; fi
    done
    if [ "$OS_KIND" = macos ] && command -v brew >/dev/null 2>&1; then
        local p
        for p in "$(brew --prefix qt 2>/dev/null)" "$(brew --prefix qt@6 2>/dev/null)"; do
            if [ -n "$p" ] && [ -x "$p/bin/qmake6" ]; then QMAKE="$p/bin/qmake6"; return 0; fi
        done
    fi
    if command -v qmake >/dev/null 2>&1 \
       && qmake -query QT_VERSION 2>/dev/null | grep -q '^6\.'; then
        QMAKE="$(command -v qmake)"; return 0
    fi
    return 1
}
find_qmake || die "找不到 qmake6，请先运行 ./run.sh 安装开发环境"

# ---------------------------------------------------------------- 产物完整性自检
# Linux：ldd 检查二进制动态库是否全部可解析（构建容器里 Qt 已装，理论上不应缺失）。
check_linux_deps() {
    [ "$OS_KIND" = linux ] || return 0
    command -v ldd >/dev/null 2>&1 || return 0
    local miss
    miss="$(ldd "$APP_BIN" 2>/dev/null | awk '/not found/{print $1}' || true)"
    if [ -n "$miss" ]; then
        err "以下库在构建环境里找不到（二进制不完整，分发后可能跑不起来）:"
        printf '      %s\n' "$miss" >&2
        die "请先在构建环境安装缺失的运行库（Docker 镜像已装 qt6-base-dev 等，理论上不应发生）"
    fi
    ok "二进制动态库全部可解析（ldd 无 not found）"
}

# macOS：检查 Qt 框架是否已被 macdeployqt 打进 .app（否则换台 Mac 可能跑不了）。
# 必须在 macdeployqt 之后调用，故只从 pkg_mac 里调用（传入打包后的 .app 内二进制路径）。
check_mac_bundle() {
    local bin="${1:-$APP_BIN}"
    [ "$OS_KIND" = macos ] || return 0
    [ -x "$bin" ] || return 0
    command -v otool >/dev/null 2>&1 || return 0
    local bad
    # 注意：Qt 已内置时，两次 grep 后没有剩余行，grep 会返回 1；
    # 在 set -o pipefail + set -e 下会让赋值失败并中断脚本，故末尾加 || true。
    bad="$(otool -L "$bin" 2>/dev/null \
        | grep -iE 'Qt(Core|Gui|Widgets)' \
        | grep -vE '@executable_path/../Frameworks|@rpath' \
        | awk '{print $1}' || true)"
    if [ -n "$bad" ]; then
        warn "以下 Qt 库似乎没有被 macdeployqt 打进 .app（可能影响在其他 Mac 上运行）:"
        printf '      %s\n' "$bad"
    else
        ok "Qt 框架已随 .app 内置（@rpath / Frameworks）"
    fi
}

# ---------------------------------------------------------------- 构建
# 允许用环境变量覆盖构建目录（Docker 容器内把 Linux 构建隔离到容器本地，
# 避免污染宿主机的 macOS 构建产物 build-pkg）。
BUILD_DIR="${POPBALL2_BUILD_DIR:-$PROJECT_DIR/build-pkg}"
# 中间产物目录，稍后用 mktemp 建在本地临时盘上（见下方说明）
WORK_DIR=""
APP_BIN=""
APP_BUNDLE=""

build_app() {
    step "构建 $APP_NAME ${VERSION}（release）"
    mkdir -p "$BUILD_DIR"
    ( cd "$BUILD_DIR" && "$QMAKE" "$PRO_FILE" PREFIX="$PREFIX" >/dev/null )
    local log="$BUILD_DIR/pkg-build.log"
    if ! ( cd "$BUILD_DIR" && make -j"$JOBS" ) >"$log" 2>&1; then
        tail -40 "$log" >&2
        die "编译失败，完整日志: $log"
    fi
    ok "编译完成"
}

locate_app() {
    if [ -x "$BUILD_DIR/$APP_NAME.app/Contents/MacOS/$APP_NAME" ]; then
        APP_BUNDLE="$BUILD_DIR/$APP_NAME.app"
        APP_BIN="$APP_BUNDLE/Contents/MacOS/$APP_NAME"
    elif [ -x "$BUILD_DIR/$APP_NAME" ]; then
        APP_BIN="$BUILD_DIR/$APP_NAME"
    else
        die "找不到构建产物（先去掉 --no-build 跑一次）"
    fi
}

if [ "$DO_BUILD" -eq 1 ]; then
    if [ -z "$JOBS" ]; then
        if command -v nproc >/dev/null 2>&1; then JOBS="$(nproc)"
        else JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"; fi
    fi
    build_app
fi
locate_app
info "可执行文件: $APP_BIN"
check_linux_deps

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

# 中间产物一律放本地临时盘。
# 原因：macdeployqt / rpmbuild / linuxdeploy 要拷贝成千上万个小文件（Qt 框架里
# 还大量使用符号链接），如果项目本身在 U 盘或网络盘上，这一步会非常慢，甚至直接卡住
# （实测在外置 APFS 盘上 macdeployqt 会停在半途）。编译产物仍在项目里，方便增量编译。
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/${APP_NAME}-pkg.XXXXXX")"
cleanup_work() {
    if [ "$KEEP_STAGE" -eq 1 ]; then
        printf '\n中间目录已保留（--keep-stage）: %s\n' "$WORK_DIR"
    else
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup_work EXIT
info "工作目录: $WORK_DIR"

# ---------------------------------------------------------------- 公共：desktop 与图标
make_desktop_file() {
    local dest="$1"
    cat > "$dest" <<EOF
[Desktop Entry]
Type=Application
Name=PopBall
Name[zh_CN]=PopBall 系统监控球
GenericName=System Monitor
GenericName[zh_CN]=系统监控
Comment=Floating desktop ball showing memory, swap, CPU and network usage
Comment[zh_CN]=桌面悬浮小球，实时显示内存、交换分区、CPU 占用与网速
Exec=$APP_NAME
Icon=$APP_NAME
Terminal=false
Categories=Utility;System;Monitor;
StartupNotify=false
Keywords=monitor;system;cpu;memory;network;swap;
EOF
}

# 应用内容清单（各包格式共用）
populate_root() {
    local root="$1" bindir="$2"
    mkdir -p "$bindir" "$root/usr/share/applications" "$root/usr/share/icons/hicolor/512x512/apps"
    install -m 0755 "$APP_BIN" "$bindir/$APP_NAME"
    make_desktop_file "$root/usr/share/applications/$APP_NAME.desktop"
    if [ -f "$ICON_PNG" ]; then
        install -m 0644 "$ICON_PNG" "$root/usr/share/icons/hicolor/512x512/apps/$APP_NAME.png"
    else
        warn "缺少图标 ${ICON_PNG}（deb/rpm/AppImage 会没有图标）"
    fi
}

# ================================================================== deb
pkg_deb() {
    step "打包 deb"
    local stage="$WORK_DIR/deb"
    rm -rf "$stage"; mkdir -p "$stage/DEBIAN"
    populate_root "$stage" "$stage$PREFIX/bin"

    local size
    size="$(du -sk "$stage" | awk '{print $1}')"

    cat > "$stage/DEBIAN/control" <<EOF
Package: $APP_NAME
Version: $VERSION
Section: utils
Priority: optional
Architecture: $DEB_ARCH
Maintainer: popball2 <noreply@example.com>
Installed-Size: $size
Depends: libc6, libqt6core6 | libqt6core6t64, libqt6gui6 | libqt6gui6t64, libqt6widgets6 | libqt6widgets6t64
Description: Floating desktop system monitor ball
 A small always-on-top desktop ball that visualises memory and swap usage
 as area charts, plus CPU usage, CPU temperature and network throughput.
 Supports Intel/AMD, ARM64, IBM POWER and IBM Z on Linux, and macOS.
EOF
    local out="$OUT_DIR/${APP_NAME}_${VERSION}_${DEB_ARCH}.deb"
    if command -v dpkg-deb >/dev/null 2>&1; then
        dpkg-deb --root-owner-group --build "$stage" "$out" >/dev/null 2>&1 \
            || dpkg-deb --build "$stage" "$out" >/dev/null
    else
        warn "系统没有 dpkg-deb，改用 ar+tar 手工组装（产物结构等价）"
        local t="$WORK_DIR/debtmp"; rm -rf "$t"; mkdir -p "$t"
        ( cd "$stage/DEBIAN" && tar -czf "$t/control.tar.gz" . )
        ( cd "$stage" && tar -czf "$t/data.tar.gz" --exclude=./DEBIAN . )
        printf '2.0\n' > "$t/debian-binary"
        ( cd "$t" && ar rc "$out" debian-binary control.tar.gz data.tar.gz )
    fi
    ok "deb  -> $out"
}

# ================================================================== rpm
pkg_rpm() {
    step "打包 rpm"
    if ! command -v rpmbuild >/dev/null 2>&1; then
        warn "缺少 rpmbuild，跳过 rpm 打包"
        info "安装后重试： Debian/Ubuntu: sudo apt install rpm    Fedora: sudo dnf install rpm-build"
        return 0
    fi
    local top="$WORK_DIR/rpmbuild"
    rm -rf "$top"; mkdir -p "$top"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

    # 预构建产物打成 src 包，spec 只做安装
    local payload="$top/SOURCES/${APP_NAME}-${VERSION}"
    mkdir -p "$payload"
    populate_root "$payload/.pkgroot" "$payload/.pkgroot/usr/bin"
    # 摊平：把 payload 里的文件放到 tarball 顶层，便于 %install 直接 install
    cp -f "$payload/.pkgroot/usr/bin/$APP_NAME" "$payload/$APP_NAME"
    cp -f "$payload/.pkgroot/usr/share/applications/$APP_NAME.desktop" "$payload/$APP_NAME.desktop"
    [ -f "$payload/.pkgroot/usr/share/icons/hicolor/512x512/apps/$APP_NAME.png" ] \
        && cp -f "$payload/.pkgroot/usr/share/icons/hicolor/512x512/apps/$APP_NAME.png" "$payload/$APP_NAME.png"
    rm -rf "$payload/.pkgroot"
    ( cd "$top/SOURCES" && tar -czf "${APP_NAME}-${VERSION}.tar.gz" "${APP_NAME}-${VERSION}" )

    cat > "$top/SPECS/${APP_NAME}.spec" <<EOF
Name:           $APP_NAME
Version:        $VERSION
Release:        1%{?dist}
Summary:        Floating desktop system monitor ball

License:        GPLv2
URL:            https://gitee.com/
Source0:        %{name}-%{version}.tar.gz

Requires:       qt6-qtbase

%global debug_package %{nil}
%global _build_id_links none

%description
A small always-on-top desktop ball that visualises memory and swap usage
as area charts, plus CPU usage, CPU temperature and network throughput.

%prep
%setup -q

%build
# 二进制已在 CI/本地预先构建，这里只做安装

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

    # 目标架构（交叉打包时才需要显式指定）
    local defines=(--define "_topdir $top")
    if [ "$RPM_ARCH" != "$(uname -m | sed 's/x86_64/x86_64/;s/arm64/aarch64/')" ] && [ -n "$RPM_ARCH" ]; then
        defines+=(--target "$RPM_ARCH")
    fi

    if rpmbuild -bb "${defines[@]}" "$top/SPECS/${APP_NAME}.spec" >"$WORK_DIR/rpm.log" 2>&1; then
        local found
        found="$(find "$top/RPMS" -name '*.rpm' | head -1)"
        [ -n "$found" ] || die "rpmbuild 成功但没找到 .rpm（日志: $WORK_DIR/rpm.log）"
        cp -f "$found" "$OUT_DIR/"
        ok "rpm  -> $OUT_DIR/$(basename "$found")"
    else
        tail -30 "$WORK_DIR/rpm.log" >&2
        die "rpmbuild 失败，完整日志: $WORK_DIR/rpm.log"
    fi
}

# ================================================================== AppImage
ai_tool() {  # ai_tool <名字> -> 回显可用路径
    local n="$1" p
    for p in "$(command -v "$n" 2>/dev/null || true)" "$PROJECT_DIR/tools/$n" "$PROJECT_DIR/tools/$n-$AI_ARCH.AppImage"; do
        [ -n "$p" ] && [ -x "$p" ] && { printf '%s' "$p"; return 0; }
    done
    return 1
}

ai_fetch() {  # ai_fetch <文件名> <url> -> 下载到 tools/
    local name="$1" url="$2"
    mkdir -p "$PROJECT_DIR/tools"
    info "下载 $name ..."
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 2 -o "$PROJECT_DIR/tools/$name" "$url" || return 1
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$PROJECT_DIR/tools/$name" "$url" || return 1
    else
        return 1
    fi
    chmod +x "$PROJECT_DIR/tools/$name"
}

pkg_appimage() {
    step "打包 AppImage"
    local ld ad
    if ! ld="$(ai_tool linuxdeploy)"; then
        if ai_fetch "linuxdeploy-$AI_ARCH.AppImage" \
            "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$AI_ARCH.AppImage"; then
            ld="$PROJECT_DIR/tools/linuxdeploy-$AI_ARCH.AppImage"
        else
            warn "缺少 linuxdeploy 且下载失败，跳过 AppImage"
            return 0
        fi
    fi
    if ! ad="$(ai_tool appimagetool)"; then
        if ai_fetch "appimagetool-$AI_ARCH.AppImage" \
            "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$AI_ARCH.AppImage"; then
            ad="$PROJECT_DIR/tools/appimagetool-$AI_ARCH.AppImage"
        else
            warn "缺少 appimagetool 且下载失败，跳过 AppImage"
            return 0
        fi
    fi

    local appdir="$WORK_DIR/AppDir"
    rm -rf "$appdir"; mkdir -p "$appdir"
    populate_root "$appdir" "$appdir/usr/bin"
    cp -f "$appdir/usr/share/applications/$APP_NAME.desktop" "$appdir/$APP_NAME.desktop"
    [ -f "$appdir/usr/share/icons/hicolor/512x512/apps/$APP_NAME.png" ] \
        && cp -f "$appdir/usr/share/icons/hicolor/512x512/apps/$APP_NAME.png" "$appdir/$APP_NAME.png"

    # 让 linuxdeploy 能找到 Qt 的 qmake
    export PATH="$(dirname "$QMAKE"):$PATH"
    # 没有 FUSE 的环境（容器/CI）也能跑 AppImage 工具
    export APPIMAGE_EXTRACT_AND_RUN=1

    info "收集 Qt 依赖（linuxdeploy）..."
    local qtplugin="$PROJECT_DIR/tools/linuxdeploy-plugin-qt-$AI_ARCH.AppImage"
    if [ ! -x "$qtplugin" ]; then
        ai_fetch "linuxdeploy-plugin-qt-$AI_ARCH.AppImage" \
            "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$AI_ARCH.AppImage" >/dev/null 2>&1 || true
    fi
    if [ -x "$qtplugin" ]; then
        export PATH="$(dirname "$qtplugin"):$PATH"
        if ! "$ld" --appdir "$appdir" --plugin qt --output appimage >"$WORK_DIR/ai.log" 2>&1; then
            tail -20 "$WORK_DIR/ai.log" >&2
            warn "linuxdeploy 执行失败（日志: $WORK_DIR/ai.log）"
        fi
    else
        "$ld" --appdir "$appdir" >"$WORK_DIR/ai.log" 2>&1 || warn "linuxdeploy 执行失败"
    fi

    local out="$OUT_DIR/${APP_NAME}-${VERSION}-${AI_ARCH}.AppImage"
    if ARCH="$AI_ARCH" "$ad" "$appdir" "$out" >"$WORK_DIR/ai2.log" 2>&1; then
        chmod +x "$out"
        ok "AppImage -> $out"
    else
        tail -20 "$WORK_DIR/ai2.log" >&2
        warn "appimagetool 失败（日志: $WORK_DIR/ai2.log）"
    fi
}

# ================================================================== macOS
pkg_mac() {
    step "打包 macOS（dmg + zip）"
    [ -n "$APP_BUNDLE" ] || die "没找到 .app 包（macOS 上需要先构建）"

    local app="$WORK_DIR/$APP_NAME.app"
    rm -rf "$app"
    cp -R "$APP_BUNDLE" "$app"

    # 图标文件先拷进去（Info.plist 的键后面统一写，见下）
    if [ -f "$ICON_ICNS" ]; then
        mkdir -p "$app/Contents/Resources"
        cp -f "$ICON_ICNS" "$app/Contents/Resources/$APP_NAME.icns"
    fi

    # 把 Qt 框架/插件打进包里
    # 注意：必须在写 Info.plist 之前执行 —— macdeployqt 会重写 Info.plist，
    # 先写的信息会被它抹掉（LSUIElement 之前就是这么丢的）。
    if command -v macdeployqt >/dev/null 2>&1; then
        info "打包 Qt 依赖（macdeployqt）..."
        macdeployqt "$app" -always-overwrite >"$WORK_DIR/macdeployqt.log" 2>&1 \
            && ok "Qt 依赖已内置" \
            || warn "macdeployqt 有警告（日志: $WORK_DIR/macdeployqt.log）"
    else
        warn "缺少 macdeployqt，产物将依赖系统里已安装的 Qt"
    fi

    # ---- Info.plist 统一在这里写（macdeployqt 之后）----
    local pb=/usr/libexec/PlistBuddy
    local plist="$app/Contents/Info.plist"

    # 图标
    if [ -f "$ICON_ICNS" ]; then
        $pb -c "Set :CFBundleIconFile $APP_NAME" "$plist" 2>/dev/null \
            || $pb -c "Add :CFBundleIconFile string $APP_NAME" "$plist"
        ok "已注入应用图标"
    else
        warn "缺少 ${ICON_ICNS}，跳过图标"
    fi

    # 版本号
    for kv in "CFBundleShortVersionString:$VERSION" "CFBundleVersion:$VERSION"; do
        local k="${kv%%:*}" v="${kv#*:}"
        $pb -c "Set :$k $v" "$plist" 2>/dev/null || $pb -c "Add :$k string $v" "$plist"
    done
    $pb -c "Set :CFBundleIdentifier com.popball2.app" "$plist" 2>/dev/null \
        || $pb -c "Add :CFBundleIdentifier string com.popball2.app" "$plist"

    # 桌面挂件不应出现在 Dock 和 Cmd+Tab 里。
    # 运行时已经用 NSApplicationActivationPolicyAccessory 处理，这里再加一层 Info.plist 的
    # LSUIElement，保证「双击 .app 启动」时同样没有 Dock 图标。
    $pb -c "Set :LSUIElement 1" "$plist" 2>/dev/null \
        || $pb -c "Add :LSUIElement integer 1" "$plist"
    ok "已写入 Info.plist（图标/版本/LSUIElement）"

    # 临时签名（本机与内网分发够用；上架需换成自己的开发者证书）
    if codesign --force --deep --sign - "$app" >/dev/null 2>&1; then
        ok "已完成临时（ad-hoc）签名"
    else
        warn "ad-hoc 签名失败，首次打开可能需要右键->打开 绕过 Gatekeeper"
    fi

    # 确认 Qt 已随包内置（macdeployqt 之后才校验，才有意义）
    check_mac_bundle "$app/Contents/MacOS/$APP_NAME"

    # zip（先在本地临时盘生成，再拷贝到输出目录）
    local zipname="${APP_NAME}-${VERSION}-macos-${MAC_ARCH}.zip"
    rm -f "$WORK_DIR/$zipname"
    ditto -c -k --sequesterRsrc --keepParent "$app" "$WORK_DIR/$zipname"
    mv -f "$WORK_DIR/$zipname" "$OUT_DIR/$zipname"
    ok "zip  -> $OUT_DIR/$zipname"

    # dmg（带 Applications 快捷方式，方便拖拽安装）
    local vol="$WORK_DIR/dmgroot"
    rm -rf "$vol"; mkdir -p "$vol"
    cp -R "$app" "$vol/"
    ln -s /Applications "$vol/Applications"
    local dmgname="${APP_NAME}-${VERSION}-macos-${MAC_ARCH}.dmg"
    rm -f "$WORK_DIR/$dmgname"
    if hdiutil create -volname "$APP_NAME $VERSION" -srcfolder "$vol" -ov -format UDZO "$WORK_DIR/$dmgname" >/dev/null; then
        mv -f "$WORK_DIR/$dmgname" "$OUT_DIR/$dmgname"
        ok "dmg  -> $OUT_DIR/$dmgname"
    else
        warn "hdiutil 创建 dmg 失败"
    fi
}

# ---------------------------------------------------------------- 分发执行
for t in "${TARGETS[@]}"; do
    case "$t" in
        deb)      pkg_deb ;;
        rpm)      pkg_rpm ;;
        appimage) pkg_appimage ;;
        mac)      pkg_mac ;;
    esac
done

# 中间目录的清理由 trap cleanup_work 负责

# ---------------------------------------------------------------- 汇总
step "产物汇总"
if [ -n "$(ls -A "$OUT_DIR" 2>/dev/null || true)" ]; then
    ls -lh "$OUT_DIR" | awk 'NR>1 {printf "    %-52s %s\n", $9, $5}'
else
    warn "输出目录为空（可能所有目标都因缺少工具被跳过）"
fi
printf '\n'
