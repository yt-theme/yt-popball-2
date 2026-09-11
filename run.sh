#!/usr/bin/env bash
#
# popball2 —— 开发环境安装 / 构建 / 运行脚本
#
#   * 自动识别 Linux 发行版与 macOS，并安装所需的 Qt6 开发环境
#   * 可交互指定构建目录、安装位置、Qt 所在位置
#   * 构建完成后直接运行
#
# 用 ./run.sh --help 查看全部参数。想跳过所有提问用 ./run.sh -y
#
set -euo pipefail

# ---------------------------------------------------------------- 基本路径
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
APP_NAME="popball2"
PRO_FILE="$PROJECT_DIR/${APP_NAME}.pro"

# ---------------------------------------------------------------- 默认参数
BUILD_DIR="$PROJECT_DIR/build"
INSTALL_PREFIX=""
QT_PREFIX=""
JOBS=""
ASSUME_YES=0
SKIP_DEPS=0
DEPS_ONLY=0
DO_INSTALL_PREFIX=0
DO_RUN=1
DO_CLEAN=0

# ---------------------------------------------------------------- 输出样式
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_R=$'\033[0m'; C_B=$'\033[1m'; C_DIM=$'\033[2m'
    C_G=$'\033[32m'; C_Y=$'\033[33m'; C_RD=$'\033[31m'; C_C=$'\033[36m'
else
    C_R=; C_B=; C_DIM=; C_G=; C_Y=; C_RD=; C_C=
fi

step() { printf '\n%s==> %s%s\n' "$C_B$C_C" "$*" "$C_R"; }
info() { printf '    %s\n' "$*"; }
ok()   { printf '    %s✓%s %s\n' "$C_G" "$C_R" "$*"; }
warn() { printf '    %s!%s %s\n' "$C_Y" "$C_R" "$*"; }
err()  { printf '    %s✗ %s%s\n' "$C_RD" "$*" "$C_R" >&2; }
die()  { err "$*"; exit 1; }

# ---------------------------------------------------------------- 交互助手
# 返回值通过 stdout 传出，提示写到 stderr，便于 $(...) 捕获
ask() {
    local prompt="$1" def="${2-}" ans=""
    if [ "$ASSUME_YES" -eq 1 ] || [ ! -t 0 ]; then printf '%s' "$def"; return 0; fi
    if [ -n "$def" ]; then
        printf '    %s [%s]: ' "$prompt" "$def" >&2
    else
        printf '    %s: ' "$prompt" >&2
    fi
    read -r ans || ans=""
    printf '%s' "${ans:-$def}"
}

confirm() {
    local prompt="$1" def="${2:-y}" ans=""
    if [ "$ASSUME_YES" -eq 1 ] || [ ! -t 0 ]; then return 0; fi
    local hint='y/N'; [ "$def" = y ] && hint='Y/n'
    read -r -p "    $prompt [$hint] " ans || true
    ans="${ans:-$def}"
    case "$ans" in [Yy]*) return 0 ;; *) return 1 ;; esac
}

usage() {
    cat <<EOF
${C_B}popball2 运行脚本${C_R}

用法: ./run.sh [选项]

选项:
  -h, --help             显示帮助
  -y, --yes              不交互，全部使用默认值
  -b, --build-dir DIR    构建目录             (默认: <项目>/build)
  -p, --prefix DIR       安装位置/前缀        (默认: /usr/local)
      --qt-prefix DIR    指定 Qt 安装位置     (默认: 自动检测)
      --jobs N           并行编译任务数       (默认: CPU 核心数)
      --deps-only        只安装开发环境后退出
      --no-deps          跳过依赖检查与安装
      --install          构建后执行 make install 到 --prefix
      --no-run           构建后不运行
      --clean            构建前清理构建目录

示例:
  ./run.sh                          # 交互式：装环境 -> 构建 -> 运行
  ./run.sh -y                       # 全部默认值，一路到底
  ./run.sh --qt-prefix ~/Qt/6.8.0 -b ~/tmp/build
  ./run.sh --deps-only              # 只装开发环境
  ./run.sh --no-deps --install -p ~/.local
EOF
}

# ---------------------------------------------------------------- 参数解析
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)      usage; exit 0 ;;
        -y|--yes)       ASSUME_YES=1 ;;
        -b|--build-dir) BUILD_DIR="${2:?--build-dir 需要一个参数}"; shift ;;
        -p|--prefix)    INSTALL_PREFIX="${2:?--prefix 需要一个参数}"; shift ;;
        --qt-prefix)    QT_PREFIX="${2:?--qt-prefix 需要一个参数}"; shift ;;
        --jobs)         JOBS="${2:?--jobs 需要一个参数}"; shift ;;
        --deps-only)    DEPS_ONLY=1 ;;
        --no-deps)      SKIP_DEPS=1 ;;
        --install)      DO_INSTALL_PREFIX=1 ;;
        --no-run)       DO_RUN=0 ;;
        --clean)        DO_CLEAN=1 ;;
        *) die "未知参数: ${1}（用 --help 查看用法）" ;;
    esac
    shift
done

# ---------------------------------------------------------------- 系统识别
OS_KIND=""; FAMILY=""; DISTRO_NAME=""; PKG_MGR=""
detect_os() {
    case "$(uname -s)" in
        Darwin)
            OS_KIND=macos; FAMILY=macos
            DISTRO_NAME="macOS $(sw_vers -productVersion 2>/dev/null || echo '')"
            command -v brew >/dev/null 2>&1 && PKG_MGR=brew
            ;;
        Linux)
            OS_KIND=linux
            local id="" like=""
            if [ -r /etc/os-release ]; then
                # shellcheck disable=SC1091
                . /etc/os-release
                id="${ID:-}"; like="${ID_LIKE:-}"
                DISTRO_NAME="${PRETTY_NAME:-${NAME:-Linux}}"
            fi
            local key=" $id $like "
            # 一个发行版可能同时匹配多个关键字，按"更具体优先"的顺序判断
            case "$key" in
                *alpine*)  FAMILY=alpine ;;
                *void*)    FAMILY=void ;;
                *arch*|*manjaro*|*cachyos*) FAMILY=arch ;;
                *opensuse*|*suse*|*sles*)   FAMILY=suse ;;
                *rhel*|*fedora*|*centos*|*ol*) FAMILY=fedora ;;
                *debian*|*ubuntu*|*mint*|*kali*|*raspbian*|*deepin*) FAMILY=debian ;;
                *)         FAMILY=unknown ;;
            esac
            for m in apt-get dnf yum zypper pacman apk xbps-install; do
                if command -v "$m" >/dev/null 2>&1; then PKG_MGR="$m"; break; fi
            done
            DISTRO_NAME="${DISTRO_NAME:-Linux}${id:+ (id=$id)}"
            ;;
        *) OS_KIND=other; FAMILY=unknown; DISTRO_NAME="$(uname -s)" ;;
    esac
}

# 各发行版的依赖包名。core = 必需，opt = 可选（装不上只警告）
deps_core() {
    case "$1" in
        debian) echo "build-essential qt6-base-dev qt6-base-dev-tools" ;;
        fedora) echo "gcc-c++ make qt6-qtbase-devel" ;;
        suse)   echo "gcc-c++ make qt6-base-devel" ;;
        arch)   echo "base-devel qt6-base" ;;
        alpine) echo "build-base qt6-qtbase-dev" ;;
        void)   echo "base-devel qt6-base-devel" ;;
        macos)  echo "qt" ;;
        *)      echo "" ;;
    esac
}
deps_opt() {
    case "$1" in
        debian) echo "qt6-l10n-tools libgl1-mesa-dev libx11-dev pkg-config" ;;
        fedora) echo "qt6-qttools-devel qt6-linguist mesa-libGL-devel libX11-devel pkgconf-pkg-config" ;;
        suse)   echo "qt6-tools-devel Mesa-libGL-devel libX11-devel pkg-config" ;;
        arch)   echo "qt6-tools" ;;
        alpine) echo "qt6-qttools-dev mesa-dev libx11-dev pkgconf" ;;
        void)   echo "qt6-tools-devel" ;;
        macos)  echo "" ;;
        *)      echo "" ;;
    esac
}

# 用包管理器安装；$1 = 包名列表
pkg_install() {
    local pkgs="$1"
    [ -n "$pkgs" ] || return 0
    case "$PKG_MGR" in
        brew)          brew install $pkgs ;;
        apt-get)       sudo apt-get update && sudo apt-get install -y $pkgs ;;
        dnf|yum)       sudo "$PKG_MGR" install -y $pkgs ;;
        zypper)        sudo zypper --non-interactive install $pkgs ;;
        pacman)        sudo pacman -S --needed --noconfirm $pkgs ;;
        apk)           sudo apk add $pkgs ;;
        xbps-install)  sudo xbps-install -Sy $pkgs ;;
        *)             return 1 ;;
    esac
}

# ---------------------------------------------------------------- 工具查找
QMAKE=""
find_qmake() {
    # 1) 用户显式指定
    if [ -n "$QT_PREFIX" ] && [ -x "$QT_PREFIX/bin/qmake6" ]; then
        QMAKE="$QT_PREFIX/bin/qmake6"; return 0
    fi
    # 2) PATH 里直接有
    local c
    for c in qmake6 qmake-qt6; do
        if command -v "$c" >/dev/null 2>&1; then QMAKE="$(command -v "$c")"; return 0; fi
    done
    # 3) macOS Homebrew
    if [ "$OS_KIND" = macos ] && command -v brew >/dev/null 2>&1; then
        local p
        for p in "$(brew --prefix qt 2>/dev/null)" "$(brew --prefix qt@6 2>/dev/null)"; do
            if [ -n "$p" ] && [ -x "$p/bin/qmake6" ]; then QMAKE="$p/bin/qmake6"; return 0; fi
        done
    fi
    # 4) qmake 但必须确实是 Qt6
    if command -v qmake >/dev/null 2>&1; then
        if qmake -query QT_VERSION 2>/dev/null | grep -q '^6\.'; then
            QMAKE="$(command -v qmake)"; return 0
        fi
        if qmake --version 2>/dev/null | grep -qi 'Qt version 6'; then
            QMAKE="$(command -v qmake)"; return 0
        fi
    fi
    return 1
}

# 缺失依赖的收集（用全局变量 + 计数器，避免 bash 3.2 下空数组展开的坑）
MISSING_COUNT=0
MISSING_NAMES=""
add_missing() {
    if [ -n "$MISSING_NAMES" ]; then MISSING_NAMES="${MISSING_NAMES}|"; fi
    MISSING_NAMES="${MISSING_NAMES}$1"
    MISSING_COUNT=$((MISSING_COUNT + 1))
}

check_toolchain() {
    MISSING_COUNT=0
    MISSING_NAMES=""
    find_qmake || add_missing "qmake6（Qt6 开发包）"
    command -v make >/dev/null 2>&1 || add_missing "make"
    if [ "$OS_KIND" = macos ]; then
        command -v clang++ >/dev/null 2>&1 || add_missing "clang++（Xcode 命令行工具）"
    else
        if ! command -v g++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1; then
            add_missing "g++ / clang++"
        fi
    fi
}

# ---------------------------------------------------------------- 主流程
printf '%s' "$C_B"
cat <<'BANNER'
   ____             ____        _ _   ____
  |  _ \ ___  _ __ | __ )  __ _| | | |___ \
  | |_) / _ \| '_ \|  _ \ / _` | | |   __) |
  |  __/ (_) | |_) | |_) | (_| | | |  / __/
  |_|   \___/| .__/|____/ \__,_|_|_| |_____|
             |_|
BANNER
printf '%s' "$C_R"

step "[1/5] 检测系统环境"
detect_os
info "系统      : $DISTRO_NAME"
info "架构      : $(uname -m)"
info "发行版族  : $FAMILY"
if [ -n "$PKG_MGR" ]; then ok "包管理器  : $PKG_MGR"; else warn "未检测到包管理器，稍后需手动安装依赖"; fi
if [ "$FAMILY" = unknown ] && [ "$OS_KIND" = linux ]; then
    warn "未能识别的发行版，依赖包名可能需要手动调整（可先用 --no-deps 跳过）"
fi

step "[2/5] 检查开发依赖"
# 注意：必须直接调用（不能放进 $(...)），否则 find_qmake 对 QMAKE 的赋值会丢在子 shell 里
check_toolchain

if [ "$SKIP_DEPS" -eq 1 ]; then
    info "已指定 --no-deps，跳过依赖安装"
else
    if [ "$MISSING_COUNT" -eq 0 ]; then
        ok "依赖齐全，无需安装"
        if [ -n "$QMAKE" ]; then
            ok "qmake     : $QMAKE (Qt $("$QMAKE" -query QT_VERSION 2>/dev/null || echo '版本未知'))"
        fi
    else
        warn "缺少以下工具/库："
        printf '%s\n' "$MISSING_NAMES" | tr '|' '\n' | while read -r l; do
            [ -n "$l" ] && printf '        - %s\n' "$l"
        done

        CORE="$(deps_core "$FAMILY")"
        OPT="$(deps_opt "$FAMILY")"
        if [ -n "$CORE$OPT" ]; then
            info "将通过 $PKG_MGR 安装: $CORE $OPT"
        fi

        if [ -z "$PKG_MGR" ]; then
            warn "没有可用的包管理器，请手动安装 Qt6 开发环境后重试"
        elif confirm "是否现在安装这些依赖？(可能需要 sudo 密码)" y; then
            if pkg_install "$CORE"; then
                ok "必需依赖安装完成"
            else
                err "必需依赖安装失败，请查看上方输出"
                die "安装中断。也可以手动装好 Qt6 后加 --no-deps 重试"
            fi
            if [ -n "$OPT" ]; then
                # 可选包装不上不致命（不同发行版包名差异较大）
                if pkg_install "$OPT" 2>/dev/null; then
                    ok "可选依赖安装完成"
                else
                    warn "部分可选依赖未安装（不影响核心构建）"
                fi
            fi
            # 安装完再探测一次，后面还要用 QMAKE
            find_qmake || true
        else
            warn "已跳过依赖安装"
        fi
    fi
fi

if [ "$FAMILY" = macos ]; then
    if ! xcode-select -p >/dev/null 2>&1; then
        warn "未检测到 Xcode 命令行工具，请先执行： xcode-select --install"
    fi
fi

if [ "$DEPS_ONLY" -eq 1 ]; then
    step "完成"
    info "--deps-only：依赖处理完毕，退出"
    exit 0
fi

# ---------------------------------------------------------------- 构建参数
step "[3/5] 配置构建参数"

if [ -z "$INSTALL_PREFIX" ]; then
    if [ "$EUID" -eq 0 ]; then INSTALL_PREFIX=/usr/local; else INSTALL_PREFIX="$HOME/.local"; fi
fi

if [ "$ASSUME_YES" -eq 0 ] && [ -t 0 ]; then
    info "下面三项可回车使用默认值"
    BUILD_DIR="$(ask '构建目录' "$BUILD_DIR")"
    INSTALL_PREFIX="$(ask '安装位置(前缀)' "$INSTALL_PREFIX")"
    local_qt="$(ask 'Qt 安装位置(留空=自动检测)' "$QT_PREFIX")"
    QT_PREFIX="$local_qt"
fi

BUILD_DIR="${BUILD_DIR/#\~/$HOME}"
INSTALL_PREFIX="${INSTALL_PREFIX/#\~/$HOME}"
[ -n "$QT_PREFIX" ] && QT_PREFIX="${QT_PREFIX/#\~/$HOME}"

if [ -n "$QT_PREFIX" ]; then find_qmake || true; fi
[ -n "$QMAKE" ] || die "找不到 qmake6。请指定 --qt-prefix，或先用 --deps-only 安装依赖"

info "构建目录  : $BUILD_DIR"
info "安装位置  : $INSTALL_PREFIX"
info "Qt        : $QMAKE"

step "[4/5] 构建 $APP_NAME"
[ -f "$PRO_FILE" ] || die "找不到工程文件: $PRO_FILE"

if [ "$DO_CLEAN" -eq 1 ] && [ -d "$BUILD_DIR" ]; then
    info "清理构建目录 ..."
    rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

info "qmake ..."
"$QMAKE" "$PRO_FILE" >/dev/null

if [ -z "$JOBS" ]; then
    if command -v nproc >/dev/null 2>&1; then JOBS="$(nproc)"
    elif command -v sysctl >/dev/null 2>&1; then JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    else JOBS=4; fi
fi
info "make -j$JOBS ..."
if ! make -j"$JOBS"; then
    die "编译失败，请查看上方错误信息"
fi
ok "构建完成"

# 定位可执行文件（macOS 是 .app 包，Linux 是普通可执行文件）
APP_BIN=""
if [ -x "$BUILD_DIR/$APP_NAME.app/Contents/MacOS/$APP_NAME" ]; then
    APP_BIN="$BUILD_DIR/$APP_NAME.app/Contents/MacOS/$APP_NAME"
elif [ -x "$BUILD_DIR/$APP_NAME" ]; then
    APP_BIN="$BUILD_DIR/$APP_NAME"
fi
[ -n "$APP_BIN" ] || die "构建产物中找不到可执行文件"
info "可执行文件: $APP_BIN"

if [ "$DO_INSTALL_PREFIX" -eq 1 ]; then
    step "安装到 $INSTALL_PREFIX"
    # qmake 的安装路径在 qmake 阶段就固化进 Makefile 了，所以要带 PREFIX 重新生成一次
    "$QMAKE" "$PRO_FILE" PREFIX="$INSTALL_PREFIX" >/dev/null
    make -j"$JOBS" >/dev/null 2>&1 || true
    if make install INSTALL_ROOT="" >/dev/null 2>&1; then
        ok "make install 完成"
    else
        warn "make install 未生效（通常是目标目录权限问题），改用直接拷贝"
    fi

    if [ "$OS_KIND" = macos ] && [ -d "$BUILD_DIR/$APP_NAME.app" ]; then
        # macOS 必须整包拷贝：只拷可执行文件会丢掉 Info.plist / 图标 / 依赖框架
        mkdir -p "$INSTALL_PREFIX"
        rm -rf "$INSTALL_PREFIX/$APP_NAME.app"
        cp -R "$BUILD_DIR/$APP_NAME.app" "$INSTALL_PREFIX/"
        ok "已安装应用包: $INSTALL_PREFIX/$APP_NAME.app"
    else
        mkdir -p "$INSTALL_PREFIX/bin"
        cp -f "$APP_BIN" "$INSTALL_PREFIX/bin/$APP_NAME"
        ok "已安装可执行文件: $INSTALL_PREFIX/bin/$APP_NAME"
    fi
fi

step "[5/5] 运行"
if [ "$DO_RUN" -eq 0 ]; then
    info "已指定 --no-run，跳过"
else
    info "小球会出现在屏幕右上区域；按 Ctrl+C 结束"
    exec "$APP_BIN"
fi

step "完成"
