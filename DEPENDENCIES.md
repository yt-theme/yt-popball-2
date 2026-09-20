# 打包依赖与安装说明（popball2）

`./build.sh`（或 `./package.sh`）打出的安装包，**目标就是「装上就能用」**。
本章说明每种包**是否已经内置全部依赖**、**运行时还差什么**、以及**正确的安装命令**。

> 一句话结论：
> - **macOS dmg / zip** 和 **Linux AppImage** —— **自包含**，Qt 已经打进包里，拷过去直接跑。
> - **Linux deb / rpm** —— **不内置 Qt**（行业标准做法），但用包管理器安装时会**自动补装**系统 Qt6，所以也是「装上就能用」。

---

## 1. 各格式一览

| 格式 | 产物 | 是否自包含 | 运行时还依赖什么 | 推荐安装方式 |
|---|---|---|---|---|
| `mac` | `popball2-<ver>-macos-<arch>.dmg` / `.zip` | ✅ 是（Qt 已内置） | macOS 11+；首次打开需绕过 Gatekeeper | 拖进 `/Applications` |
| `appimage` | `popball2-<ver>-<arch>.AppImage` | ✅ 是（Qt 已内置） | 需要 **FUSE**（无 FUSE 可用 `--appimage-extract`）；glibc ≥ 2.35 | `chmod +x` 后直接运行 |
| `deb` | `popball2_<ver>_<arch>.deb` | ❌ 否（用系统 Qt6） | Debian/Ubuntu 仓库里的 `qt6-base`（A→自动装） | `sudo apt install ./xxx.deb` |
| `rpm` | `popball2-<ver>-1.<arch>.rpm` | ❌ 否（用系统 Qt6） | Fedora/RHEL 里的 `qt6-qtbase`（DNF 自动装） | `sudo dnf install ./xxx.rpm` |

> 想「一个文件到处跑、零系统依赖」就选 **AppImage**；想走系统包管理器就选 **deb / rpm**。

---

## 2. macOS（dmg / zip）

### 依赖情况
- 打包时已用 `macdeployqt` 把 **Qt6 的 Core / Gui / Widgets 框架**以及 **Cocoa 平台插件**
  全部复制进 `.app/Contents/Frameworks` 与 `PlugIns/`，并把二进制改写成
  `@rpath` / `@executable_path/../Frameworks` 引用。
- **完全自包含**，拷到另一台 Mac 上不需要装 Qt。
- 仅要求目标系统为 **macOS 11 (Big Sur) 或更新**。

### 安装 / 使用
```bash
# dmg：双击挂载，把 PopBall.app 拖到「应用程序」
open popball2-1.0.0-macos-arm64.dmg
# zip：解压后把 .app 拖到 /Applications
```
- 由于是个人 **ad-hoc 临时签名**（无开发者证书），首次打开可能被 Gatekeeper 拦截：
  - 右键 →「打开」，或在终端放行：
    ```bash
    xattr -dr com.apple.quarantine /Applications/PopBall.app
    ```
- 正式对外发布时，请用你自己的**开发者证书**重签（见下「正式签名」）。

### 为什么不用 Notarize
脚本只做 ad-hoc 签名（够内网/个人用）。上架 App Store 或避免 Gatekeeper 弹窗需要：
```bash
codesign --force --deep --sign "Developer ID Application: <你的证书>" PopBall.app
xcrun notarytool submit PopBall.app ...   # 如需公证
```
这一步需要你自己的账号，未写进脚本。

---

## 3. Linux AppImage（自包含，推荐）

### 依赖情况
- 打包时用 `linuxdeploy --plugin qt` 把 **Qt6 运行库 + 平台/风格插件** 全部塞进
  `AppDir`，再用 `appimagetool` 压成一个文件。**完全自包含**，不依赖系统 Qt。
- 运行时仅依赖：
  - **glibc ≥ 2.35**（构建镜像基于 Ubuntu 22.04；主流发行版 22.04+ 均满足）。
  - **FUSE** 用于挂载 AppImage 自身（见下）。

### 安装 / 使用
```bash
chmod +x popball2-1.0.0-x86_64.AppImage
./popball2-1.0.0-x86_64.AppImage
```
- **没有 FUSE**（部分最小化/容器系统）时直接运行会报 FUSE 错误，两种办法：
  ```bash
  # 办法 A：装 FUSE
  sudo apt install fuse        # Debian/Ubuntu
  sudo dnf install fuse        # Fedora
  # 办法 B：不解挂，直接抽出内容运行
  ./popball2-1.0.0-x86_64.AppImage --appimage-extract
  ./squashfs-root/AppRun
  ```

---

## 4. Linux deb（Debian / Ubuntu）

### 依赖情况
- **不内置 Qt**（所有 Qt 桌面应用的 deb 都这样，避免重复打包、保持与系统一致）。
- 包的 `Depends` 已声明：
  ```
  libc6, libqt6core6 | libqt6core6t64,
  libqt6gui6 | libqt6gui6t64, libqt6widgets6 | libqt6widgets6t64,
  libqt6sql6 | libqt6sql6t64,
  libqt6multimedia6 | libqt6multimedia6t64
  ```
  （`*t64` 是 64 位时间制式变体，Debian 13 / Ubuntu 24.04+ 用；用 `|` 兼容两种）
- **图形会话（X11 / Wayland）**：Xorg 会话用 Qt 的 **xcb** 平台插件（`qt6-qpa-plugins`，
  通常随 `libqt6gui6` 传递依赖装好）；Wayland 会话需要 **`qt6-wayland`** 提供的
  wayland 平台插件。两者都装后，**同一二进制在 Xorg 与 Wayland 下都能直接跑**（Qt 自动
  检测会话）。缺插件时启动会报 `could not load the Qt platform plugin "xcb"/"wayland"`，
  用包管理器补装对应包即可。弹窗尺寸调整在 Wayland 下自动启用右/下边缘拖拽（协议不提供
  全局鼠标坐标，左/上缘固定；X11 与 Windows 四边缘全支持），尺寸一律实时落盘。
- **剪贴板历史（SQLite）**：程序用 QtSql 的 QSQLITE 驱动把中转站内容持久化到
  `~/.popball2_clipboard.db`。deb 里以 `Recommends: libqt6sql6-sqlite` 声明驱动插件
  （apt 默认会装上；用 `--no-install-recommends` 时不会装）。缺这个插件时程序**照常运行**，
  只是历史不落盘（启动日志里会有一行提示）。Fedora 的 sqlite 驱动包含在 `qt6-qtbase` 里，无需额外包。
- **视频预览**（「数据中转站」面板的「预览」布局与悬停静音播放）：
  - 播放依赖 **QtMultimedia** 运行时；Linux 上还需要 gstreamer 插件，deb 里写成
    `Recommends: gstreamer1.0-plugins-base, gstreamer1.0-plugins-good`。缺了不会崩，
    只是播放失败 → 退化成"静态封面 + 双击用系统播放器打开"。
  - **封面帧**优先用 `ffmpeg`（`-ss 1` 抽一帧）；没有 ffmpeg 时 macOS 退回系统 Quick Look
    （`qlmanage -t`）。两者都没有时显示胶片占位图 + 播放按钮，其它功能不受影响。
  - 构建时用 `qtHaveModule(multimedia)` 守卫：没装 Qt6 Multimedia 开发包也能编译，只是不含播放；
    打包镜像会尽量装 `qt6-multimedia-dev`，装不上会打一行 WARN 继续构建。
- 用 **apt** 安装时会**自动从仓库下载并装好**这些 Qt6 运行库（含它们传递依赖的
  libgl、fontconfig、xkbcommon 等）。所以「装上就能用」。

### 正确的安装命令（重要）
```bash
# ✅ 用 apt 装本地 deb —— 自动解析并补装 Qt6
sudo apt install ./popball2_1.0.0_amd64.deb

# ❌ 不要这样（dpkg 不会自动装依赖，会报缺库）：
sudo dpkg -i popball2_1.0.0_amd64.deb
sudo apt -f install          # 如果误用了 dpkg -i，再补这一句即可
```

### 需要系统里有 Qt6 仓库
- Debian 12 / Ubuntu 22.04+ 官方源自带 `qt6-base`，无需额外添加。
- 更老的发行版（如 Ubuntu 20.04）官方源**没有** Qt6，deb 能装但 Qt6 装不上 →
  这种系统请用 **AppImage**（自带 Qt）。

---

## 5. Linux rpm（Fedora / RHEL / openSUSE）

### 依赖情况
- 同样**不内置 Qt**。`Requires: qt6-qtbase` 由 DNF/YUM 自动解析并安装
  （Fedora/RHEL 的 `qt6-qtbase` 已包含 Core/Gui/Widgets）。
- openSUSE 对应包名也是 `qt6-base`，DNF/Zypper 安装时同样自动补。

### 正确的安装命令
```bash
# ✅ Fedora / RHEL（DNF 自动补装 Qt6）
sudo dnf install ./popball2-1.0.0-1.x86_64.rpm

# ✅ openSUSE（Zypper 自动补装）
sudo zypper install ./popball2-1.0.0-1.x86_64.rpm

# ❌ 避免用 rpm -ivh（不会自动装依赖）
sudo rpm -ivh popball2-1.0.0-1.x86_64.rpm
sudo dnf -y install qt6-qtbase   # 若误用 rpm -ivh，手动补这一句
```

---

## 6. 架构说明

- **x86_64（x64）**：Intel / AMD 64 位。
- **aarch64（arm64）**：Apple Silicon、树莓派 4/5（64 位系统）、飞腾、鲲鹏、骁龙 X 等。
- 包名里的架构字段：deb 用 `amd64`/`arm64`，rpm 用 `x86_64`/`aarch64`，AppImage 用 `x86_64`/`aarch64`。
- 在 macOS 上用 `./build.sh` 时，Linux 包通过 Docker 分别打出 **x64 与 arm** 两种架构，
  都是该架构**真实可运行**的二进制（不是模拟）。

---

## 7. 从源码构建时的依赖（开发者用）

`./build.sh` / `./package.sh` 自己会装齐构建依赖；这里仅作记录：

| 系统 | 构建依赖 |
|---|---|
| Debian/Ubuntu | `build-essential qt6-base-dev qt6-base-dev-tools qmake6 dpkg-dev rpm curl patchelf` |
| Fedora/RHEL | `gcc-c++ qt6-qtbase-devel rpm-build curl patchelf` |
| macOS | Homebrew `qt` + Xcode Command Line Tools（`macdeployqt` 随 Qt 提供） |
| Docker 镜像 | `docker/linux-build/Dockerfile` 已固化上述 Debian 依赖 |

打包额外工具（缺失时脚本会**黄色警告并跳过对应格式**，不致命）：
- `rpm` / `rpmbuild`：deb 与 AppImage 不受影响。
- `linuxdeploy` + `appimagetool`：AppImage 首次会自动联网下载到 `tools/`，之后复用。

---

## 8. 打包后自检（脚本已做）

`./package.sh`（被 `build.sh` 调用）在打包前会：
- **Linux**：跑 `ldd` 确认二进制所有动态库都能解析，避免打出「缺库」的残包；
- **macOS**：跑 `otool -L` 确认 Qt 框架已被 `macdeployqt` 打进 `.app`，
  若发现 Qt 仍指向系统/Homebrew 路径会**警告**（说明 macdeployqt 可能没生效）。

若出现上述告警，多半是构建环境缺 Qt 或 `macdeployqt` 版本不匹配，先跑 `./run.sh` 修环境再打包。
