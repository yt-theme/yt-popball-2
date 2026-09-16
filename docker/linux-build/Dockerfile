# popball2 —— Linux 构建/打包镜像（多架构：linux/amd64 或 linux/arm64）
#
# 用途：在 macOS（或其它非 Linux 宿主）上用 Docker 真正编译并打包 Linux 包。
# 镜像只装构建与打包依赖，项目源码在运行时通过挂载传入（见 build.sh）。
#
# 基础镜像 ubuntu:22.04 同时提供 amd64 / arm64 两种架构，docker build --platform
# 会按需拉取对应架构的层；Qt6 来自 Ubuntu 22.04 官方仓库（qt6-base-dev）。
FROM ubuntu:22.04

# 注入代理（由 package.sh 传入，已把宿主机的 127.0.0.1 改写为 host.docker.internal）。
# 用独立命名（MY_*）避免被 Docker Desktop 的“Proxies”设置（常是残缺的 127.0.0.1:0）覆盖；
# 在 RUN 里先 unset 掉 daemon 注入的残缺代理，再导出我们传入的正确代理。无代理留空即直连。
ARG MY_HTTP_PROXY=""
ARG MY_HTTPS_PROXY=""

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8 LC_ALL=C.UTF-8

RUN unset HTTP_PROXY HTTPS_PROXY http_proxy https_proxy 2>/dev/null; \
    export HTTP_PROXY="$MY_HTTP_PROXY" HTTPS_PROXY="$MY_HTTPS_PROXY" \
           http_proxy="$MY_HTTP_PROXY" https_proxy="$MY_HTTPS_PROXY"; \
    apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        qt6-base-dev \
        qt6-base-dev-tools \
        qt6-l10n-tools \
        libqt6sql6-sqlite \
        qmake6 \
        dpkg-dev \
        rpm \
        curl \
        wget \
        patchelf \
        file \
        binutils \
        libgl1 \
        libglib2.0-0 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Qt6 的 lrelease/lupdate 在 Ubuntu 22.04 里由 qt6-l10n-tools 提供（真实二进制在
# /usr/lib/qt6/bin/）。若系统装了 qtchooser，/usr/bin/lrelease 会被指向一个对 Qt6 失效的
# 包装脚本（报 “could not find a Qt installation of ''”），导致 qmake 的 CONFIG+=lrelease 失败。
# 这里把真正的二进制直接链到 /usr/bin，确保无论 qmake 走 PATH 还是 qtchooser 都能编译 .ts。
RUN ln -sf /usr/lib/qt6/bin/lrelease /usr/bin/lrelease 2>/dev/null || true \
 && ln -sf /usr/lib/qt6/bin/lupdate  /usr/bin/lupdate  2>/dev/null || true

# 视频悬停预览播放用 QtMultimedia（可选依赖）：装上就有该功能；包不可用时构建照旧继续，
# configure 里 qtHaveModule(multimedia) 会自动关掉播放，运行时退化成"静态封面 + 双击系统播放器"。
RUN unset HTTP_PROXY HTTPS_PROXY http_proxy https_proxy 2>/dev/null; \
    export HTTP_PROXY="$MY_HTTP_PROXY" HTTPS_PROXY="$MY_HTTPS_PROXY" \
           http_proxy="$MY_HTTP_PROXY" https_proxy="$MY_HTTPS_PROXY"; \
    apt-get update \
    && (apt-get install -y --no-install-recommends qt6-multimedia-dev \
        || echo "WARN: qt6-multimedia-dev 装不上，本次构建不含视频预览播放") \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
