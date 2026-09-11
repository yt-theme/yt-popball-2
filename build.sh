#!/usr/bin/env bash
#
# popball2 —— 一键构建并打包
#
#   先编译（release），再调用同目录的 package.sh 打包：
#       deb / rpm / AppImage / macOS(dmg + zip)
#
#   平台与方式：
#     • 在 Linux 上        ：直接用本机构建并打包 deb/rpm/AppImage（真实可分发）。
#     • 在 macOS 上        ：macOS 包用本机构建（真实可分发）；
#                            Linux 包（deb/rpm/AppImage）一律用 Docker 容器构建——
#                            把项目挂进 ubuntu:22.04（已装 Qt6 的镜像）里真正编译并打包，
#                            对每个架构（x86 / arm）各起一个容器，产物真实可分发。
#                            没有 Docker 或显式 --no-docker 时，退回
#                            --skip-platform-check 仅验证打包流程（不可分发）。
#
#   多平台（参考 QrCode_gen 的 build-docker-rpm.sh / build-docker-linux.sh）：
#     Linux 的 x86_64 与 aarch64 两种架构都通过 Docker 打出真包，
#     在 Apple Silicon 上 x86 走 QEMU 模拟、arm 走原生，二者都可用。
#
#   用法与示例见 ./build.sh --help
#
set -euo pipefail

# ---------------------------------------------------------------- 路径
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
PKG="$SCRIPT_DIR/package.sh"
PRO_FILE="$PROJECT_DIR/popball2.pro"
DOCKERFILE="$SCRIPT_DIR/docker/linux-build/Dockerfile"
DOCKER_CTX="$SCRIPT_DIR/docker/linux-build"
[ -f "$PKG" ] || { echo "错误: 找不到同目录下的 package.sh" >&2; exit 1; }

# ---------------------------------------------------------------- 输出样式
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_R=$'\033[0m'; C_B=$'\033[1m'
    C_G=$'\033[32m'; C_Y=$'\033[33m'; C_C=$'\033[36m'
else
    C_R=; C_B=; C_G=; C_Y=; C_C=
fi
step() { printf '\n%s==> %s%s\n' "$C_B$C_C" "$*" "$C_R"; }
ok()   { printf '    %s✓%s %s\n' "$C_G" "$C_R" "$*"; }
warn() { printf '    %s!%s %s\n' "$C_Y" "$C_R" "$*"; }
die()  { printf '    %s✗ %s%s\n' "$C_R" "$*" >&2; exit 1; }
info() { printf '    %s\n' "$*"; }

# ---------------------------------------------------------------- Docker 辅助
# 参考 QrCode_gen 的 build-docker-rpm.sh / build-docker-linux.sh：
# 用 docker run 把项目挂进 Linux 容器，在容器内编译并打包，对每个架构循环一次。

# 确保 Docker 守护进程可用（未运行则给出友好提示；macOS 需手动开 Docker Desktop）
ensure_docker() {
    if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
        ok "Docker 已就绪 ($(docker --version | awk '{print $3}' | tr -d ', '))"
        return 0
    fi
    if command -v docker >/dev/null 2>&1; then
        if [ "$(uname -s)" = Darwin ]; then
            die "检测到 docker 命令，但 Docker 守护进程未运行。请打开 Docker Desktop（等菜单栏鲸鱼图标稳定）后再运行本脚本。"
        fi
        sudo systemctl start docker >/dev/null 2>&1 || true
        if docker info >/dev/null 2>&1; then ok "Docker 守护进程已启动"; return 0; fi
        die "Docker 守护进程未运行，请执行 'sudo systemctl start docker' 后重跑。"
    fi
    if [ "$(uname -s)" = Darwin ]; then
        die "未检测到 Docker。请先安装并启动 Docker Desktop：https://www.docker.com/products/docker-desktop/"
    fi
    die "未检测到 Docker。请先安装 Docker：https://docs.docker.com/get-docker/"
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

usage() {
    cat <<EOF
${C_B}popball2 一键构建并打包${C_R}

用法: ./build.sh [目标...] [选项]

目标（可多选，缺省 = 全部四种: deb rpm appimage mac）:
  deb         Debian/Ubuntu 的 .deb
  rpm         Fedora/RHEL/openSUSE 的 .rpm
  appimage    通用 Linux AppImage
  mac         macOS 的 .dmg 与 .zip
  all         以上全部

选项:
      --version V     版本号          (默认: 读取 .pro 里的 VERSION)
      --arch ARCH     目标架构        (默认: 本机架构；macOS 上仅影响 mac 包)
      --linux-arch L  用 Docker 打的 Linux 架构，逗号分隔
                      (默认: x86,arm；可选 x86/x64/amd64/x86_64 与 arm/arm64/aarch64；
                       x86 即 64 位 Intel/AMD，deb 产物名为 *amd64.deb)
      --out DIR       产物输出目录    (默认: <项目>/dist)
      --jobs N        并行编译任务数
      --no-build      跳过【本机】编译，直接打包（假设已有 build-pkg 产物）
      --keep-stage    保留中间打包目录（排错用）
      --docker        强制用 Docker 打包 Linux 包（无 Docker 则报错）
      --no-docker     禁用 Docker，Linux 包退回只验证打包流程
      --native-only   只打包本机平台的格式（macOS 上即只打 mac）
  -h, --help          显示帮助

说明（在 macOS 上）:
  • macOS 包  : 本机编译，真实可分发（dmg/zip）。
  • Linux 包  : 用 Docker 容器构建（参考 QrCode_gen 的 Docker 打包方式）——
                把项目挂进容器，在 ubuntu:22.04（已装 Qt6）里真正编译并打包
                Linux 的 x86（x86_64 / amd64）和 arm（aarch64）两种架构（真实可分发）。
                对每个架构各起一个容器；产物直接写回 dist/。
                首次会下载并构建镜像（装 Qt6，稍慢，之后缓存）。
                在 Apple Silicon 上 x86 走 QEMU 模拟、arm 走原生，二者都可用；
                若 x86 构建报 'exec format error'，脚本会自动注册 QEMU binfmt。
  • 无 Docker : Linux 目标退回 --skip-platform-check 仅验证流程（不可分发）。
  • 挂载说明 : 项目在 /Users、/tmp 下可直接挂载；在 U 盘（/Volumes）等
                Docker 默认不共享的路径时，脚本自动改为先 tar 到 /tmp 再挂载。

示例:
  ./build.sh                 # macOS: mac(本机) + deb/rpm/appimage(x64,arm 经 Docker)
  ./build.sh --native-only   # 只打本机 macOS 包（dmg/zip，真实可分发）
  ./build.sh deb --linux-arch arm     # 只要 arm 架构的 .deb
  ./build.sh mac --version 1.2.0
EOF
}

# ---------------------------------------------------------------- 参数解析
VERSION=""; ARCH=""; OUT=""; JOBS=""; NO_BUILD=0; KEEP=0; NATIVE_ONLY=0
FORCE_DOCKER=0; NO_DOCKER=0; LINUX_ARCHES_ARG=""
TARGETS=()
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)      usage; exit 0 ;;
        --version)      VERSION="${2:?--version 需要一个参数}"; shift ;;
        --arch)         ARCH="${2:?--arch 需要一个参数}"; shift ;;
        --linux-arch)   LINUX_ARCHES_ARG="${2:?--linux-arch 需要一个参数}"; shift ;;
        --out)          OUT="${2:?--out 需要一个参数}"; shift ;;
        --jobs)         JOBS="${2:?--jobs 需要一个参数}"; shift ;;
        --no-build)     NO_BUILD=1 ;;
        --keep-stage)   KEEP=1 ;;
        --docker)       FORCE_DOCKER=1 ;;
        --no-docker)    NO_DOCKER=1 ;;
        --native-only)  NATIVE_ONLY=1 ;;
        deb|rpm|appimage|mac|all) TARGETS+=("$1") ;;
        *) die "未知参数或目标: ${1}（用 --help 查看用法）" ;;
    esac
    shift
done

# ---------------------------------------------------------------- 平台识别
case "$(uname -s)" in
    Linux)  OS_KIND=linux;  NATIVE=(deb rpm appimage) ;;
    Darwin) OS_KIND=macos;  NATIVE=(mac) ;;
    *)      die "不支持的平台: $(uname -s)（本脚本仅支持 Linux 与 macOS）" ;;
esac

# 缺省目标 = 全部四种
if [ ${#TARGETS[@]} -eq 0 ]; then
    TARGETS=(deb rpm appimage mac)
else
    EXPANDED=()
    for t in "${TARGETS[@]}"; do
        if [ "$t" = all ]; then EXPANDED+=(deb rpm appimage mac); else EXPANDED+=("$t"); fi
    done
    TARGETS=("${EXPANDED[@]}")
    # 去重
    UNIQ=()
    for t in "${TARGETS[@]}"; do
        dup=0
        for u in "${UNIQ[@]+"${UNIQ[@]}"}"; do [ "$u" = "$t" ] && dup=1; done
        [ "$dup" -eq 0 ] && UNIQ+=("$t")
    done
    TARGETS=("${UNIQ[@]}")
fi

# --native-only：把目标裁剪为本机平台能真正产出的格式
if [ "$NATIVE_ONLY" -eq 1 ]; then
    FILT=()
    for t in "${TARGETS[@]}"; do
        for n in "${NATIVE[@]}"; do [ "$n" = "$t" ] && FILT+=("$t"); done
    done
    TARGETS=("${FILT[@]}")
    [ ${#TARGETS[@]} -eq 0 ] && die "本机平台 ($OS_KIND) 没有可打包的原生格式"
fi

# 把目标拆成“本机(原生)”与“Linux(可能走 Docker)”两组
MAC_TARGETS=(); LINUX_TARGETS=()
for t in "${TARGETS[@]}"; do
    case "$t" in
        mac) MAC_TARGETS+=(mac) ;;
        deb|rpm|appimage) LINUX_TARGETS+=("$t") ;;
    esac
done

# 输出目录（提前建好，Docker 挂载也需要它存在）
[ -n "$OUT" ] || OUT="$PROJECT_DIR/dist"
mkdir -p "$OUT"
OUT_DIR="$(cd "$OUT" && pwd)"

# ---------------------------------------------------------------- Docker 决策
# 在 macOS 上打 Linux 包 → 默认用 Docker（多架构真实可分发）；
# 在 Linux 上打 Linux 包 → 本机原生即可（Docker 可选，用于跨架构）。
USE_DOCKER=0
if [ "$OS_KIND" = macos ] && [ ${#LINUX_TARGETS[@]} -gt 0 ]; then
    if [ "$NO_DOCKER" -eq 1 ]; then
        USE_DOCKER=0
    elif [ "$FORCE_DOCKER" -eq 1 ]; then
        ensure_docker
        USE_DOCKER=1
    else
        if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
            USE_DOCKER=1
        else
            warn "本机未检测到可用的 Docker，Linux 包将退回仅验证流程（不可分发）。"
            warn "如需真实多平台包，请启动 Docker Desktop 后重跑（或加 --docker 强制）。"
        fi
    fi
fi

# Linux 架构清单（仅在使用 Docker 时生效）
if [ "$USE_DOCKER" -eq 1 ]; then
    if [ -n "$LINUX_ARCHES_ARG" ]; then
        IFS=',' read -ra LINUX_ARCHES <<< "$LINUX_ARCHES_ARG"
    elif [ -n "$ARCH" ]; then
        LINUX_ARCHES=("$ARCH")
    else
        LINUX_ARCHES=(x86 arm)
    fi
    ensure_docker   # Docker 路径启用时，确保 daemon 可用
fi

printf '%s%s%s\n' "$C_B" "popball2 一键构建并打包" "$C_R"
info_plat() { printf '    %s\n' "$*"; }
info_plat "平台    : $OS_KIND ($(uname -m))"
info_plat "打包目标: ${TARGETS[*]}"
[ ${#MAC_TARGETS[@]} -gt 0 ]   && info_plat "  macOS   : ${MAC_TARGETS[*]}（本机构建）"
[ ${#LINUX_TARGETS[@]} -gt 0 ] && info_plat "  Linux   : ${LINUX_TARGETS[*]} $([ "$USE_DOCKER" -eq 1 ] && echo "（Docker: ${LINUX_ARCHES[*]}）" || echo "（仅验证流程）")"

# ---------------------------------------------------------------- 查找 qmake（本机 macOS 构建用）
find_qmake() {
    local c
    for c in qmake6 qmake-qt6; do
        if command -v "$c" >/dev/null 2>&1; then QMAKE="$(command -v "$c")"; return 0; fi
    done
    if command -v qmake >/dev/null 2>&1 \
       && qmake -query QT_VERSION 2>/dev/null | grep -q '^6\.'; then
        QMAKE="$(command -v qmake)"; return 0
    fi
    return 1
}

# ---------------------------------------------------------------- 本机构建（仅 macOS 目标需要）
if [ "$OS_KIND" = macos ] && [ ${#MAC_TARGETS[@]} -gt 0 ] && [ "$NO_BUILD" -eq 0 ]; then
    step "构建 popball2（macOS release）"
    find_qmake || die "找不到 qmake6，请先运行 ./run.sh 安装开发环境"
    if [ -z "$JOBS" ]; then
        if command -v nproc >/dev/null 2>&1; then JOBS="$(nproc)"
        else JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"; fi
    fi
    BUILD_DIR="$PROJECT_DIR/build-pkg"
    mkdir -p "$BUILD_DIR"
    ( cd "$BUILD_DIR" && "$QMAKE" "$PRO_FILE" PREFIX=/usr >/dev/null ) \
        || die "qmake 失败"
    build_log="$BUILD_DIR/build.sh-build.log"
    if ! ( cd "$BUILD_DIR" && make -j"$JOBS" ) >"$build_log" 2>&1; then
        tail -40 "$build_log" >&2
        die "编译失败，完整日志: $build_log"
    fi
    ok "编译完成 -> $BUILD_DIR"
fi

# ---------------------------------------------------------------- 打包 macOS（本机）
if [ ${#MAC_TARGETS[@]} -gt 0 ]; then
    step "打包 macOS: ${MAC_TARGETS[*]}"
    PKG_ARGS=()
    [ -n "$VERSION" ] && PKG_ARGS+=(--version "$VERSION")
    [ -n "$ARCH" ]    && PKG_ARGS+=(--arch "$ARCH")
    [ -n "$JOBS" ]    && PKG_ARGS+=(--jobs "$JOBS")
    [ "$KEEP" -eq 1 ] && PKG_ARGS+=(--keep-stage)
    PKG_ARGS+=(--out "$OUT_DIR")
    [ "$NO_BUILD" -eq 1 ] && PKG_ARGS+=(--no-build)
    PKG_ARGS+=("${MAC_TARGETS[@]}")
    "$PKG" "${PKG_ARGS[@]}"
fi

# ---------------------------------------------------------------- 打包 Linux
DOCKER_LINUX_FAILED=0
if [ ${#LINUX_TARGETS[@]} -gt 0 ]; then
    if [ "$USE_DOCKER" -eq 1 ]; then
        # 用 Docker 真正编译并打包每个 Linux 架构（x86 / arm），参考 build-docker-rpm.sh 的多架构循环：
        # 对每个架构各起一个容器，在容器内 qmake+make+package.sh，产物直接写回 dist/。
        docker_build_linux() {
            local arch="$1"; shift
            local targets=("$@")
            local plat debarch img
            case "$arch" in
                x86|x64|amd64|x86_64)   plat=linux/amd64; debarch=x86_64;  img="popball2-linux-build:amd64" ;;
                arm|arm64|aarch64)      plat=linux/arm64; debarch=aarch64; img="popball2-linux-build:arm64" ;;
                *) die "不支持的 Linux 架构: ${arch}（可选 x86 / arm；x86 即 x86_64 64 位 Intel/AMD）" ;;
            esac
            # 跨架构（如 Apple Silicon 打 x86）前确保 QEMU 模拟就绪
            ensure_qemu "$plat"
            # 镜像按架构分别构建并缓存（首跑装 Qt6，稍慢）
            if ! docker image inspect "$img" >/dev/null 2>&1; then
                step "首次构建 Docker 镜像 ${img}（下载 ubuntu:22.04 并安装 Qt6，请稍候）"
                docker build --platform "$plat" -t "$img" -f "$DOCKERFILE" "$DOCKER_CTX" \
                    || { warn "Docker 镜像构建失败（${arch}）：通常是 Docker 的镜像仓库镜像/代理配置有问题（如某个 registry-mirror 指向 127.0.0.1 不可用）。请在 Docker Desktop 的 设置→Docker Engine 里检查 registry-mirrors，或 设置→Proxies 关闭/修正代理后重跑。"; DOCKER_LINUX_FAILED=1; return 1; }
            fi

            # package.sh 参数：版本 / jobs / 架构 / 输出目录 / 目标 / 保留中间目录
            local pa=()
            [ -n "$VERSION" ] && pa+=(--version "$VERSION")
            [ -n "$JOBS" ]    && pa+=(--jobs "$JOBS")
            pa+=(--arch "$debarch")
            [ "$KEEP" -eq 1 ] && pa+=(--keep-stage)
            pa+=("${targets[@]}")

            step "Docker 构建并打包 Linux ($arch / $debarch): ${targets[*]}"
            if path_is_docker_shared "$PROJECT_DIR"; then
                # 直接挂载项目：容器内的编译与产物直写 $PROJECT_DIR/dist（无需回拷）
                pa+=(--out /project/dist)
                docker run --rm --platform "$plat" \
                    -v "$PROJECT_DIR:/project:rw" -w /project \
                    -e POPBALL2_BUILD_DIR=/build -e NO_COLOR=1 \
                    "$img" bash -lc "cd /project && ./package.sh ${pa[*]}" \
                    || { warn "Docker 内打包失败 ($arch)：容器构建/打包出错，详见上方日志。"; DOCKER_LINUX_FAILED=1; return 1; }
            else
                # 项目在 Docker 默认不共享的卷（如 U 盘）：先 tar 到 /tmp 再挂载
                warn "项目不在 Docker 共享路径（/Users、/tmp），改用 /tmp 暂存方式挂载"
                local stage outstage
                stage="$(mktemp -d "/tmp/popball2-docker.XXXXXX")"
                outstage="$(mktemp -d "/tmp/popball2-docker-out.XXXXXX")"
                tar -C "$PROJECT_DIR" --exclude=build-pkg --exclude=build --exclude='*.o' \
                    --exclude=dist --exclude=.git -cf - . | tar -C "$stage" -xf -
                pa+=(--out /out)
                docker run --rm --platform "$plat" \
                    -v "$stage:/project:ro" -v "$outstage:/out:rw" \
                    -e POPBALL2_BUILD_DIR=/build -e NO_COLOR=1 \
                    "$img" bash -lc "cd /project && ./package.sh ${pa[*]}" \
                    || { rm -rf "$stage" "$outstage"; warn "Docker 内打包失败 ($arch)：容器构建/打包出错，详见上方日志。"; DOCKER_LINUX_FAILED=1; return 1; }
                cp -f "$outstage"/* "$OUT_DIR"/ 2>/dev/null || true
                rm -rf "$stage" "$outstage"
            fi
            ok "Linux ($arch) 打包完成 -> $OUT_DIR"
        }
        for a in "${LINUX_ARCHES[@]}"; do
            docker_build_linux "$a" "${LINUX_TARGETS[@]}" || true
        done
    else
        # 无 Docker：退回仅验证打包流程（不可分发）
        warn "未使用 Docker（--no-docker 或本机无 docker）：Linux 目标仅验证打包流程（产物不可分发）"
        PKG_ARGS=()
        [ -n "$VERSION" ] && PKG_ARGS+=(--version "$VERSION")
        [ -n "$ARCH" ]    && PKG_ARGS+=(--arch "$ARCH")
        [ -n "$JOBS" ]    && PKG_ARGS+=(--jobs "$JOBS")
        [ "$KEEP" -eq 1 ] && PKG_ARGS+=(--keep-stage)
        PKG_ARGS+=(--out "$OUT_DIR" --no-build --skip-platform-check "${LINUX_TARGETS[@]}")
        "$PKG" "${PKG_ARGS[@]}"
    fi
fi

printf '\n%s完成。%s 里查看产物。%s\n' "$C_G" "$OUT_DIR" \
    "$([ "$USE_DOCKER" -eq 1 ] && echo '(macOS 本机 + Linux x86/arm 均真实可分发)' || echo '(本机格式可分发，非本机格式仅验证流程)')"
info_plat "依赖与安装说明见 DEPENDENCIES.md（deb/rpm 请用包管理器安装以自动补装 Qt6）"

# 若有 Linux 目标经 Docker 构建失败（多为镜像仓库/代理配置问题），以非 0 退出，便于 CI/脚本感知
if [ "$DOCKER_LINUX_FAILED" -eq 1 ]; then
    warn "部分 Linux 包经 Docker 构建失败（详见上方警告）。本机 macOS 包已正常产出；请按提示修正 Docker 的 registry-mirror / 代理后重跑 Linux 目标。"
    exit 1
fi
