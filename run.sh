#!/usr/bin/env bash
#
# popball2 —— 开发环境安装 / 构建 / 运行脚本
#
#   * 自动识别 Linux 发行版、macOS 与 Windows(Git Bash/MSYS2)，并安装所需的 Qt6 开发环境
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

# Windows CPU 温度驱动（放到 exe 旁由程序自动装成内核服务，见 sysInfo.cpp）
WIN_DRIVER_X64="WinRing0x64.sys"
WIN_DRIVER_X86="WinRing0.sys"

# 版本号单一来源：package.json（与 .pro / package.sh 读的是同一处）
APP_VERSION=""
read_app_version() {
    sed -n 's/^  *"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        "$PROJECT_DIR/package.json" 2>/dev/null | head -1
}

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

Windows（Git Bash / MSYS2）说明:
  没有系统包管理器，依赖由 aqtinstall 装到 C:\\Qt（Qt6 的 win64_mingw 套件 + MinGW 编译器），
  构建用 mingw32-make。可用环境变量覆盖：
    POPBALL2_QT_ROOT     Qt 安装根目录（默认 /c/Qt）
    POPBALL2_QT_VERSION  Qt 版本（默认 6.8.3）
    POPBALL2_MINGW_TOOL  MinGW 工具包（默认 tools_mingw1310，对应 Qt 6.8）
    POPBALL2_QT_MIRROR   下载镜像（默认官方 download.qt.io，国内可换
                         https://mirrors.ustc.edu.cn/qtproject/ 等；注意镜像需有顶层
                         Updates.xml，且部分镜像对并发下载限速，反而容易校验失败）
    POPBALL2_QT_TIMEOUT  单次下载超时秒数（默认 120；aqt 自身默认仅 5 秒，易截断）
    POPBALL2_QT_ARCHIVES 归档子集（默认 "qtbase qtsvg qttools qttranslations MinGW"，
                         本项目不需要 QML，故省掉 qtdeclarative；设为空串则装全部）
    POPBALL2_SEVENZIP    指定 7z 可执行文件（默认自动找 7z/7za/7zr，没有就下载 7zr.exe；
                         用外部 7z 解压比 aqt 内置的 py7zr 快很多、也更少出错）
    POPBALL2_PYTHON      Python 解释器（默认自动找 python3/python/py）
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
        MINGW*|MSYS*|MSYS_NT*|CYGWIN*)
            # Windows 的 Git Bash / MSYS2 / Cygwin：没有 apt/dnf 这类系统包管理器，
            # Qt6 与 MinGW 改由 aqtinstall 安装（见 install_windows_deps）
            OS_KIND=windows; FAMILY=windows
            DISTRO_NAME="Windows（$(uname -s) 环境）"
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
        windows) echo "" ;;   # Windows 走 aqtinstall（见 install_windows_deps），不用系统包管理器
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
        windows) echo "" ;;   # MinGW 工具链随 Qt 由 aqtinstall 一起装
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

# ---------------------------------------------------------------- Windows 依赖安装
# Windows（Git Bash / MSYS2）没有系统包管理器，用 aqtinstall 把 Qt6（MinGW 套件）
# 和 MinGW 工具链（g++ / mingw32-make）装到 Qt 根目录，装完自动被 find_* 发现。
# 可用环境变量覆盖：POPBALL2_QT_ROOT(默认 /c/Qt=C:\Qt) POPBALL2_QT_VERSION(默认 6.8.3)
#                  POPBALL2_MINGW_TOOL(默认 tools_mingw1310) POPBALL2_PYTHON
install_windows_deps() {
    local py="" c
    # 顺序：python3 → py（Windows 官方启动器）→ python
    # 注意 Windows 自带一个 App Execution Alias 的假 python.exe（会弹应用商店），
    # 所以每个候选都实跑一次 import sys 验证版本，假的会在这里被淘汰
    for c in "${POPBALL2_PYTHON:-}" python3 py python; do
        [ -n "$c" ] || continue
        command -v "$c" >/dev/null 2>&1 || continue
        if "$c" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 7) else 1)' >/dev/null 2>&1; then
            py="$c"; break
        fi
    done
    if [ -z "$py" ]; then
        err "找不到可用的 Python 3（aqtinstall 需要）"
        info "请先安装 Python 3.7+，或用 POPBALL2_PYTHON 指定解释器路径"
        return 1
    fi

    local root="${POPBALL2_QT_ROOT:-/c/Qt}"
    local ver="${POPBALL2_QT_VERSION:-6.8.3}"
    local tool="${POPBALL2_MINGW_TOOL:-tools_mingw1310}"
    local mirror="${POPBALL2_QT_MIRROR:-}"
    # aqt 默认超时只有 5 秒，网络稍抖就会截断下载（表现为 "checksum error"），这里放宽
    local timeout="${POPBALL2_QT_TIMEOUT:-120}"
    # 只装本项目用得到的归档：.pro 里 core/gui/widgets + sql/svg 守卫，用不到 QML，
    # 省掉 qtdeclarative(约 100MB)，也少一个下载失败点。留空 = 装全部。
    local archives="${POPBALL2_QT_ARCHIVES-qtbase qtsvg qttools qttranslations MinGW}"

    info "Python    : $py"
    info "安装位置  : $root  (默认 C:\\Qt，可用 POPBALL2_QT_ROOT 改)"
    info "将安装    : Qt $ver (win64_mingw) + $tool"
    [ -n "$mirror" ] && info "镜像      : $mirror"

    if ! "$py" -m aqt version >/dev/null 2>&1; then
        info "安装 aqtinstall（pip）..."
        if ! "$py" -m pip install --no-input -U aqtinstall; then
            err "aqtinstall 安装失败（pip 报错见上）"
            return 1
        fi
    fi

    # ---- aqtinstall 的两个已知坑（Windows 上必踩）----
    # 1) 默认 concurrency=4，用 multiprocessing 起多个进程同时往同一个目标目录解压，
    #    Windows 上会互相抢文件/目录，表现为 "Failed to write to base directory ..." 的
    #    PermissionError，且只解压一半（qtbase 的 plugins/*.dll 缺失 → 运行时找不到平台插件）。
    #    改成串行即可稳定通过。
    # 2) 默认走内置的 py7zr 解压，对 qtbase 这种几千条目的归档慢到十几分钟。
    #    有 7z/7zr 时交给它解压，快且稳。
    local cfg=""
    cfg="$("$py" -c 'import os, aqt.helper; print(os.path.join(os.path.dirname(aqt.helper.__file__), "settings.ini"))' 2>/dev/null || true)"
    if [ -n "$cfg" ] && [ -f "$cfg" ] && ! grep -qE '^concurrency[[:space:]]*:[[:space:]]*1[[:space:]]*$' "$cfg"; then
        info "调整 aqt 并发为 1（避免多进程解压互相踩踏）"
        sed -i.bak 's/^concurrency[[:space:]]*:.*/concurrency : 1/' "$cfg" || warn "改写 $cfg 失败，继续尝试"
    fi

    local sevenz="${POPBALL2_SEVENZIP:-}" c
    if [ -z "$sevenz" ]; then
        for c in 7z 7za 7zr; do
            if command -v "$c" >/dev/null 2>&1; then sevenz="$(cygpath -w "$(command -v "$c")" 2>/dev/null || command -v "$c")"; break; fi
        done
    fi
    if [ -z "$sevenz" ]; then
        # 没有就下一个官方的单文件 7zr.exe（约 600KB，只支持 .7z，正好够用）
        local ez="$root/tools/7zr.exe" ez_win=""
        if command -v curl >/dev/null 2>&1; then
            info "下载 7-Zip 命令行（加速解压）..."
            mkdir -p "$root/tools"
            if curl -fsSL --max-time 120 -o "$ez" https://www.7-zip.org/a/7zr.exe; then
                ez_win="$(cygpath -w "$ez" 2>/dev/null || echo "$ez")"
                sevenz="$ez_win"
            else
                warn "下载失败，将使用 aqt 内置解压（较慢）"
            fi
        fi
    fi
    [ -n "$sevenz" ] && info "解压工具  : $sevenz"

    # aqt 是 Windows 程序，路径要给它 Windows 形式（Git Bash 里 /c/Qt -> C:\Qt）
    local root_win
    root_win="$(cygpath -w "$root" 2>/dev/null || echo "$root")"

    # -b 镜像 / --archives 归档子集：参数为空时不传，保持 aqt 默认行为
    local -a common=(-O "$root_win" --timeout "$timeout")
    [ -n "$mirror" ] && common+=(-b "$mirror")
    [ -n "$sevenz" ] && common+=(-E "$sevenz")
    local -a arch_opt=()
    # shellcheck disable=SC2086
    [ -n "$archives" ] && arch_opt=(--archives $archives)

    info "下载 Qt $ver ...（首次数百 MB，视网速可能较久）"
    if ! "$py" -m aqt install-qt windows desktop "$ver" win64_mingw "${common[@]}" "${arch_opt[@]}"; then
        err "Qt 下载或解压失败"
        info "可换镜像重试： POPBALL2_QT_MIRROR=https://mirrors.ustc.edu.cn/qtproject/ ./run.sh"
        return 1
    fi

    info "下载 MinGW 工具链 $tool ..."
    if ! "$py" -m aqt install-tool windows desktop "$tool" "${common[@]}"; then
        err "MinGW 工具链下载或解压失败"
        return 1
    fi

    ok "Qt6 + MinGW 已装到 $root"
    return 0
}

# ---------------------------------------------------------------- 工具查找
QMAKE=""
find_qmake() {
    # 1) 用户显式指定（Linux/macOS 是 bin/qmake6；Windows 是 bin/qmake.exe）
    if [ -n "$QT_PREFIX" ]; then
        local c
        for c in qmake6 qmake.exe; do
            if [ -x "$QT_PREFIX/bin/$c" ]; then QMAKE="$QT_PREFIX/bin/$c"; return 0; fi
        done
        # --qt-prefix 也可能直接指向套件的 bin 目录本身
        for c in qmake6 qmake.exe; do
            if [ -x "$QT_PREFIX/$c" ]; then QMAKE="$QT_PREFIX/$c"; return 0; fi
        done
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
    # 3b) Windows：扫描 Qt 安装根目录下的 <版本>/<套件>/bin/qmake.exe
    #     （默认 C:\Qt，Git Bash 里是 /c/Qt；可用 POPBALL2_QT_ROOT 覆盖）
    #     优先 MinGW 套件（本脚本用 mingw32-make 构建），其次才是 MSVC 等
    if [ "$OS_KIND" = windows ]; then
        local root d
        for root in "${POPBALL2_QT_ROOT:-/c/Qt}" "$HOME/Qt"; do
            [ -d "$root" ] || continue
            for d in $(ls -1d "$root"/*/mingw*/bin 2>/dev/null | sort -Vr); do
                if [ -x "$d/qmake.exe" ]; then QMAKE="$d/qmake.exe"; return 0; fi
            done
            for d in $(ls -1d "$root"/*/*/bin 2>/dev/null | sort -Vr); do
                if [ -x "$d/qmake.exe" ]; then QMAKE="$d/qmake.exe"; return 0; fi
            done
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

# ---------------------------------------------------------------- Windows 工具链
# Windows 上没有 make/g++，构建要靠 Qt 一起装下来的 MinGW：g++ + mingw32-make。
# 注意：即便找到套件也要把它加进 PATH，否则 make 找不到 g++、运行 exe 找不到 Qt DLL。
MINGW_BIN=""
MAKE_TOOL="make"
find_mingw() {
    MINGW_BIN=""
    local d
    # 1) 显式指定的 Qt 前缀下可能带着 Tools/mingwXXX_64
    if [ -n "$QT_PREFIX" ]; then
        for d in "$QT_PREFIX"/Tools/mingw*/bin "$QT_PREFIX"/mingw*/bin; do
            if [ -x "$d/mingw32-make.exe" ] && [ -x "$d/g++.exe" ]; then MINGW_BIN="$d"; return 0; fi
        done
    fi
    # 2) PATH 里已经齐了
    if command -v mingw32-make >/dev/null 2>&1 && command -v g++ >/dev/null 2>&1; then
        MINGW_BIN="$(dirname "$(command -v g++)")"; return 0
    fi
    # 3) Qt 安装根目录自带的 MinGW（aqt install-tool tools_mingwXXXX 装在这里）
    local root
    for root in "${POPBALL2_QT_ROOT:-/c/Qt}" "$HOME/Qt"; do
        [ -d "$root/Tools" ] || continue
        for d in $(ls -1d "$root/Tools"/mingw*/bin 2>/dev/null | sort -Vr); do
            if [ -x "$d/mingw32-make.exe" ] && [ -x "$d/g++.exe" ]; then MINGW_BIN="$d"; return 0; fi
        done
    done
    return 1
}

# 把 Qt 与 MinGW 的 bin 挂到 PATH 最前面（构建期找工具、运行期找 Qt DLL 都要用）
prepend_windows_path() {
    [ "$OS_KIND" = windows ] || return 0
    if [ -n "$MINGW_BIN" ]; then PATH="$MINGW_BIN:$PATH"; fi
    if [ -n "$QMAKE" ]; then PATH="$(dirname "$QMAKE"):$PATH"; fi
    export PATH
}

# Windows 下 qmake / mingw32-make 是原生程序，**参数**必须是 Windows 风格路径。
# Git Bash 不会把 "/c/Users/..." 自动转成 "C:\Users\..."，qmake 会把它当成"当前盘符
# 根目录下的相对路径"，报 Cannot find file: \c\Users\...。所以显式转一次。
to_win_path() {
    if [ "$OS_KIND" = windows ] && command -v cygpath >/dev/null 2>&1; then
        cygpath -w "$1"
    else
        printf '%s' "$1"
    fi
}

# ---------------------------------------------------------------- Windows 运行库部署
# 把 Qt6 运行库（DLL / 平台插件 / 编译器运行库）拷到 exe 旁，让「程序目录」自包含。
# 为什么构建目录也要部署：程序读 CPU 温度需要装内核驱动，非管理员时会经 UAC 重新拉起自己
# （popball2.exe --install-winring0-driver），提权后的子进程是**干净环境**——拿不到这里临时
# 挂进 PATH 的 Qt bin，exe 旁边没有 Qt DLL 就会以 0xC0000135(STATUS_DLL_NOT_FOUND) 秒退，
# 结果驱动装不上、CPU 温度永远读不出来。顺带好处：构建目录双击即可运行，不再依赖开发机 PATH。
find_windeployqt() {
    local c
    if command -v windeployqt >/dev/null 2>&1; then command -v windeployqt; return 0; fi
    if command -v windeployqt.exe >/dev/null 2>&1; then command -v windeployqt.exe; return 0; fi
    if [ -n "$QMAKE" ] && [ -x "$(dirname "$QMAKE")/windeployqt.exe" ]; then
        printf '%s' "$(dirname "$QMAKE")/windeployqt.exe"; return 0
    fi
    if [ -n "$QMAKE" ] && [ -x "$(dirname "$QMAKE")/windeployqt" ]; then
        printf '%s' "$(dirname "$QMAKE")/windeployqt"; return 0
    fi
    return 1
}

deploy_win_runtime() {   # <exe 路径> [日志文件]
    local exe="$1" log="${2:-$BUILD_DIR/qt-deploy.log}" dir wdq exe_win
    dir="$(dirname "$exe")"
    if [ ! -f "$exe" ]; then
        warn "找不到 $exe，跳过 Qt6 运行库部署"
        return 1
    fi
    if ! wdq="$(find_windeployqt)"; then
        warn "找不到 windeployqt（应有 Qt 的 bin 目录在 PATH，或与 qmake 同目录）"
        return 1
    fi
    # windeployqt 是原生 Windows 程序，参数必须是 Windows 路径：传 /tmp/... 会被当成
    # "\tmp\..." 并报 "... does not exist."（Qt 库一个都拷不进去）。
    exe_win="$(to_win_path "$exe")"
    # --no-opengl-sw：纯 QWidget/QPainter 绘制，不用 OpenGL/Quick，省掉 ~18MB 的软件 OpenGL 回退
    "$wdq" --release --no-translations --no-opengl-sw "$exe_win" >"$log" 2>&1 \
        || warn "windeployqt 有警告（日志: $log）"
    # 有没有 Qt6Core.dll 是「能不能跑」的唯一判据
    [ -f "$dir/Qt6Core.dll" ] && return 0
    warn "$dir 里没有 Qt6Core.dll —— Qt6 运行库未部署成功（日志: $log）"
    return 1
}

# 把 WinRing0 驱动拷到 exe 旁（程序按 applicationDirPath() 找它，见 sysInfo.cpp）。
# 架构必须匹配：x64 → WinRing0x64.sys、x86 → WinRing0.sys；ARM Windows 内核不能加载 x64
# 驱动，故 arm64 不拷（程序会自动回退其它温度来源）。
copy_winring0_next_to() {   # <目标目录>
    local dest="$1" arch name drv d
    [ "$OS_KIND" = windows ] || return 0
    case "$(uname -m)" in
        i386|i686) arch=x86 ;;
        aarch64|arm64) warn "ARM Windows 无法加载 WinRing0（x64 内核驱动），不放驱动"; return 0 ;;
        *) arch=x64 ;;
    esac
    if [ "$arch" = x64 ]; then name="$WIN_DRIVER_X64"; else name="$WIN_DRIVER_X86"; fi
    [ -f "$dest/$name" ] && return 0        # 已经在位，避免自我拷贝
    drv=""
    for d in "${POPBALL2_WINRING0_SYS:-}" "$PROJECT_DIR/drivers/$name" \
             "$BUILD_DIR/release/$name" "$BUILD_DIR/$name" "$PROJECT_DIR/$name"; do
        [ -n "$d" ] && [ "$d" != "$dest/$name" ] && [ -f "$d" ] && { drv="$d"; break; }
    done
    if [ -n "$drv" ]; then
        cp -f "$drv" "$dest/$name" && ok "已附带驱动 $name（CPU 温度）"
        return 0
    fi
    warn "没找到 $name —— 程序目录里将没有 CPU 温度驱动"
    info "  可把 $name 放到仓库 drivers/ 目录，或用 POPBALL2_WINRING0_SYS=/path/to/$name 指定"
    return 0
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
    if [ "$OS_KIND" = windows ]; then
        # Windows 只有 qmake.exe + MinGW（没有裸 make/g++），单独判定
        find_qmake || add_missing "qmake（Qt6 开发包）"
        find_mingw  || add_missing "MinGW 工具链（g++ / mingw32-make）"
        return 0
    fi
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
if [ "$OS_KIND" = windows ]; then
    ok "依赖来源  : aqtinstall（Qt6 + MinGW，装到 ${POPBALL2_QT_ROOT:-/c/Qt}）"
elif [ -n "$PKG_MGR" ]; then ok "包管理器  : $PKG_MGR"
else warn "未检测到包管理器，稍后需手动安装依赖"; fi
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
    elif [ "$OS_KIND" = windows ]; then
        warn "缺少以下工具/库："
        printf '%s\n' "$MISSING_NAMES" | tr '|' '\n' | while read -r l; do
            [ -n "$l" ] && printf '        - %s\n' "$l"
        done
        info "Windows 上这些由 aqtinstall 一次装齐（Qt6 的 MinGW 套件 + MinGW 编译器）"
        if confirm "是否现在安装这些依赖？（需要 Python 3 与网络，约 1.5GB）" y; then
            if install_windows_deps; then
                # 装完重新探测，后面的构建要直接用 QMAKE / MINGW_BIN
                find_qmake || true
                find_mingw  || true
                if [ -n "$QMAKE" ] && [ -n "$MINGW_BIN" ]; then
                    ok "依赖已就绪"
                else
                    die "安装完成但仍找不到 Qt/MinGW，请检查上面输出（可用 POPBALL2_QT_ROOT 指定安装位置）"
                fi
            else
                die "安装中断。也可以手动装好 Qt6 + MinGW 后加 --no-deps 重试"
            fi
        else
            warn "已跳过依赖安装"
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

# Windows：把 Qt / MinGW 的 bin 挂到 PATH 最前面 —— make 找 g++、运行 exe 找 Qt DLL 都要它
prepend_windows_path

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
PRO_FILE_ARG="$(to_win_path "$PRO_FILE")"
if [ -z "$APP_VERSION" ]; then APP_VERSION="$(read_app_version)"; fi
# 版本号经环境变量交给 .pro（.pro 的 grep/sed 管道在 Windows 上由 cmd 执行，必然为空 →
# 版本号会落成 0.0.0，Windows exe 的资源版本号也就成了 0.0.0.0）；命令行 VERSION= 再兜一层。
[ -n "$APP_VERSION" ] && export POPBALL2_VERSION="$APP_VERSION"
if [ "$OS_KIND" = windows ]; then
    [ -n "$APP_VERSION" ] && info "版本      : $APP_VERSION"
    "$QMAKE" "$PRO_FILE_ARG" VERSION="$APP_VERSION" >/dev/null
else
    "$QMAKE" "$PRO_FILE_ARG" >/dev/null
fi

if [ -z "$JOBS" ]; then
    if command -v nproc >/dev/null 2>&1; then JOBS="$(nproc)"
    elif command -v sysctl >/dev/null 2>&1; then JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    elif [ -n "${NUMBER_OF_PROCESSORS:-}" ]; then JOBS="$NUMBER_OF_PROCESSORS"
    else JOBS=4; fi
fi
# Windows/MinGW 的 make 叫 mingw32-make
if [ "$OS_KIND" = windows ] && [ -n "$MINGW_BIN" ]; then MAKE_TOOL=mingw32-make; fi
info "$MAKE_TOOL -j$JOBS ..."
if ! "$MAKE_TOOL" -j"$JOBS"; then
    die "编译失败，请查看上方错误信息"
fi
ok "构建完成"

# 定位可执行文件（macOS 是 .app 包，Linux/Windows 是普通可执行文件，Windows 带 .exe）
APP_BIN=""
if [ -x "$BUILD_DIR/$APP_NAME.exe" ]; then                     # Windows / MinGW
    APP_BIN="$BUILD_DIR/$APP_NAME.exe"
elif [ -x "$BUILD_DIR/release/$APP_NAME.exe" ]; then            # Windows / MSVC
    APP_BIN="$BUILD_DIR/release/$APP_NAME.exe"
elif [ -x "$BUILD_DIR/$APP_NAME.app/Contents/MacOS/$APP_NAME" ]; then
    APP_BIN="$BUILD_DIR/$APP_NAME.app/Contents/MacOS/$APP_NAME"
elif [ -x "$BUILD_DIR/$APP_NAME" ]; then
    APP_BIN="$BUILD_DIR/$APP_NAME"
fi
[ -n "$APP_BIN" ] || die "构建产物中找不到可执行文件"
info "可执行文件: $APP_BIN"

# Windows：把 Qt6 运行库与 WinRing0 驱动部署到 exe 旁，让程序目录自包含（详见函数上方注释）。
# 少了 Qt6 DLL，双击 exe 会报「找不到 Qt6Core.dll」；UAC 提权装驱动时也会 0xC0000135 秒退。
if [ "$OS_KIND" = windows ]; then
    info "部署 Qt6 运行库到程序目录 ..."
    if deploy_win_runtime "$APP_BIN" "$BUILD_DIR/qt-deploy.log"; then
        ok "Qt6 运行库已就位: $(dirname "$APP_BIN")"
    else
        warn "Qt6 运行库未部署成功，双击 exe 会报「找不到 Qt6 的 DLL」"
    fi
    copy_winring0_next_to "$(dirname "$APP_BIN")" || true
fi

if [ "$DO_INSTALL_PREFIX" -eq 1 ]; then
    step "安装到 $INSTALL_PREFIX"
    # qmake 的安装路径在 qmake 阶段就固化进 Makefile 了，所以要带 PREFIX 重新生成一次
    if [ "$OS_KIND" = windows ]; then
        PREFIX_ARG="$(to_win_path "$INSTALL_PREFIX")"
        if [ -n "$APP_VERSION" ]; then
            "$QMAKE" "$PRO_FILE_ARG" PREFIX="$PREFIX_ARG" VERSION="$APP_VERSION" >/dev/null
        else
            "$QMAKE" "$PRO_FILE_ARG" PREFIX="$PREFIX_ARG" >/dev/null
        fi
    else
        "$QMAKE" "$PRO_FILE_ARG" PREFIX="$INSTALL_PREFIX" >/dev/null
    fi
    "$MAKE_TOOL" -j"$JOBS" >/dev/null 2>&1 || true
    if "$MAKE_TOOL" install INSTALL_ROOT="" >/dev/null 2>&1; then
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
        # Windows 上可执行文件必须带 .exe，否则双击/命令行都起不来
        BIN_NAME="$APP_NAME"
        [ "$OS_KIND" = windows ] && BIN_NAME="$APP_NAME.exe"
        mkdir -p "$INSTALL_PREFIX/bin"
        cp -f "$APP_BIN" "$INSTALL_PREFIX/bin/$BIN_NAME"
        ok "已安装可执行文件: $INSTALL_PREFIX/bin/$BIN_NAME"
        # Windows 安装目录同样要自包含：Qt6 运行库与 WinRing0 驱动必须跟着 exe 走
        if [ "$OS_KIND" = windows ]; then
            if deploy_win_runtime "$INSTALL_PREFIX/bin/$BIN_NAME" "$BUILD_DIR/qt-deploy-install.log"; then
                ok "Qt6 运行库已部署到 $INSTALL_PREFIX/bin"
            else
                warn "Qt6 运行库未部署到 $INSTALL_PREFIX/bin（运行会报找不到 Qt6 的 DLL）"
            fi
            copy_winring0_next_to "$INSTALL_PREFIX/bin" || true
        fi
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
