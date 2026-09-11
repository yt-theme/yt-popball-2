# popball2 —— Linux 构建/打包镜像（多架构：linux/amd64 或 linux/arm64）
#
# 用途：在 macOS（或其它非 Linux 宿主）上用 Docker 真正编译并打包 Linux 包。
# 镜像只装构建与打包依赖，项目源码在运行时通过挂载传入（见 build.sh）。
#
# 基础镜像 ubuntu:22.04 同时提供 amd64 / arm64 两种架构，docker build --platform
# 会按需拉取对应架构的层；Qt6 来自 Ubuntu 22.04 官方仓库（qt6-base-dev）。
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8 LC_ALL=C.UTF-8

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        qt6-base-dev \
        qt6-base-dev-tools \
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

WORKDIR /src
