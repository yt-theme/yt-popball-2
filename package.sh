#!/usr/bin/env bash
#
# popball2 —— 打包脚本（平台感知 + Docker 多架构 + 容错）
#
# 平台与方式：
#   • macOS 宿主：
#       - macOS 包（.dmg / .zip）：本机编译，真实可分发。
#       - Linux 包（.deb）      ：用 Docker 容器构建 x86_64(amd64) 与 aarch64(arm64)
#                                 两种架构，真实可分发。
#   • Linux 宿主：
#       - 不打包 macOS。
#       - Linux 包（.deb）      ：默认用 Docker 构建 x86_64 与 aarch64 两种架构；
#                                 没有 Docker 或 --no-docker 时，退回本机架构的原生构建
#                                 （仅单架构，无法跨架构）。
#   • Windows 宿主（Git Bash / MSYS2 / Cygwin）：
#       - Windows 包（zip）     ：本机编译 + windeployqt 内置 Qt 运行库，真实可分发。
#
# 容错：每个打包目标（mac / linux-amd64-deb / linux-arm64-deb / 其它）都在独立子 shell 中
#       执行；任一目标失败都会被记录并跳过，不会中断其它目标；最后统一汇总并给出退出码。
#       —— 因此本脚本【不使用 set -e】，错误由各 stage 自行捕获，run_stage 决定是否跳过。
#
# 用法与示例见 ./package.sh --help
#
set -o pipefail

# ---------------------------------------------------------------- 基本路径
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
APP_NAME="popball2"
PRO_FILE="$PROJECT_DIR/${APP_NAME}.pro"
PKG_FILE="$PROJECT_DIR/package.json"
ICON_PNG="$PROJECT_DIR/resources/${APP_NAME}.png"
ICON_ICNS="$PROJECT_DIR/resources/${APP_NAME}.icns"
DOCKERFILE="$SCRIPT_DIR/docker/linux-build/Dockerfile"
DOCKER_CTX="$SCRIPT_DIR/docker/linux-build"
[ -f "$PRO_FILE" ] || { echo "错误: 找不到 ${APP_NAME}.pro" >&2; exit 1; }

# ---------------------------------------------------------------- 默认参数
VERSION=""
OUT_DIR="$PROJECT_DIR/dist"
PREFIX="/usr"
ARCH_OVERRIDE=""
JOBS=""
DO_BUILD=1
KEEP_STAGE=0
NO_DOCKER=0
FORCE_DOCKER=0
NATIVE_ONLY=0
FORCE_CROSS=0
LINUX_ARCHES_ARG=""
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
die()  { err "$*"; exit 1; }   # 仅在顶层用于“无法继续”的致命错误；stage 内也可调用（仅退出该子 shell）

# ---------------------------------------------------------------- 容错执行单元
# run_stage <名字> <命令...>  ：在独立子 shell（启用 set -e）中执行目标命令。
#   成功 -> 记入 SUCCEEDED；失败（含 die/exit 1）-> 记入 FAILED 并继续，绝不中断脚本。
SUCCEEDED=()
FAILED=()
SKIPPED=()   # 因「当前平台不支持」而跳过的目标 —— 这不是失败，不应影响退出码
run_stage() {
    local name="$1"; shift
    step "开始: $name"
    if ( set -e; "$@" ); then
        ok "$name 完成"
        SUCCEEDED+=("$name")
        return 0
    fi
    local rc=$?
    err "$name 失败（exit=$rc），已跳过，继续其它目标"
    FAILED+=("$name")
    return 1
}

# 因「当前平台不支持」而跳过某目标：记入 SKIPPED（不是失败），并给出清晰提示
skip_target() {   # <名字> <原因>
    SKIPPED+=("$1")
    warn "$1：当前平台（$OS_KIND）不支持，已跳过 —— $2"
}

# 列表包含判断：in_list <元素> <列表...>
in_list() {
    local needle="$1"; shift
    local x
    for x in "$@"; do [ "$x" = "$needle" ] && return 0; done
    return 1
}

usage() {
    cat <<EOF
${C_B}popball2 打包脚本（平台感知 + Docker 多架构 + 容错）${C_R}

用法: ./package.sh [目标...] [选项]

目标（可多选，缺省 = 平台默认）:
  deb         Debian/Ubuntu 的 .deb（经 Docker 打 x86_64 + arm64 两种架构）
  deb:x64     只要 x86_64 的 .deb
  deb:arm     只要 arm64 的 .deb
  rpm         Fedora/RHEL/openSUSE 的 .rpm（经 Docker 打 x86_64 + arm64 两种架构）
  rpm:x64     只要 x86_64 的 .rpm
  rpm:arm     只要 arm64 的 .rpm
  appimage    通用 Linux AppImage（本机架构；跨设备请用显式变体）
  appimage:x64 x86_64 AppImage（须在 x86_64 机器上构建；linuxdeploy 不能跨架构）
  appimage:arm arm64 AppImage（须在 arm64 机器上构建；linuxdeploy 不能跨架构）
  mac         macOS 的 .dmg 与 .zip（仅 macOS 宿主）
  win         Windows 的 .zip（仅 Windows 宿主，windeployqt 内置 Qt 运行库）
  all         当前平台支持的全部
  all:x64     全部 x86_64 变体（deb:x64 + rpm:x64 + appimage:x64 + mac）
  all:arm     全部 arm64 变体  （deb:arm + rpm:arm + appimage:arm + mac）

缺省目标:
  macOS 宿主 : mac deb rpm appimage （mac 本机 + Linux deb/rpm/appimage 经 Docker）
  Linux 宿主 : deb rpm appimage     （x86_64 + arm64，默认经 Docker）
  Windows 宿主: win                  （本机 zip，windeployqt 内置 Qt 运行库）

选项:
      --version V    版本号           (默认: 读取 .pro 里的 VERSION)
      --arch ARCH    目标架构         (默认: 本机架构；仅影响原生构建)
      --linux-arch L 用 Docker 打的 Linux 架构，逗号分隔
                      (默认: amd64,arm64；可选 amd64/arm64；
                       同义写法: x86/x64/x86_64=amd64, arm/aarch64=arm64)
      --out DIR      产物输出目录     (默认: <项目>/dist)
      --prefix DIR   deb/rpm 安装前缀 (默认: /usr)
      --no-build     复用已有构建产物，不重新编译
      --jobs N       并行编译任务数
      --keep-stage   保留中间打包目录（排错用）
      --docker       强制用 Docker 打包 Linux 包（无 Docker 则跳过 Linux 目标）
      --no-docker    禁用 Docker，Linux 包退回本机架构原生构建（macOS 上则跳过 Linux）
      --native-only  只打包本机平台的格式（macOS 即只打 mac；Linux 即只打本机架构 deb）
      --skip-platform-check
                     跳过平台校验（仅兼容旧调用，建议用 --no-docker）
  -h, --help         显示帮助

容错说明:
  任一目标（mac / linux-amd64-deb / linux-arm64-deb / linux-amd64-rpm / linux-arm64-rpm / appimage）失败都不会中断脚本，
  失败目标会被记录并跳过，其余目标继续执行；最后汇总并给出退出码（有失败则 exit 1）。

示例:
  ./package.sh                          # macOS: mac(本机) + deb/rpm/appimage(x86_64,arm64 经 Docker)
  ./package.sh                          # Linux : deb/rpm/appimage(x86_64,arm64 经 Docker)
  ./package.sh --linux-arch arm         # 只要 arm64 架构的 .deb / .rpm
  ./package.sh mac --version 1.2.0
  ./package.sh deb --no-docker          # 不用 Docker，仅本机架构原生 deb
  ./package.sh rpm:x64                   # 只要 x86_64 的 .rpm（经 Docker）
  ./package.sh rpm:arm                   # 只要 arm64 的 .rpm（经 Docker）
  ./package.sh appimage                 # 通用 AppImage（macOS 上经 Docker，本机架构）
  ./package.sh appimage:x64              # x86_64 AppImage（须在 x86_64 机器上跑）
  ./package.sh appimage:arm              # arm64 AppImage（须在 arm64 机器上跑）
  ./package.sh all:x64                    # 全套 x64（deb/rpm/appimage:x64 + mac）
  ./package.sh all:arm                    # 全套 arm（deb/rpm/appimage:arm + mac）
  ./package.sh all --out ~/pkgs
EOF
}

# ---------------------------------------------------------------- 参数解析
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)        usage; exit 0 ;;
        --version)        VERSION="${2:?--version 需要一个参数}"; shift ;;
        --arch)           ARCH_OVERRIDE="${2:?--arch 需要一个参数}"; shift ;;
        --linux-arch)     LINUX_ARCHES_ARG="${2:?--linux-arch 需要一个参数}"; shift ;;
        --out)            OUT_DIR="${2:?--out 需要一个参数}"; shift ;;
        --prefix)         PREFIX="${2:?--prefix 需要一个参数}"; shift ;;
        --jobs)           JOBS="${2:?--jobs 需要一个参数}"; shift ;;
        --no-build)       DO_BUILD=0 ;;
        --keep-stage)     KEEP_STAGE=1 ;;
        --docker)         FORCE_DOCKER=1 ;;
        --no-docker)      NO_DOCKER=1 ;;
        --native-only)    NATIVE_ONLY=1 ;;
        --skip-platform-check) FORCE_CROSS=1 ;;
        deb|rpm|appimage|mac|win|all) TARGETS+=("$1") ;;
        appimage:x64|appimage-x64) TARGETS+=("appimage"); ARCH_OVERRIDE="amd64" ;;
        appimage:arm|appimage-arm) TARGETS+=("appimage"); ARCH_OVERRIDE="arm64" ;;
        rpm:x64|rpm-x64) TARGETS+=("rpm"); LINUX_ARCHES_ARG="amd64" ;;
        rpm:arm|rpm-arm) TARGETS+=("rpm"); LINUX_ARCHES_ARG="arm64" ;;
        all:x64|all-x64) TARGETS+=("all"); LINUX_ARCHES_ARG="amd64"; ARCH_OVERRIDE="amd64" ;;
        all:arm|all-arm) TARGETS+=("all"); LINUX_ARCHES_ARG="arm64"; ARCH_OVERRIDE="arm64" ;;
        *) die "未知参数或目标: ${1}（用 --help 查看用法）" ;;
    esac
    shift
done

# ---------------------------------------------------------------- 环境识别
# Windows 的 Git Bash / MSYS / Cygwin 下 uname 会返回 MINGW64_NT / MSYS_NT / CYGWIN_NT
case "$(uname -s)" in
    Linux)     OS_KIND=linux ;;
    Darwin)    OS_KIND=macos ;;
    MINGW*|MSYS*|MSYS_NT*|CYGWIN*) OS_KIND=windows ;;
    *)         die "不支持的平台: $(uname -s)（本脚本支持 Linux、macOS 与 Windows 的 Git Bash/MSYS2）" ;;
esac

# ---------------------------------------------------------------- 平台支持矩阵
# 「本平台支持哪些打包目标」在这里集中定义，供「缺省目标 / all / 不支持提示」统一使用，
# 避免各处写死列表而互相漂移。（是否用 Docker、工具是否就绪属于运行期条件，由各目标自行再判断。）
host_supported_targets() {
    if [ "$OS_KIND" = macos ]; then
        printf '%s\n' mac deb rpm appimage
    elif [ "$OS_KIND" = windows ]; then
        printf '%s\n' win
    else
        printf '%s\n' deb rpm appimage
    fi
}
# 目标在本平台是否受支持（受支持 -> 0）
platform_supports_target() {
    local t="$1" x
    for x in $(host_supported_targets); do [ "$x" = "$t" ] && return 0; done
    return 1
}

# 读 package.json 里的 version（单一版本号来源，改一处即全局生效）
[ -n "$VERSION" ] || VERSION="$(sed -n 's/^  *"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$PKG_FILE" 2>/dev/null | head -1)"
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

# 本机架构对应的 deb 架构（原生回退时用）
HOST_DEBARCH="$MACH"
case "$HOST_DEBARCH" in
    x86_64|amd64) HOST_DEBARCH=amd64 ;;
    arm64|aarch64) HOST_DEBARCH=arm64 ;;
esac

# 缺省目标 / all：都取「当前平台支持的全部格式」（见上面的平台支持矩阵），不写死
if [ ${#TARGETS[@]} -eq 0 ]; then
    while IFS= read -r _t; do TARGETS+=("$_t"); done < <(host_supported_targets)
fi
# all 展开
EXPANDED=()
for t in "${TARGETS[@]}"; do
    if [ "$t" = all ]; then
        while IFS= read -r _t; do EXPANDED+=("$_t"); done < <(host_supported_targets)
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

# ---------------------------------------------------------------- Docker 决策
# 是否用 Docker 打 Linux 包：
#   - 显式禁用(--no-docker / --native-only) -> 否
#   - 已在容器内 / 无 docker 命令 / docker daemon 不可用 -> 否
#   - 其余（macOS 与 Linux 宿主，只要 docker 可用）-> 是
use_docker_for_linux() {
    [ "$NO_DOCKER" -eq 0 ] || return 1
    [ "$NATIVE_ONLY" -eq 0 ] || return 1
    [ -f /.dockerenv ] && return 1
    [ -n "${POPBALL2_IN_DOCKER:-}" ] && return 1
    command -v docker >/dev/null 2>&1 || return 1
    docker info >/dev/null 2>&1 || return 1
    return 0
}

# ---------------------------------------------------------------- 输出目录
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

printf '%s%s %s%s\n' "$C_B" "$APP_NAME" "$VERSION" "$C_R"
info "平台    : $OS_KIND ($MACH)"
info "打包目标: ${TARGETS[*]}"
info "输出目录: $OUT_DIR"

# ---------------------------------------------------------------- Docker 辅助
# 用 docker run 把项目挂进 Linux 容器，在容器内真正编译并打包。
# 参考 build-docker-rpm.sh / build-docker-linux.sh 的多架构循环：每个架构各起一个容器。

# 确保 Docker 守护进程可用（未运行给出友好提示）。返回 0 可用 / 1 不可用，不主动退出脚本。
ensure_docker() {
    if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
        ok "Docker 已就绪 ($(docker --version | awk '{print $3}' | tr -d ', '))"
        return 0
    fi
    if command -v docker >/dev/null 2>&1; then
        if [ "$(uname -s)" = Darwin ]; then
            err "检测到 docker 命令，但 Docker 守护进程未运行。请打开 Docker Desktop（等菜单栏鲸鱼图标稳定）后再运行本脚本。"
            return 1
        fi
        sudo systemctl start docker >/dev/null 2>&1 || true
        if docker info >/dev/null 2>&1; then ok "Docker 守护进程已启动"; return 0; fi
        err "Docker 守护进程未运行，请执行 'sudo systemctl start docker' 后重跑。"
        return 1
    fi
    if [ "$(uname -s)" = Darwin ]; then
        err "未检测到 Docker。请先安装并启动 Docker Desktop：https://www.docker.com/products/docker-desktop/"
    else
        err "未检测到 Docker。请先安装 Docker：https://docs.docker.com/get-docker/"
    fi
    return 1
}

# 判断路径是否处于 Docker Desktop（macOS）默认共享范围内
# （/Users、/tmp 等可直接挂载；U 盘 /Volumes/... 需先 tar 到 /tmp）
path_is_docker_shared() {
    [ "$(uname -s)" = Darwin ] || return 0   # Linux 本地 daemon 始终可挂载
    case "$1" in
        /Users/*|/tmp/*|/private/tmp/*|/private/var/folders/*) return 0 ;;
        *) return 1 ;;
    esac
}

# 跨架构构建前确保 QEMU binfmt 可用（Apple Silicon 打 x86 / x64 打 arm 时需要）
ensure_qemu() {
    local plat="$1"; local host_m; host_m="$(uname -m)"
    local host_plat=""
    case "$host_m" in
        x86_64|amd64)  host_plat=linux/amd64 ;;
        arm64|aarch64) host_plat=linux/arm64 ;;
    esac
    [ -n "$host_plat" ] && [ "$host_plat" = "$plat" ] && return 0   # 同架构无需模拟
    info "跨架构构建 ${plat}（本机 ${host_m}），需 QEMU 模拟"
    if docker run --rm --platform "$plat" --pull=always mplatform/mquery >/dev/null 2>&1; then
        ok "目标架构模拟可用"; return 0
    fi
    warn "未检测到 $plat 模拟支持，尝试注册 QEMU binfmt..."
    if docker run --privileged --rm tonistiigi/binfmt --install all >/dev/null 2>&1; then
        ok "QEMU binfmt 已注册"
    else
        warn "自动注册失败：请手动执行  docker run --privileged --rm tonistiigi/binfmt --install all"
    fi
}

# ---------------------------------------------------------------- 查找 qmake（本机构建用）
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

# ---------------------------------------------------------------- 产物完整性自检
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

check_mac_bundle() {
    local bin="${1:-$APP_BIN}"
    [ "$OS_KIND" = macos ] || return 0
    [ -x "$bin" ] || return 0
    command -v otool >/dev/null 2>&1 || return 0
    local bad
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
BUILD_DIR="${POPBALL2_BUILD_DIR:-$PROJECT_DIR/build-pkg}"
WORK_DIR=""
APP_BIN=""
APP_BUNDLE=""

build_app() {
    # Windows 的 MinGW 用 mingw32-make；其它平台用 make
    local make_tool="${MAKE:-make}"
    if [ "$OS_KIND" = windows ] && command -v mingw32-make >/dev/null 2>&1; then
        make_tool=mingw32-make
    fi

    step "构建 $APP_NAME ${VERSION}（release）"
    mkdir -p "$BUILD_DIR"
    ( cd "$BUILD_DIR" && "$QMAKE" "$PRO_FILE" PREFIX="$PREFIX" >/dev/null )
    local log="$BUILD_DIR/pkg-build.log"
    if ! ( cd "$BUILD_DIR" && "$make_tool" -j"$JOBS" ) >"$log" 2>&1; then
        tail -40 "$log" >&2
        die "编译失败，完整日志: $log"
    fi
    ok "编译完成"
}

locate_app() {
    # Windows（MSVC 会放在 release/ 子目录；MinGW 就在根目录）
    if [ "$OS_KIND" = windows ]; then
        if [ -x "$BUILD_DIR/$APP_NAME.exe" ]; then
            APP_BIN="$BUILD_DIR/$APP_NAME.exe"
        elif [ -x "$BUILD_DIR/release/$APP_NAME.exe" ]; then
            APP_BIN="$BUILD_DIR/release/$APP_NAME.exe"
        elif [ -x "$BUILD_DIR/debug/$APP_NAME.exe" ]; then
            APP_BIN="$BUILD_DIR/debug/$APP_NAME.exe"
        else
            die "找不到构建产物（先去掉 --no-build 跑一次）"
        fi
        return
    fi
    if [ -x "$BUILD_DIR/$APP_NAME.app/Contents/MacOS/$APP_NAME" ]; then
        APP_BUNDLE="$BUILD_DIR/$APP_NAME.app"
        APP_BIN="$APP_BUNDLE/Contents/MacOS/$APP_NAME"
    elif [ -x "$BUILD_DIR/$APP_NAME" ]; then
        APP_BIN="$BUILD_DIR/$APP_NAME"
    else
        die "找不到构建产物（先去掉 --no-build 跑一次）"
    fi
}

# 中间产物目录，尽量放本地临时盘（避免 U 盘 / 网络盘上 macdeployqt/rpmbuild 极慢）。
# 编译产物仍在项目里，方便增量编译。
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
Depends: libc6, libqt6core6 | libqt6core6t64, libqt6gui6 | libqt6gui6t64, libqt6widgets6 | libqt6widgets6t64, libqt6sql6 | libqt6sql6t64, libqt6multimedia6 | libqt6multimedia6t64
Recommends: libqt6sql6-sqlite, gstreamer1.0-plugins-base, gstreamer1.0-plugins-good
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

    local payload="$top/SOURCES/${APP_NAME}-${VERSION}"
    mkdir -p "$payload"
    populate_root "$payload/.pkgroot" "$payload/.pkgroot/usr/bin"
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
ai_tool() {
    local n="$1" p
    for p in "$(command -v "$n" 2>/dev/null || true)" "$PROJECT_DIR/tools/$n" "$PROJECT_DIR/tools/$n-$AI_ARCH.AppImage"; do
        [ -n "$p" ] && [ -x "$p" ] && { printf '%s' "$p"; return 0; }
    done
    return 1
}

ai_fetch() {
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

    export PATH="$(dirname "$QMAKE"):$PATH"
    export APPIMAGE_EXTRACT_AND_RUN=1

    # linuxdeploy-plugin-qt 内部调用的是 `qmake`（不是 qmake6）。若系统用 qtchooser，
    # /usr/bin/qmake 会指向对 Qt6 失效的包装脚本（报 "could not find a Qt installation of ''"），
    # 导致 Qt 依赖收集失败、打出的 AppImage 缺 Qt 库。这里造一个 qmake 垫片指向 qmake6。
    if [ -n "${QMAKE:-}" ]; then
        local qshim="$WORK_DIR/qmake-shim"
        mkdir -p "$qshim"
        ln -sf "$QMAKE" "$qshim/qmake"
        export PATH="$qshim:$PATH"
        export QMAKE="$qshim/qmake"
    fi

    info "收集 Qt 依赖（linuxdeploy）..."
    local qtplugin="$PROJECT_DIR/tools/linuxdeploy-plugin-qt-$AI_ARCH.AppImage"
    if [ ! -x "$qtplugin" ]; then
        ai_fetch "linuxdeploy-plugin-qt-$AI_ARCH.AppImage" \
            "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$AI_ARCH.AppImage" >/dev/null 2>&1 || true
    fi
    if [ -x "$qtplugin" ]; then
        export PATH="$(dirname "$qtplugin"):$PATH"
        # 只用 linuxdeploy 收集依赖（含 Qt 插件），打包交给下面带 --runtime-file 的 appimagetool。
        # 这里不再传 --output appimage —— 那会触发内置的 appimage 插件去联网下 runtime，白跑一趟且会报错。
        if ! "$ld" --appdir "$appdir" --plugin qt >"$WORK_DIR/ai.log" 2>&1; then
            tail -20 "$WORK_DIR/ai.log" >&2
            warn "linuxdeploy 执行失败（日志: $WORK_DIR/ai.log）"
        fi
    else
        "$ld" --appdir "$appdir" >"$WORK_DIR/ai.log" 2>&1 || warn "linuxdeploy 执行失败"
    fi

    # appimagetool 需要 type2 runtime；默认它会临时去 GitHub 下载（离线/受限网络会失败）。
    # 优先用 tools/runtime-<arch>（本仓已预置）；没有就尝试下载；仍没有则交回 appimagetool 自己处理。
    local rt="$PROJECT_DIR/tools/runtime-$AI_ARCH"
    if [ ! -f "$rt" ]; then
        ai_fetch "runtime-$AI_ARCH" \
            "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-$AI_ARCH" >/dev/null 2>&1 || true
    fi
    local rt_arg=()
    [ -f "$rt" ] && rt_arg=(--runtime-file "$rt")

    local out="$OUT_DIR/${APP_NAME}-${VERSION}-${AI_ARCH}.AppImage"
    if ARCH="$AI_ARCH" "$ad" "${rt_arg[@]}" "$appdir" "$out" >"$WORK_DIR/ai2.log" 2>&1; then
        chmod +x "$out"
        ok "AppImage -> $out"
    else
        tail -20 "$WORK_DIR/ai2.log" >&2
        warn "appimagetool 失败（日志: $WORK_DIR/ai2.log）"
        return 1
    fi
}

# ================================================================== macOS
pkg_mac() {
    step "打包 macOS（dmg + zip）"
    [ -n "$APP_BUNDLE" ] || die "没找到 .app 包（macOS 上需要先构建）"

    local app="$WORK_DIR/$APP_NAME.app"
    rm -rf "$app"
    cp -R "$APP_BUNDLE" "$app"

    if [ -f "$ICON_ICNS" ]; then
        mkdir -p "$app/Contents/Resources"
        cp -f "$ICON_ICNS" "$app/Contents/Resources/$APP_NAME.icns"
    fi

    if command -v macdeployqt >/dev/null 2>&1; then
        info "打包 Qt 依赖（macdeployqt）..."
        macdeployqt "$app" -always-overwrite >"$WORK_DIR/macdeployqt.log" 2>&1 \
            && ok "Qt 依赖已内置" \
            || warn "macdeployqt 有警告（日志: $WORK_DIR/macdeployqt.log）"
    else
        warn "缺少 macdeployqt，产物将依赖系统里已安装的 Qt"
    fi

    local pb=/usr/libexec/PlistBuddy
    local plist="$app/Contents/Info.plist"

    if [ -f "$ICON_ICNS" ]; then
        $pb -c "Set :CFBundleIconFile $APP_NAME" "$plist" 2>/dev/null \
            || $pb -c "Add :CFBundleIconFile string $APP_NAME" "$plist"
        ok "已注入应用图标"
    else
        warn "缺少 ${ICON_ICNS}，跳过图标"
    fi

    for kv in "CFBundleShortVersionString:$VERSION" "CFBundleVersion:$VERSION"; do
        local k="${kv%%:*}" v="${kv#*:}"
        $pb -c "Set :$k $v" "$plist" 2>/dev/null || $pb -c "Add :$k string $v" "$plist"
    done
    $pb -c "Set :CFBundleIdentifier com.popball2.app" "$plist" 2>/dev/null \
        || $pb -c "Add :CFBundleIdentifier string com.popball2.app" "$plist"

    $pb -c "Set :LSUIElement 1" "$plist" 2>/dev/null \
        || $pb -c "Add :LSUIElement integer 1" "$plist"
    ok "已写入 Info.plist（图标/版本/LSUIElement）"

    if codesign --force --deep --sign - "$app" >/dev/null 2>&1; then
        ok "已完成临时（ad-hoc）签名"
    else
        warn "ad-hoc 签名失败，首次打开可能需要右键->打开 绕过 Gatekeeper"
    fi

    check_mac_bundle "$app/Contents/MacOS/$APP_NAME"

    local zipname="${APP_NAME}-${VERSION}-macos-${MAC_ARCH}.zip"
    rm -f "$WORK_DIR/$zipname"
    ditto -c -k --sequesterRsrc --keepParent "$app" "$WORK_DIR/$zipname"
    mv -f "$WORK_DIR/$zipname" "$OUT_DIR/$zipname"
    ok "zip  -> $OUT_DIR/$zipname"

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

# ================================================================== Windows
pkg_windows() {
    step "打包 Windows（windeployqt + zip）"
    [ -n "$APP_BIN" ] && [ -f "$APP_BIN" ] || { die "找不到构建产物（先构建）"; }

    local bundle="$WORK_DIR/$APP_NAME"
    rm -rf "$bundle"; mkdir -p "$bundle"
    cp -f "$APP_BIN" "$bundle/$(basename "$APP_BIN")"
    if [ -f "$ICON_PNG" ]; then
        cp -f "$ICON_PNG" "$bundle/$APP_NAME.png"
    fi

    # windeployqt：把 Qt 运行库/DLL/插件打进目录，产物脱离开发机也能跑
    local wdq=""
    if wdq="$(command -v windeployqt)"; then
        info "内置 Qt 运行库（windeployqt）..."
        "$wdq" --no-translations --release "$bundle/$(basename "$APP_BIN")" \
                >"$WORK_DIR/windeployqt.log" 2>&1 \
            && ok "Qt 依赖已内置" \
            || warn "windeployqt 有警告（日志: $WORK_DIR/windeployqt.log）"
    else
        warn "缺少 windeployqt（请把 Qt 的 bin 目录加入 PATH），产物将依赖系统已装的 Qt"
    fi

    # 附带简体中文翻译（应用已内嵌 Qt 翻译，这里再补一份 qtbase 的翻译给 Qt 控件用）
    if [ -n "$QMAKE" ]; then
        local qtdir="$(dirname "$QMAKE")/../translations"
        if [ -d "$qtdir" ]; then
            mkdir -p "$bundle/translations"
            cp -f "$qtdir"/qtbase_zh_CN.qm "$bundle/translations/" 2>/dev/null || true
        fi
    fi

    local winarch="$MACH"
    case "$MACH" in
        x86_64|amd64) winarch=x86_64 ;;
        aarch64|arm64) winarch=arm64 ;;
    esac
    local zname="${APP_NAME}-${VERSION}-windows-${winarch}.zip"

    # 打包 zip：优先用真实 zip；Windows 上用 PowerShell Compress-Archive（Git Bash 常无 zip）
    local w_out="$OUT_DIR"
    command -v cygpath >/dev/null 2>&1 \
        && w_out="$(cygpath -w "$OUT_DIR" 2>/dev/null || echo "$OUT_DIR")"
    rm -f "$OUT_DIR/$zname"
    if [ "$OS_KIND" = windows ] && command -v powershell >/dev/null 2>&1; then
        ( cd "$WORK_DIR" \
            && powershell -NoProfile -Command \
                "Compress-Archive -Path '$APP_NAME' -DestinationPath '$w_out\\$zname' -Force" )
    elif command -v zip >/dev/null 2>&1; then
        ( cd "$WORK_DIR" && zip -r -q "$OUT_DIR/$zname" "$APP_NAME" )
    else
        warn "缺少 zip 且无法用 PowerShell 压缩，跳过 Windows 包生成"
        return 1
    fi

    if [ -f "$OUT_DIR/$zname" ]; then
        ok "win zip -> $OUT_DIR/$zname"
    else
        warn "未能生成 $zname"
        return 1
    fi
}

# ---------------------------------------------------------------- 各 stage（均可能失败，由 run_stage 捕获）
stage_mac() {
    find_qmake || { err "找不到 qmake6，请先运行 ./run.sh 安装开发环境"; return 1; }
    if [ -z "$JOBS" ]; then
        if command -v nproc >/dev/null 2>&1; then JOBS="$(nproc)"; else JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"; fi
    fi
    BUILD_DIR="${POPBALL2_BUILD_DIR:-$PROJECT_DIR/build-pkg}"
    if [ "$DO_BUILD" -eq 1 ]; then build_app; fi
    locate_app
    info "可执行文件: $APP_BIN"
    pkg_mac
}

stage_win() {
    find_qmake || { err "找不到 qmake6，请先安装 Qt 并把其 bin 目录加入 PATH（MinGW 或 MSVC 均可）"; return 1; }
    if [ -z "$JOBS" ]; then
        if command -v nproc >/dev/null 2>&1; then JOBS="$(nproc)"; else JOBS=4; fi
    fi
    BUILD_DIR="${POPBALL2_BUILD_DIR:-$PROJECT_DIR/build-pkg}"
    if [ "$DO_BUILD" -eq 1 ]; then build_app; fi
    locate_app
    info "可执行文件: $APP_BIN"
    pkg_windows
}

stage_native_linux() {
    local fmt="$1"
    find_qmake || { err "找不到 qmake6，请先运行 ./run.sh 安装开发环境"; return 1; }
    if [ -z "$JOBS" ]; then
        if command -v nproc >/dev/null 2>&1; then JOBS="$(nproc)"; else JOBS=4; fi
    fi
    BUILD_DIR="${POPBALL2_BUILD_DIR:-$PROJECT_DIR/build-pkg}"
    if [ "$DO_BUILD" -eq 1 ]; then build_app; fi
    locate_app
    check_linux_deps
    case "$fmt" in
        deb)      pkg_deb ;;
        rpm)      pkg_rpm ;;
        appimage) pkg_appimage ;;
    esac
}

# 在 Docker 容器里为指定架构构建并打出 .deb（真实可分发）
# 兜底：守护进程拉不到「目标架构」基础镜像时，在本机架构的构建镜像里交叉编译目标架构 .deb。
# 仅当 目标架构 != 本机架构 且 本机架构镜像已缓存 时可用。
cross_build_deb() {
    local arch_label="$1" debarch="$2"
    local host_arch native_img native_plat
    host_arch="$(uname -m)"
    case "$host_arch" in
        arm64|aarch64) native_img="popball2-linux-build:arm64"; native_plat="linux/arm64" ;;
        x86_64)        native_img="popball2-linux-build:amd64"; native_plat="linux/amd64" ;;
        *) err "无法判断本机架构用于交叉编译: $host_arch"; return 1 ;;
    esac
    # 若存在预置交叉工具链镜像（已装好目标架构 Qt6/交叉编译器），优先用它：免去每次重下 ~145MB，且不怕下载抖动
    if docker image inspect "${native_img}-cross" >/dev/null 2>&1; then
        native_img="${native_img}-cross"
    fi
    if [ "$native_img" = "popball2-linux-build:${debarch}" ]; then
        err "交叉编译目标架构与本机架构相同，无需交叉"
        return 1
    fi
    if ! docker image inspect "$native_img" >/dev/null 2>&1; then
        err "缺少本机架构构建镜像 $native_img，无法交叉编译 $debarch（请先成功构建本机架构 deb）"
        return 1
    fi
    step "改用交叉编译：在 $native_img 内为 $debarch 编译并打 .deb"
    _rewp() { local p="$1"; [ -z "$p" ] && return 0; echo "$p" | sed -E 's#(https?://)127\.0\.0\.1:#\1host.docker.internal:#'; }
    local _hp _hs; _hp="$(_rewp "${HTTP_PROXY:-}")"; _hs="$(_rewp "${HTTPS_PROXY:-}")"
    local proxy_args=()
    case "${HTTP_PROXY:-}${HTTPS_PROXY:-}" in *127.0.0.1*|*localhost*) : ;; *)
        [ -n "$_hp" ] && proxy_args+=(-e HTTP_PROXY="$_hp"  -e http_proxy="$_hp")
        [ -n "$_hs" ] && proxy_args+=(-e HTTPS_PROXY="$_hs" -e https_proxy="$_hs")
    esac
    # 始终用 /tmp 暂存（不挂载真实工程），避免交叉编译产物污染源码树
    local stage outstage
    stage="$(mktemp -d "/tmp/${APP_NAME}-x.XXXXXX")"
    outstage="$(mktemp -d "/tmp/${APP_NAME}-x-out.XXXXXX")"
    tar -C "$PROJECT_DIR" --exclude=build-pkg --exclude=build --exclude='*.o' \
        --exclude=dist --exclude=.git -cf - . | tar -C "$stage" -xf -
    # 持久化 apt 缓存与索引：交叉编译需下载 ~145MB 目标架构包，缓存后重跑/重试无需重下
    if ! docker run --rm --platform "$native_plat" --add-host=host.docker.internal:host-gateway \
        -v "$stage:/project:rw" -v "$outstage:/out:rw" \
        -v popball2-aptcache:/var/cache/apt/archives -v popball2-aptlists:/var/lib/apt/lists \
        -e POPBALL2_IN_DOCKER=1 -e NO_COLOR=1 "${proxy_args[@]}" \
        "$native_img" bash -lc "bash /project/docker/linux-build/cross-build.sh /project /out ${VERSION}"; then
        rm -rf "$stage" "$outstage"
        err "交叉编译失败 ($debarch)"
        return 1
    fi
    cp -f "$outstage"/* "$OUT_DIR"/ 2>/dev/null || true
    rm -rf "$stage" "$outstage"
    ok "Linux ($arch_label / $debarch) deb（交叉编译）已写入 $OUT_DIR"
}

docker_build_deb() {
    local arch_label="$1"
    local plat debarch img
    case "$arch_label" in
        amd64|x86_64|x86|x64) plat=linux/amd64; debarch=amd64; img="popball2-linux-build:amd64" ;;
        arm64|aarch64|arm)    plat=linux/arm64; debarch=arm64; img="popball2-linux-build:arm64" ;;
        *) err "不支持的 Linux 架构: ${arch_label}"; return 1 ;;
    esac

    ensure_qemu "$plat" || true
    ensure_docker || { err "Docker 不可用，无法构建 ${arch_label} deb"; return 1; }

    # 把宿主机的 127.0.0.1 代理改写为 host.docker.internal（Docker Desktop 的宿主机网关）
    local _hp _hs _orig_hp="${HTTP_PROXY:-}" _orig_hs="${HTTPS_PROXY:-}"
    _rewrite_proxy() { local p="$1"; [ -z "$p" ] && return 0; echo "$p" | sed -E 's#(https?://)127\.0\.0\.1:#\1host.docker.internal:#'; }
    _hp="$(_rewrite_proxy "$_orig_hp")"; _hs="$(_rewrite_proxy "$_orig_hs")"
    # 回环代理（127.0.0.1 / localhost）在容器内指向容器自己，永远不可达，绝不注入；
    # 这种情况让容器直接联网（前提是宿主 Docker 网络本身可联网）。
    local _loopback_proxy=0
    case "$_orig_hp$_orig_hs" in *127.0.0.1*|*localhost*) _loopback_proxy=1 ;; esac
    local build_proxy_args=()
    if [ "$_loopback_proxy" -eq 0 ]; then
        [ -n "$_hp" ] && build_proxy_args+=(--build-arg "MY_HTTP_PROXY=$_hp" --build-arg "MY_HTTPS_PROXY=$_hs")
    fi

    # 镜像按架构分别构建并缓存（首跑装 Qt6，稍慢）
    if ! docker image inspect "$img" >/dev/null 2>&1; then
        step "首次构建 Docker 镜像 ${img}（下载 ubuntu:22.04 并安装 Qt6，请稍候）"
        if ! docker build --platform "$plat" -t "$img" -f "$DOCKERFILE" "$DOCKER_CTX" "${build_proxy_args[@]}"; then
            warn "Docker 镜像构建失败（${arch_label}）：守护进程无法拉取基础镜像 ubuntu:22.04（常见于代理只放行 HTTP / 主机白名单）。"
            warn "尝试改用交叉编译兜底（在本机架构已缓存的镜像内为目标架构编译 + 打 .deb）..."
            if cross_build_deb "$arch_label" "$debarch"; then
                return 0
            fi
            err "镜像构建与交叉编译均失败（${arch_label}）。最常见原因：Docker 守护进程拉取基础镜像 ubuntu:22.04 失败。"
            err "  • 守护进程自己的代理/registry-mirror 坏了（常表现为 127.0.0.1:0，连 registry-1.docker.io:443 不通）。"
            err "  • 容器内 apt 走的是 HTTP，而拉镜像走 HTTPS；若本机代理只支持 HTTP 不支持 HTTPS，镜像拉取会卡死。"
            err "解决办法（任选其一，再重跑本目标）："
            err "  1) Docker Desktop → 设置 → Proxies / Docker Engine 的 registry-mirrors，改成能正常出 HTTPS 的代理或留空；"
            err "  2) 在能联网的机器上先缓存基础镜像：docker pull --platform linux/${arch_label} ubuntu:22.04，再拷到本机；"
            err "  3) 直接在 x86_64 本机跑 ./package.sh（无需跨架构，最快出 amd64 包）。"
            return 1
        fi
    fi

    # 容器内 package.sh 参数：--no-docker 防止递归；--arch 指定该架构
    local pa=()
    [ -n "$VERSION" ] && pa+=(--version "$VERSION")
    [ -n "$JOBS" ]    && pa+=(--jobs "$JOBS")
    pa+=(--no-docker --arch "$debarch" deb --out /project/dist)
    [ "$KEEP_STAGE" -eq 1 ] && pa+=(--keep-stage)

    local proxy_args=()
    if [ "$_loopback_proxy" -eq 0 ]; then
        [ -n "$_hp" ] && proxy_args+=(-e HTTP_PROXY="$_hp"  -e http_proxy="$_hp")
        [ -n "$_hs" ] && proxy_args+=(-e HTTPS_PROXY="$_hs" -e https_proxy="$_hs")
        [ -n "${NO_PROXY:-}" ] && proxy_args+=(-e NO_PROXY="$NO_PROXY" -e no_proxy="$NO_PROXY")
    fi

    if path_is_docker_shared "$PROJECT_DIR"; then
        # 直接挂载项目：容器内编译与产物直写 $PROJECT_DIR/dist（无需回拷）
        if ! docker run --rm --platform "$plat" \
            -v "$PROJECT_DIR:/project:rw" -w /project \
            -e POPBALL2_BUILD_DIR=/build -e POPBALL2_IN_DOCKER=1 -e NO_COLOR=1 "${proxy_args[@]}" \
            "$img" bash -lc "cd /project && ./package.sh ${pa[*]}"; then
            err "Docker 内打包失败 ($arch_label)。详见上方容器日志；多因镜像/代理或源码挂载问题。"
            return 1
        fi
    else
        # 项目在 Docker 默认不共享的卷（如 U 盘）：先 tar 到 /tmp 再挂载
        warn "项目不在 Docker 共享路径（/Users、/tmp），改用 /tmp 暂存方式挂载"
        local stage outstage
        stage="$(mktemp -d "/tmp/${APP_NAME}-docker.XXXXXX")"
        outstage="$(mktemp -d "/tmp/${APP_NAME}-docker-out.XXXXXX")"
        tar -C "$PROJECT_DIR" --exclude=build-pkg --exclude=build --exclude='*.o' \
            --exclude=dist --exclude=.git -cf - . | tar -C "$stage" -xf -
        pa+=(--out /out)
        if ! docker run --rm --platform "$plat" \
            -v "$stage:/project:ro" -v "$outstage:/out:rw" \
            -e POPBALL2_BUILD_DIR=/build -e POPBALL2_IN_DOCKER=1 -e NO_COLOR=1 "${proxy_args[@]}" \
            "$img" bash -lc "cd /project && ./package.sh ${pa[*]}"; then
            rm -rf "$stage" "$outstage"
            err "Docker 内打包失败 ($arch_label)。"
            return 1
        fi
        cp -f "$outstage"/* "$OUT_DIR"/ 2>/dev/null || true
        rm -rf "$stage" "$outstage"
    fi
    ok "Linux ($arch_label / $debarch) deb 已写入 $OUT_DIR"
}

# 在 Docker 容器里构建 rpm（与 deb 同样的多架构模式；镜像复用 deb 的构建镜像）。
docker_build_rpm() {
    local arch_label="$1"
    local plat rpmarch img
    case "$arch_label" in
        amd64|x86_64|x86|x64) plat=linux/amd64; rpmarch=x86_64;  img="popball2-linux-build:amd64" ;;
        arm64|aarch64|arm)    plat=linux/arm64; rpmarch=aarch64; img="popball2-linux-build:arm64" ;;
        *) err "不支持的 Linux 架构: ${arch_label}"; return 1 ;;
    esac

    ensure_qemu "$plat" || true
    ensure_docker || { err "Docker 不可用，无法构建 ${arch_label} rpm"; return 1; }

    # 把宿主机的 127.0.0.1 代理改写为 host.docker.internal（Docker Desktop 的宿主机网关）
    local _hp _hs _orig_hp="${HTTP_PROXY:-}" _orig_hs="${HTTPS_PROXY:-}"
    _rewrite_proxy() { local p="$1"; [ -z "$p" ] && return 0; echo "$p" | sed -E 's#(https?://)127\.0\.0\.1:#\1host.docker.internal:#'; }
    _hp="$(_rewrite_proxy "$_orig_hp")"; _hs="$(_rewrite_proxy "$_orig_hs")"
    local _loopback_proxy=0
    case "$_orig_hp$_orig_hs" in *127.0.0.1*|*localhost*) _loopback_proxy=1 ;; esac
    local build_proxy_args=()
    if [ "$_loopback_proxy" -eq 0 ]; then
        [ -n "$_hp" ] && build_proxy_args+=(--build-arg "MY_HTTP_PROXY=$_hp" --build-arg "MY_HTTPS_PROXY=$_hs")
    fi

    # 复用 deb 的构建镜像（同一套 Qt6 环境）
    if ! docker image inspect "$img" >/dev/null 2>&1; then
        step "首次构建 Docker 镜像 ${img}（下载 ubuntu:22.04 并安装 Qt6，请稍候）"
        if ! docker build --platform "$plat" -t "$img" -f "$DOCKERFILE" "$DOCKER_CTX" "${build_proxy_args[@]}"; then
            err "Docker 镜像构建失败（${arch_label}）。rpm 与 deb 共用构建镜像，先成功构建一次本机架构的 deb 即可自动建好镜像。"
            return 1
        fi
    fi

    # 容器内 package.sh 参数：--no-docker 防止递归；--arch 指定该架构
    local pa=()
    [ -n "$VERSION" ] && pa+=(--version "$VERSION")
    [ -n "$JOBS" ]    && pa+=(--jobs "$JOBS")
    pa+=(--no-docker --arch "$arch_label" rpm --out /project/dist)
    [ "$KEEP_STAGE" -eq 1 ] && pa+=(--keep-stage)

    local proxy_args=()
    if [ "$_loopback_proxy" -eq 0 ]; then
        [ -n "$_hp" ] && proxy_args+=(-e HTTP_PROXY="$_hp"  -e http_proxy="$_hp")
        [ -n "$_hs" ] && proxy_args+=(-e HTTPS_PROXY="$_hs" -e https_proxy="$_hs")
        [ -n "${NO_PROXY:-}" ] && proxy_args+=(-e NO_PROXY="$NO_PROXY" -e no_proxy="$NO_PROXY")
    fi

    if path_is_docker_shared "$PROJECT_DIR"; then
        if ! docker run --rm --platform "$plat" \
            -v "$PROJECT_DIR:/project:rw" -w /project \
            -e POPBALL2_BUILD_DIR=/build -e POPBALL2_IN_DOCKER=1 -e NO_COLOR=1 "${proxy_args[@]}" \
            "$img" bash -lc "cd /project && ./package.sh ${pa[*]}"; then
            err "Docker 内 rpm 打包失败 ($arch_label)。详见上方容器日志。"
            return 1
        fi
    else
        warn "项目不在 Docker 共享路径（/Users、/tmp），改用 /tmp 暂存方式挂载"
        local stage outstage
        stage="$(mktemp -d "/tmp/${APP_NAME}-docker.XXXXXX")"
        outstage="$(mktemp -d "/tmp/${APP_NAME}-docker-out.XXXXXX")"
        tar -C "$PROJECT_DIR" --exclude=build-pkg --exclude=build --exclude='*.o' \
            --exclude=dist --exclude=.git -cf - . | tar -C "$stage" -xf -
        pa+=(--out /out)
        if ! docker run --rm --platform "$plat" \
            -v "$stage:/project:ro" -v "$outstage:/out:rw" \
            -e POPBALL2_BUILD_DIR=/build -e POPBALL2_IN_DOCKER=1 -e NO_COLOR=1 "${proxy_args[@]}" \
            "$img" bash -lc "cd /project && ./package.sh ${pa[*]}"; then
            rm -rf "$stage" "$outstage"
            err "Docker 内 rpm 打包失败 ($arch_label)。"
            return 1
        fi
        cp -f "$outstage"/* "$OUT_DIR"/ 2>/dev/null || true
        rm -rf "$stage" "$outstage"
    fi
    ok "Linux ($arch_label / $rpmarch) rpm 已写入 $OUT_DIR"
}

# 在 Docker 容器里构建 AppImage（原生路径只在 Linux 宿主可用，这里让 macOS 也能出 AppImage）。
# 关键限制：linuxdeploy / appimagetool 是「按架构」的工具，不能跨架构，
# 所以 AppImage 只在「目标架构 == 本机架构」时可构建（本机 arm64 出 aarch64；
# 要 amd64 AppImage 得起在 x86_64 机器上跑）。
docker_build_appimage() {
    local arch_label="$1"
    local plat aiarch img
    case "$arch_label" in
        amd64|x86_64|x86|x64) plat=linux/amd64; aiarch=x86_64;  img="popball2-linux-build:amd64" ;;
        arm64|aarch64|arm)    plat=linux/arm64; aiarch=aarch64; img="popball2-linux-build:arm64" ;;
        *) err "不支持的 AppImage 架构: ${arch_label}"; return 1 ;;
    esac

    ensure_docker || { err "Docker 不可用，无法构建 AppImage"; return 1; }

    local host_arch host_plat
    host_arch="$(uname -m)"
    case "$host_arch" in
        arm64|aarch64) host_plat=linux/arm64 ;;
        x86_64)        host_plat=linux/amd64 ;;
        *)             host_plat="" ;;
    esac
    if [ -z "$host_plat" ] || [ "$plat" != "$host_plat" ]; then
        err "AppImage 要求「目标架构 == 本机架构」（linuxdeploy 不能跨架构）。"
        err "  • 本机是 ${host_arch}，只能在本机出对应架构的 AppImage；"
        err "  • 想要 x86_64/amd64 的 AppImage，请在 x86_64 机器上跑 ./package.sh appimage。"
        return 1
    fi
    if ! docker image inspect "$img" >/dev/null 2>&1; then
        err "缺少 Linux 构建镜像 $img（先成功构建一次本机架构的 deb，或手动构建该镜像）"
        return 1
    fi

    step "在 Docker($img) 内构建 AppImage ($aiarch)"
    local _rewp _hp _hs
    _rewp() { local p="$1"; [ -z "$p" ] && return 0; echo "$p" | sed -E 's#(https?://)127\.0\.0\.1:#\1host.docker.internal:#'; }
    _hp="$(_rewp "${HTTP_PROXY:-}")"; _hs="$(_rewp "${HTTPS_PROXY:-}")"
    local proxy_args=()
    case "${HTTP_PROXY:-}${HTTPS_PROXY:-}" in *127.0.0.1*|*localhost*) : ;; *)
        [ -n "$_hp" ] && proxy_args+=(-e HTTP_PROXY="$_hp"  -e http_proxy="$_hp")
        [ -n "$_hs" ] && proxy_args+=(-e HTTPS_PROXY="$_hs" -e https_proxy="$_hs")
    esac

    # /tmp 暂存（不污染源码树）：容器内构建 + 打包，产物回拷
    local stage rc=0
    stage="$(mktemp -d "/tmp/${APP_NAME}-ai.XXXXXX")"
    tar -C "$PROJECT_DIR" --exclude=build-pkg --exclude=build --exclude='*.o' \
        --exclude=dist --exclude=.git -cf - . | tar -C "$stage" -xf -
    docker run --rm --platform "$plat" --add-host=host.docker.internal:host-gateway \
        -v "$stage:/project:rw" -w /project \
        -e POPBALL2_IN_DOCKER=1 -e NO_COLOR=1 "${proxy_args[@]}" \
        "$img" bash -lc "./package.sh --no-docker --arch ${arch_label} appimage --out /project/dist" || rc=$?
    cp -f "$stage"/dist/*.AppImage "$OUT_DIR"/ 2>/dev/null || true
    rm -rf "$stage"
    if [ "$rc" -ne 0 ]; then
        err "Docker 内 AppImage 打包失败"
        return 1
    fi
    if ls "$OUT_DIR"/*.AppImage >/dev/null 2>&1; then
        ok "AppImage 已写入 $OUT_DIR"
    else
        err "未产出 AppImage"
        return 1
    fi
}

# ================================================================ 目标分发
# 注意：以下每个目标都经 run_stage 执行，单个失败不影响其它目标继续执行。

# ---------- macOS 包（仅 macOS 宿主）----------
if in_list mac "${TARGETS[@]}"; then
    if [ "$OS_KIND" = macos ]; then
        run_stage "macOS 包 (dmg + zip)" stage_mac
    else
        warn "mac 包只能在 macOS 上构建（当前 $OS_KIND），已跳过 mac"
    fi
fi

# ---------- Windows 包（仅 Windows 宿主）----------
if in_list win "${TARGETS[@]}"; then
    if [ "$OS_KIND" = windows ]; then
        run_stage "Windows 包 (zip)" stage_win
    else
        warn "win 包只能在 Windows（Git Bash / MSYS2 / Cygwin）上构建（当前 $OS_KIND），已跳过 win"
    fi
fi

# ---------- Linux deb（Docker 优先；macOS 无 Docker 则跳过；Linux 无 Docker 则原生单架构）----------
if in_list deb "${TARGETS[@]}"; then
    LINUX_ARCHES=()
    if use_docker_for_linux; then
        if [ -n "$LINUX_ARCHES_ARG" ]; then
            IFS=',' read -ra LINUX_ARCHES <<< "$LINUX_ARCHES_ARG"
            norm=()
            for a in "${LINUX_ARCHES[@]}"; do
                case "$a" in
                    x86|x64|amd64|x86_64) norm+=(amd64) ;;
                    arm|arm64|aarch64)    norm+=(arm64) ;;
                    *) norm+=("$a") ;;
                esac
            done
            LINUX_ARCHES=("${norm[@]}")
        else
            LINUX_ARCHES=(amd64 arm64)
        fi
        if docker info >/dev/null 2>&1; then
            for a in "${LINUX_ARCHES[@]}"; do
                run_stage "Linux deb (${a}, Docker)" docker_build_deb "$a"
            done
        elif [ "$OS_KIND" = macos ]; then
            warn "macOS 宿主且无可用 Docker：无法产出 Linux deb，已跳过（请启动 Docker Desktop 后重跑 linux 目标）"
        else
            warn "Docker 不可用：Linux deb 仅构建本机架构 ${HOST_DEBARCH}（需要 x86_64+arm64 请启用 Docker）"
            run_stage "Linux deb (${HOST_DEBARCH}, 原生)" stage_native_linux deb
        fi
    else
        if [ "$OS_KIND" = macos ]; then
            warn "未使用 Docker（--no-docker / --native-only / 容器内）：macOS 宿主无法原生构建 Linux deb，已跳过 Linux 目标"
        else
            warn "未使用 Docker：Linux deb 仅构建本机架构 ${HOST_DEBARCH}"
            run_stage "Linux deb (${HOST_DEBARCH}, 原生)" stage_native_linux deb
        fi
    fi
fi

# ---------- Linux rpm（Docker 优先；macOS 无 Docker 则跳过；Linux 无 Docker 则原生单架构）----------
if in_list rpm "${TARGETS[@]}"; then
    LINUX_ARCHES=()
    if use_docker_for_linux; then
        if [ -n "$LINUX_ARCHES_ARG" ]; then
            IFS=',' read -ra LINUX_ARCHES <<< "$LINUX_ARCHES_ARG"
            norm=()
            for a in "${LINUX_ARCHES[@]}"; do
                case "$a" in
                    x86|x64|amd64|x86_64) norm+=(amd64) ;;
                    arm|arm64|aarch64)    norm+=(arm64) ;;
                    *) norm+=("$a") ;;
                esac
            done
            LINUX_ARCHES=("${norm[@]}")
        else
            LINUX_ARCHES=(amd64 arm64)
        fi
        if docker info >/dev/null 2>&1; then
            for a in "${LINUX_ARCHES[@]}"; do
                run_stage "Linux rpm (${a}, Docker)" docker_build_rpm "$a"
            done
        elif [ "$OS_KIND" = macos ]; then
            warn "macOS 宿主且无可用 Docker：无法产出 Linux rpm，已跳过"
        else
            warn "Docker 不可用：Linux rpm 仅构建本机架构（需要 x86_64+arm64 请启用 Docker）"
            run_stage "Linux rpm (原生)" stage_native_linux rpm
        fi
    else
        if [ "$OS_KIND" = macos ]; then
            warn "未使用 Docker（--no-docker / --native-only / 容器内）：macOS 宿主无法原生构建 Linux rpm，已跳过"
        else
            run_stage "Linux rpm (原生)" stage_native_linux rpm
        fi
    fi
fi

# ---------- Linux AppImage ----------
# Linux 宿主：原生构建（最省事）。
# macOS 宿主：经 Docker 在本机架构的构建镜像里打包（AppImage 目标架构须等于本机架构）。
if in_list appimage "${TARGETS[@]}"; then
    if [ "$OS_KIND" = linux ]; then
        run_stage "Linux appimage (原生)" stage_native_linux appimage
    elif docker info >/dev/null 2>&1; then
        run_stage "Linux appimage (${ARCH_OVERRIDE:-$(uname -m)}, Docker)" \
            docker_build_appimage "${ARCH_OVERRIDE:-$(uname -m)}"
    else
        warn "AppImage 在 macOS 宿主上需要 Docker（未检测到可用 Docker），已跳过 appimage"
    fi
fi

# ---------------------------------------------------------------- 汇总
step "打包汇总"
if [ ${#SUCCEEDED[@]} -gt 0 ]; then
    info "成功 (${#SUCCEEDED[@]}): ${SUCCEEDED[*]}"
fi
if [ ${#FAILED[@]} -gt 0 ]; then
    warn "失败/跳过 (${#FAILED[@]}): ${FAILED[*]}"
fi
if [ -n "$(ls -A "$OUT_DIR" 2>/dev/null || true)" ]; then
    ls -lh "$OUT_DIR" | awk 'NR>1 {printf "    %-52s %s\n", $9, $5}'
else
    warn "输出目录为空（可能所有目标都因缺少工具/环境被跳过）"
fi
info "依赖与安装说明见 DEPENDENCIES.md（deb/rpm 请用包管理器安装以自动补装 Qt6）"

if [ ${#FAILED[@]} -gt 0 ]; then
    warn "部分目标失败，但脚本未中断；请按上方提示修复后重跑对应目标。"
    exit 1
fi
printf '\n%s全部目标打包完成。%s\n' "$C_G" "$C_R"
