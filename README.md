# yt-popball-2

#### 介绍
popb使用qt6制作的popball第二代 —— 一个像"悬浮球"一样常驻桌面最顶层的系统监控挂件：
用面积图实时显示内存与交换分区占用，并显示 CPU 占用曲线、CPU 温度与上下行网速。

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
./package.sh deb rpm         # 只打指定格式
./package.sh --version 1.2.0 # 指定版本号（默认读 .pro 里的 VERSION）
./package.sh --help
```

在 Linux 机器上跑一次 `./package.sh` 就得到 deb / rpm / AppImage 三个安装包；
在 macOS 上跑一次得到 dmg / zip。**同一份脚本两平台通用**，发布流程：
Linux 上打一次 → macOS 上打一次 → 4 类安装包齐了。（跨平台无法交叉编译，
rpm 需要装 `rpmbuild`，AppImage 首次运行会自动下载工具。）

| 目标 | 产物 | 说明 |
|---|---|---|
| `deb` | `popball2_<版本>_<架构>.deb` | Debian / Ubuntu |
| `rpm` | `popball2-<版本>-1.<架构>.rpm` | Fedora / RHEL / openSUSE（需要 `rpmbuild`） |
| `appimage` | `popball2-<版本>-<架构>.AppImage` | 通用 Linux（自动下载 linuxdeploy / appimagetool） |
| `mac` | `popball2-<版本>-macos-<架构>.dmg` / `.zip` | 已用 macdeployqt 内置 Qt，可独立分发 |

产物统一输出到 `dist/`。macOS 包会做临时（ad-hoc）签名；正式分发请换成自己的开发者证书。

#### 使用说明

1. 小球常驻桌面最顶层，可按住左键拖动；松手后位置会记住（Wayland 下受协议限制无法定位）
2. **右键**点小球弹出菜单：「设置」打开扁平化设置窗口（9 种颜色含阴影色、6 套预制配色、大小/边框/阴影长度(0=无阴影)/透明度/刷新率等全部可配，支持应用/恢复默认(二次确认)，保存即生效），「退出」先隐藏窗口再安全退出
3. 配置在 `~/.popball2_config.ini`：颜色、大小、刷新间隔、显示哪些组件都在这里改
4. 桌面没开混成（compositing）时，可在配置里设 `shape_mask=1` 让窗口退回圆形蒙版，避免出现矩形黑框
5. 取不到的指标（例如 Apple Silicon 的 CPU 频率）会自动隐藏，不会显示假数据
6. macOS 上应用是「配件型」：不会出现在 Dock 和 Cmd+Tab 里，就像 360 悬浮球那样


#### 参与贡献

1.  Fork 本仓库
2.  新建 Feat_xxx 分支
3.  提交代码
4.  新建 Pull Request


#### 特技

1.  使用 Readme\_XXX.md 来支持不同的语言，例如 Readme\_en.md, Readme\_zh.md
2.  Gitee 官方博客 [blog.gitee.com](https://blog.gitee.com)
3.  你可以 [https://gitee.com/explore](https://gitee.com/explore) 这个地址来了解 Gitee 上的优秀开源项目
4.  [GVP](https://gitee.com/gvp) 全称是 Gitee 最有价值开源项目，是综合评定出的优秀开源项目
5.  Gitee 官方提供的使用手册 [https://gitee.com/help](https://gitee.com/help)
6.  Gitee 封面人物是一档用来展示 Gitee 会员风采的栏目 [https://gitee.com/gitee-stars/](https://gitee.com/gitee-stars/)
