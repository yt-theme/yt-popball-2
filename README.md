# yt-popball-2

#### 介绍
popb使用qt6制作的popball第二代 —— 一个像"悬浮球"一样常驻桌面最顶层的系统监控挂件：
用面积图实时显示内存与交换分区占用，并显示 CPU 占用曲线、CPU 温度与上下行网速。
把小球拖到屏幕左/右边缘，它会吸附成一根**圆角竖条**，用**窄柱图**显示 CPU 占用 / 内存 /
交换分区，贴边不挡视线；把竖条往屏幕里侧拖开（或单击竖条）即可变回小球。

#### 软件架构
框架使用qt6，IDE使用安装qt6附带的qtcreator

支持 Linux（Xorg / Wayland）与 macOS；采集层已适配 x86_64、ARM64、Apple Silicon、
IBM POWER、IBM Z，以及高通骁龙 / 联发科天玑等 SoC 的传感器命名差异。

#### 快速开始

```bash
./run.sh              # 交互式：自动装开发环境 -> 构建 -> 运行
./run.sh -y           # 全部用默认值，一路到底
./run.sh --help       # 所有参数
```

`run.sh` 会自动识别发行版（Debian/Ubuntu、Fedora/RHEL、openSUSE、Arch、Alpine、Void…）
或 macOS（Homebrew），缺什么装什么；并会交互式询问构建目录、安装位置和 Qt 所在位置。

手动构建：

```bash
mkdir build && cd build
qmake6 ../popball2.pro && make -j$(nproc)
./popball2.app/Contents/MacOS/popball2     # macOS
./popball2                                 # Linux
```

#### 打包

一键打包发布（当前平台的全部格式，产物输出到 `dist/`）：

```bash
./package.sh                 # Linux: deb+rpm+AppImage   macOS: dmg+zip
./package.sh appimage        # 通用 AppImage（macOS 上经 Docker 构建，目标架构=本机架构）
./package.sh deb rpm         # 只打指定格式
./package.sh --version 1.2.0 # 指定版本号（默认读 .pro 里的 VERSION）
./package.sh --help
```

在 Linux 机器上跑一次 `./package.sh` 就得到 deb / rpm / AppImage 三个安装包；
在 macOS 上跑一次得到 dmg / zip，**再加 `appimage` 目标即可经 Docker 额外产出 AppImage**。
**同一份脚本两平台通用**，发布流程：Linux 上打一次 → macOS 上打一次 → 各格式齐活。
（跨平台无法交叉编译；rpm 需要 `rpmbuild`；AppImage 用 `tools/` 里预置的
linuxdeploy / appimagetool / runtime，缺失时才联网下载。注意 AppImage 只能在
「目标架构 == 本机架构」下构建——linuxdeploy 不能跨架构。）

| 目标 | 产物 | 说明 |
|---|---|---|
| `deb` | `popball2_<版本>_<架构>.deb` | Debian / Ubuntu |
| `rpm` | `popball2-<版本>-1.<架构>.rpm` | Fedora / RHEL / openSUSE（需要 `rpmbuild`） |
| `appimage` | `popball2-<版本>-<架构>.AppImage` | 通用 Linux，Qt 已内置；Linux 宿主原生构建，macOS 宿主经 Docker 构建 |
| `mac` | `popball2-<版本>-macos-<架构>.dmg` / `.zip` | 已用 macdeployqt 内置 Qt，可独立分发 |

产物统一输出到 `dist/`。macOS 包会做临时（ad-hoc）签名；正式分发请换成自己的开发者证书。

#### 依赖与「装上就能用」

| 格式 | 自包含？ | 关键点 |
|---|---|---|
| macOS dmg/zip | ✅ Qt 已内置 | 首次打开可能被 Gatekeeper 拦截，见下文 |
| Linux AppImage | ✅ Qt 已内置 | 需 FUSE，无 FUSE 用 `--appimage-extract` |
| Linux deb | ❌ 用系统 Qt6 | 用 `sudo apt install ./xxx.deb` 装会自动补装 Qt6 |
| Linux rpm | ❌ 用系统 Qt6 | 用 `sudo dnf install ./xxx.rpm` 装会自动补装 Qt6 |

> ⚠️ deb/rpm **不要**用 `dpkg -i` / `rpm -ivh` 直接装，那样不会自动解决依赖；
> 用包管理器形式（`apt install` / `dnf install` / `zypper install` 加本地路径）才会自动装 Qt6。
> 一键脚本 `./build.sh` 在 macOS 上通过 Docker 把 Linux 的 **x64 和 arm** 两种架构都打成真实可运行包。

完整的依赖关系、安装命令、各发行版注意事项、架构与 glibc 要求见 **[DEPENDENCIES.md](DEPENDENCIES.md)**。

#### 使用说明

1. 小球常驻桌面最顶层，可按住左键拖动；松手后位置会记住（Wayland 下受协议限制无法定位）
2. **贴边竖条**：把小球拖到屏幕左/右边缘（松手后贴边吸附），它会变成一根圆角竖条，
   条内用窄柱图显示 CPU 占用 / 内存 / 交换分区（柱高 = 当前百分比，柱底浅色是"满格"轨道）；
   竖条只能沿边缘上下移动，鼠标悬停会显示各指标的具体百分比。
   变回小球：把竖条往屏幕里侧拖开，或直接单击竖条。
   **宽度/高度/圆角大小都能调**（默认 30×100、圆角 8）：右键 →「设置 → 贴边竖条」，
   那里有「窄 20 / 标准 30 / 宽 36」一键档位，也可以直接填宽度、高度、圆角半径（改完立即生效）
3. **右键**点小球（或竖条）弹出菜单：「设置」打开扁平化设置窗口（9 种颜色含阴影色、6 套预制配色、大小/边框/阴影长度(0=无阴影)/透明度/刷新率等全部可配，支持应用/恢复默认(二次确认)，保存即生效），「退出」先隐藏窗口再安全退出
4. 配置在 `~/.popball2_config.ini`：颜色、大小、刷新间隔、显示哪些组件都在这里改
   （贴边竖条相关项在 `[aside]` 段：`snap_to_edge` 开关、`aside_edge` 当前吸附侧，竖条宽高沿用 `[appearance]` 的 `aside_width`/`aside_height`）
5. 桌面没开混成（compositing）时，可在配置里设 `shape_mask=1` 让窗口退回形状蒙版（圆球=圆形、竖条=圆角矩形），避免出现矩形黑框
6. 取不到的指标（例如 Apple Silicon 的 CPU 频率）会自动隐藏，不会显示假数据
7. macOS 上应用是「配件型」：不会出现在 Dock 和 Cmd+Tab 里，就像 360 悬浮球那样


#### 开源协议

本项目基于 **GNU General Public License v2（GPLv2）** 发布，详见仓库根目录的 `LICENSE` 文件。
任何对本软件的再分发或修改版本，都必须在相同协议（GPLv2）下提供源代码。

#### 参与贡献

1.  Fork 本仓库
2.  新建 Feat_xxx 分支
3.  提交代码（可用仓库内 `./push.sh "提交说明"` 一键同时推送到 Gitee 与 GitHub）
4.  新建 Pull Request


#### 特技

1.  使用 Readme\_XXX.md 来支持不同的语言，例如 Readme\_en.md, Readme\_zh.md
2.  Gitee 官方博客 [blog.gitee.com](https://blog.gitee.com)
3.  你可以 [https://gitee.com/explore](https://gitee.com/explore) 这个地址来了解 Gitee 上的优秀开源项目
4.  [GVP](https://gitee.com/gvp) 全称是 Gitee 最有价值开源项目，是综合评定出的优秀开源项目
5.  Gitee 官方提供的使用手册 [https://gitee.com/help](https://gitee.com/help)
6.  Gitee 封面人物是一档用来展示 Gitee 会员风采的栏目 [https://gitee.com/gitee-stars/](https://gitee.com/gitee-stars/)
