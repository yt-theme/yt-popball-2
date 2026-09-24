# yt-popball-2

#### 介绍

popball2 —— 用 Qt6 制作的桌面「悬浮球」系统监控挂件 + 剪贴板数据中转站，常驻桌面最顶层：

- **系统监控**：面积图实时显示内存与交换分区占用，CPU 占用曲线、CPU 温度、CPU 频率、
  上下行网速、磁盘读写速度，全部数值用数码管风格显示，颜色/显隐均可配置；
- **多种形态**：悬浮球支持 球形（默认）/ 圆角矩形 / 直角方形 / 长条形 四种形态，
  每种形态的数码管文字尺寸与位置独立适配（长条形改为两行横排），设置里一键切换；
- **贴边竖条**：把小球拖到屏幕左/右边缘，它会吸附成一根圆角竖条，用窄柱图显示
  CPU 占用 / 内存 / 交换分区，贴边不挡视线；
- **数据中转站**：把鼠标在悬浮球上停留片刻（或拖文件到球上）即可弹出 —— 剪贴板
  里的文本、图片、文件会被自动收进来，支持四种视图、悬停预览、文本编辑、拖入/拖出，
  以及「仅剪贴板 / 仅中转数据」来源过滤，历史记录持久化到本地 SQLite，重启不丢；
- **中英双语**：界面支持 跟随系统 / 简体中文 / English 三种语言，设置里一键切换，
  翻译走 Qt 官方 lupdate/lrelease + .ts/.qm 管线，新增界面文案后跑两条命令即可更新。

支持 **macOS**（Apple Silicon / Intel）、**Linux**（Xorg / Wayland，Debian/Ubuntu、
Fedora/RHEL、openSUSE、Arch、Alpine、Void 等）与 **Windows**；采集层已适配 x86_64、
ARM64、Apple Silicon、IBM POWER、IBM Z，以及高通骁龙 / 联发科天玑等 SoC 的传感器命名差异。

#### 软件架构

- 框架：Qt 6（C++17），IDE 可用安装 Qt6 附带的 Qt Creator
- 分层：
  - `widget` —— 悬浮球本体：绘制、拖拽、贴边竖条、悬停轮询
  - `popdock` —— 数据中转站面板：四种视图、类型筛选、悬停预览（图片/文本/视频）、
    文本小编辑器、剪贴板监控、划出/划入动画
  - `clipstore` —— 剪贴板历史（SQLite，QtSql/QSQLITE；构建时无该模块自动退化不持久化）
  - `sysInfo` —— 跨平台系统信息采集（CPU 温度/频率/占用、内存、交换、网速、磁盘 IO）
  - `config` —— INI 配置读写（默认值、迁移、`POPBALL2_CONFIG` 覆盖）
  - `settingsdialog` —— 扁平化设置窗口
  - `macwindow` —— macOS 桌面配件行为（不出现在 Dock / Cmd+Tab，跨 Space 常驻）
- 可选模块（qmake 检测到才启用，缺了也能编译运行）：
  - `QtSql` → 剪贴板历史落盘（Linux 运行时另需 QSQLITE 插件包）
  - `QtMultimedia` → 视频条目在悬停预览里静音循环播放（缺省时退化为静态封面）

#### 功能特性

| 模块 | 特性 |
|---|---|
| 系统监控 | 内存 / 交换面积图、CPU 占用曲线、CPU 温度（AppleSMC / Linux hwmon / Windows：WinRing0 驱动 → LibreHardwareMonitor → OpenHardwareMonitor → ACPI 热区）、CPU 频率、上下行网速、磁盘读写速度（自动挑 IO 最高的盘或指定盘） |
| 悬浮球形态 | 球形（默认）/ 圆角矩形 / 直角方形 / 长条形 四种形态，设置里切换；每种形态单独适配 LCD 文字尺寸与位置（长条形为两行横排） |
| 贴边竖条 | 拖到屏幕左/右边缘吸附成圆角竖条，窄柱图显示 CPU / 内存 / 交换；悬停显示百分比；往屏幕里侧拖开或单击即变回小球；宽度/高度/圆角可调，设置里有「窄 20 / 标准 30 / 宽 36」一键档位 |
| 数据中转站 | 悬浮球上停留 300ms 弹出；自动收录剪贴板（文本/图片/文件），支持拖文件入站、Ctrl/Cmd+V 粘贴、底栏「新增记事」 |
| 弹窗尺寸调整 | 鼠标按住弹窗四条边缘（6px 热区）拖拽即可实时改大小（最小 240×260）；也可在设置「中转站弹窗 → 弹窗宽×高」精确输入，旁边「恢复默认尺寸」一键回到 330×452；改完点「应用」/「确定」立即生效并持久化，重启、重开弹窗都保持。跨平台：macOS 边缘拖拽走系统原生缩放，Windows / Linux(X11) 走 Qt 鼠标事件、四边缘全支持，Linux Wayland 因协议不提供全局鼠标坐标、启用右/下边缘拖拽（左/上缘固定，放大缩小都够用）；各平台最终尺寸统一由 `resizeEvent` 兜底落盘，拖拽中途中断也不丢 |
| 来源过滤 | 弹窗顶部两个开关「仅剪贴板」「仅中转数据」：互斥勾选，只显示对应来源的条目；都关=显示全部（默认）；与类型筛选正交叠加，不动数据与顺序；旧库记录统一按「剪贴板」处理 |
| 四种视图 | 图标网格 / 列表 / 详细 / 预览，一键切换，布局偏好落盘 |
| 类型筛选 | 顶部 tab：全部 / 文档（文本+Office+PDF+代码等）/ 图片（剪贴板图+图片文件），只影响展示不动数据 |
| 悬停预览 | 鼠标悬停条目弹出预览：图片原图、文本内容、文件图标、视频（QtMultimedia 时静音循环播放）；预览下沿显示「类型 · 尺寸 · 大小 · 完整路径」详情条 |
| 文本编辑 | 双击文本条目弹出小编辑器，可直接修改；关闭时未保存的改动自动保存 |
| 剪贴板历史 | SQLite 持久化（默认 `~/.popball2_clipboard.db`，`POPBALL2_DB` 可覆盖）；按内容指纹去重，重复只提升排序不新增；默认保留最近 200 条；图片以 PNG 二进制入库 |
| 面板操作按钮 | 中转站面板右上角：⚙ 设置、柱状图 系统监视器、⏻ 退出 独立按钮；系统监视器命令可在设置里自定义（留空自动检测，单实例启动） |
| 窗口行为 | 小球常驻最顶层、位置自动记住；macOS 为「配件型」应用（不在 Dock、不在 Cmd+Tab、跨桌面显示、全屏应用之上可见） |
| 本地化 | 界面语言：0=跟随系统（默认）1=简体中文 2=English；设置窗口「窗 口 → 界面语言」切换，保存后重启生效；语言名用自身语言显示（业界惯例） |

#### 快速开始

```bash
./run.sh              # 交互式：自动装开发环境 -> 构建 -> 运行
./run.sh -y           # 全部用默认值，一路到底
./run.sh --help       # 所有参数
```

`run.sh` 会自动识别发行版（Debian/Ubuntu、Fedora/RHEL、openSUSE、Arch、Alpine、Void…）、
macOS（Homebrew）或 Windows（Git Bash / MSYS2），缺什么装什么；并会交互式询问构建目录、
安装位置和 Qt 所在位置。

Windows 上没有系统包管理器，脚本改用 **aqtinstall** 把 Qt6 的 MinGW 套件和 MinGW 编译器
装到 `C:\Qt`，然后用 `mingw32-make` 构建：

```bash
# 在 Git Bash / MSYS2 里执行
./run.sh --deps-only     # 只装 Qt6 + MinGW（首次约 1.5GB）
./run.sh -y              # 装依赖 -> 构建 -> 运行
```

可用环境变量覆盖默认值：`POPBALL2_QT_ROOT`（安装根目录，默认 `/c/Qt`）、
`POPBALL2_QT_VERSION`（默认 `6.8.3`）、`POPBALL2_MINGW_TOOL`（默认 `tools_mingw1310`）、
`POPBALL2_QT_MIRROR`（下载镜像，默认官方源）、`POPBALL2_QT_TIMEOUT`（单次下载超时，默认 120 秒）、
`POPBALL2_QT_ARCHIVES`（归档子集，默认跳过用不到的 QML）、`POPBALL2_SEVENZIP`（7z 可执行文件）、
`POPBALL2_PYTHON`（Python 解释器路径）。

> aqtinstall 有两个坑脚本已自动处理：默认 `concurrency=4` 会多进程同时往同一目录解压，
> Windows 上容易互相踩踏、只解压一半（`Failed to write to base directory`）；默认用内置
> py7zr 解压 qtbase 这种几千条目的归档会很慢。脚本会把并发改成 1，并在系统没有 7z 时自动
> 下一个 `7zr.exe` 交给 aqt 用。

手动构建：

```bash
mkdir build && cd build
qmake6 ../popball2.pro && make -j$(nproc)
./popball2.app/Contents/MacOS/popball2     # macOS
./popball2                                 # Linux
./release/popball2.exe                     # Windows（要先 windeployqt 把 Qt 运行库放
                                           # 到 exe 旁，否则报「找不到 Qt6Core.dll」；
                                           # 直接跑 ./run.sh 会自动完成这一步）
```

#### 打包

一键打包发布（当前平台的全部格式，产物输出到 `dist/`）：

```bash
./package.sh                 # Linux: deb+rpm+AppImage   macOS: dmg+zip   Windows: zip
./package.sh win             # 只打 Windows zip（windeployqt 内置 Qt + 附带 WinRing0 驱动）
./package.sh appimage        # 通用 AppImage（macOS 上经 Docker 构建，目标架构=本机架构）
./package.sh deb rpm         # 只打指定格式
./package.sh --version 1.2.0 # 指定版本号（默认读 package.json 的 version）
./package.sh --help
```

也支持 npm 脚本入口（Node 封装，转发给 `pack.js`，同一套逻辑）：

```bash
npm run deb:x64     # 构建 Linux x86_64 .deb（Docker 或本机交叉编译）
npm run deb:arm     # 构建 Linux arm64 .deb
npm run rpm:x64     # / rpm:arm / appimage:x64 / appimage:arm / mac / win
npm run all         # 当前平台全部格式
```

在 Linux 机器上跑一次 `./package.sh` 就得到 deb / rpm / AppImage 三个安装包；
在 macOS 上跑一次得到 dmg / zip，**再加 `appimage` 目标即可经 Docker 额外产出 AppImage**。
**同一份脚本两平台通用**，发布流程：Linux 上打一次 → macOS 上打一次 → 各格式齐活。
（跨平台无法交叉编译；rpm 需要 `rpmbuild`；AppImage 用 `tools/` 里预置的
linuxdeploy / appimagetool / runtime，缺失时才联网下载。注意 AppImage 只能在
「目标架构 == 本机架构」下构建——linuxdeploy 不能跨架构。）

Windows 上跑 `./package.sh win`（或 `npm run win`）得到
`dist/popball2-<版本>-windows-x86_64.zip`，解压即用（无需装 Qt）：包内已用
`windeployqt` 内置 Qt 运行库与 MinGW 运行库、附带 **WinRing0 驱动**（CPU 温度，
见 `drivers/README.md`）和 `README-Windows.txt` 使用说明，exe 也带上了
`resources/popball2.ico` 图标与版本号。脚本会自动探测 Qt / MinGW
（PATH → `C:\Qt\<版本>\mingw_64` → `C:\Qt\Tools\mingw*`），无需手工配 PATH；
可用 `POPBALL2_QT_ROOT` / `POPBALL2_MINGW_BIN` 覆盖。

**构建目录同样自包含**：`./run.sh` 与 `./package.sh win` 在编译后都会调用 `windeployqt`，
把 Qt6 运行库与 `WinRing0x64.sys` 直接放到 exe 旁，所以 `build/release/popball2.exe`
双击就能运行，不再依赖开发机的 PATH。这一步不是锦上添花：程序读 CPU 温度要装内核驱动，
非管理员时会经 **UAC 提权重新拉起自己**，而提权后的子进程是「干净环境」（拿不到脚本临时
挂上去的 Qt bin）——exe 旁边没有 Qt DLL 就会以 `0xC0000135`(STATUS_DLL_NOT_FOUND) 秒退，
驱动装不上、CPU 温度也就读不出来。实测对比：exe 旁无 Qt DLL 时进程 3.8MB/4 线程卡在加载
失败态，有 DLL 时正常起球（约 38MB）。`run.sh --install` 安装到的目录同样会被部署。

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

1. **小球**：常驻桌面最顶层，可按住左键拖动；松手后位置会记住（Wayland 下受协议限制无法定位）。
   球上按配置显示 温度 / 频率 / 磁盘读写 / 上下行网速 等数值（数码管样式，可分别隐藏）。
2. **贴边竖条**：把小球拖到屏幕左/右边缘（松手后贴边吸附），它会变成一根圆角竖条，
   条内用窄柱图显示 CPU 占用 / 内存 / 交换分区（柱高 = 当前百分比，柱底浅色是"满格"轨道）；
   竖条只能沿边缘上下移动，鼠标悬停会显示各指标的具体百分比。
   变回小球：把竖条往屏幕里侧拖开，或直接单击竖条。
   **宽度/高度/圆角大小都能调**（默认 30×100、圆角 8）：面板右上角「⚙ 设置 → 贴边竖条」，
   那里有「窄 20 / 标准 30 / 宽 36」一键档位，也可以直接填宽度、高度、圆角半径（改完立即生效）。
3. **数据中转站**：鼠标在悬浮球上停留约 300ms（或把文件拖到球上）即弹出面板：
   - **收录**：复制过的文本 / 图片会自动进来；把文件拖入面板即可入站；底栏右侧
     「铅笔+加号」可手动新建一条记事；也可以按 `Ctrl/Cmd+V` 直接粘贴当前剪贴板。
   - **四种视图**：底栏左侧切换 图标网格 / 列表 / 详细 / 预览；「详细」视图显示
     每条的类型、大小、路径与文本摘要。
   - **类型筛选**：标题下沿 tab 按**站内实际内容**动态出现 ——「全部」恒在，另按
     内容显示 文档 / 图片 / 视频 / 安装包 / 压缩包 / 音频 / 可执行 / 字体 /
     数据库 / 设计（某分类没有条目就不显示对应 tab，拖入第一个该类型文件时自动
     出现；一行放不下自动换第二行）。安装包覆盖 Android / iOS / Windows /
     macOS / Linux 常见格式（apk / ipa / exe / dmg / deb…），可执行含无后缀可执行
     文件，字体、数据库、设计 / CAD / 3D 各有独立扩展名白名单。分类互斥，
     只筛"看什么"，不动数据与顺序，切回「全部」立刻恢复；标题计数显示「可见 / 总数」。
   - **来源过滤**：tab 上方两个开关「仅剪贴板」「仅中转数据」互斥 —— 勾「仅剪贴板」
     只看剪贴板粘贴/复制进来的内容，勾「仅中转数据」只看拖入面板/新增记事的内容；
     都关（默认）= 全部。与类型 tab 正交叠加（先过类型、再过来源），只影响"看什么"，
     不动数据与顺序；标题计数同步显示「可见 / 总数」。旧版历史记录没有来源标记，
     统一视为「剪贴板」，不会凭空消失。
   - **预览**：鼠标悬停条目弹出预览（图片原图 / 文本内容 / 文件图标 / 视频静音播放），
     标题行完整显示文件名（超长自动换行），预览下沿有「类型 · 尺寸 · 大小 · 完整路径」详情条；移开即收起。
   - **打开与编辑**：双击条目 —— 文本打开小编辑器（改完关闭自动保存），
     其它交给系统默认程序；右键条目可 复制 / 打开 / 删除，`Delete` 键也可删除。
   - **拖出**：把条目拖到桌面/文件夹即复制出文件或图片。
   - **持久化**：所有内容存进本地剪贴板历史库（默认 `~/.popball2_clipboard.db`），
     重启后按"最近使用"顺序回填，内容相同只提升排序、不重复。
   - **主题联动**：面板的选中高亮、右键菜单底色、自绘图标（文本条目 T 图标、底栏
     「新增记事」/「保存记事」按钮）均跟随主题强调色（`main_border_color`），
     切换主题即时刷新。
4. **面板操作按钮**：中转站面板右上角一排操作按钮（原悬浮球右键菜单收进面板，悬浮球右键不再弹菜单）：
   - 「⚙ 设置」直接打开扁平化设置窗口（预制配色 6 套、11 种颜色项、大小/边框/阴影长度(0=无阴影)/
     透明度/刷新率、显示项开关、磁盘选择、贴边竖条档位、形状蒙版等，支持应用/恢复默认
     (二次确认)，保存即生效）；
   - 「📈 系统监视器」直接打开系统监视器（macOS 打开活动监视器、Linux 按桌面环境检测，
     可在「设置 → 系统监视器」自定义命令，单实例启动）；
   - 「⏻ 退出」电源图标：先隐藏窗口再安全退出。
   三个按钮图标使用用户提供的 SVG 形状（设置齿轮 / 性能统计折线 / 电源开关），
   渲染时把原图灰色 `#515151` 替换为主题强调色（`main_border_color`），
   切换主题即时刷新（编译需 Qt SVG 模块，`qtHaveModule(svg)` 自动启用）。
5. **配置**：`~/.popball2_config.ini`（可用环境变量 `POPBALL2_CONFIG` 指向其它路径）。
   颜色、大小、刷新间隔、显示哪些组件、贴边竖条、中转站默认视图、界面语言都在这里改，详见下表。
6. **界面语言**：设置窗口「窗 口 → 界面语言」可选 跟随系统（自动）/ 简体中文 / English。
   选择后点「应用」或「确定」，会提示「重启应用后生效」；重启后整个界面（设置窗口、
   数据中转站面板、预览、文本编辑器、提示框）都会切换。直接改配置文件 `[ui] language` 同样有效。
6b. **弹窗大小**：三种方式任选 —— 鼠标按住数据中转站弹窗的边缘拖拽（出现箭头光标即进入热区，
   左右/上下/四角都行，最小 240×260）；或在设置「中转站弹窗 → 弹窗宽×高」精确输入；
   想回到出厂尺寸点旁边的「恢复默认尺寸」（330×452）。改完点「应用」或「确定」立即生效，
   之后重开弹窗、重启应用都保持上次尺寸。
7. 桌面没开混成（compositing）时，可在配置里设 `shape_mask=1` 让窗口退回形状蒙版
   （圆球=圆形、竖条=圆角矩形），避免出现矩形黑框；`shape_mask=2` 强制关闭，`0` 为自动检测。
8. 取不到的指标（例如 Apple Silicon 的 CPU 频率）会自动隐藏，不会显示假数据。
9. macOS 上应用是「配件型」：不会出现在 Dock 和 Cmd+Tab 里，切换桌面、全屏应用之上
   都保持可见，就像 360 悬浮球那样。

#### 配置文件说明（~/.popball2_config.ini）

| 段 | 键 | 说明 |
|---|---|---|
| `[appearance]` | `width` `height` | 小球直径（默认 100） |
| | `aside_width` `aside_height` | 贴边竖条的宽与高（默认 30×100） |
| | `opacity` | 窗口不透明度（默认 0.91） |
| | `shadow_radius` / `shadow_color` | 阴影长度（0=无阴影）/ 阴影颜色 |
| | `shape` | 形态：0=圆球，1=贴边竖条（一般由程序自动切换） |
| | `main_color` `main_border_color` `main_border_width` | 球体背景 / 边框颜色 / 边框宽度 |
| | `mem_color` `swap_color` | 内存 / 交换面积图颜色 |
| | `cpu_usage_color` `cpu_usage_width` | CPU 曲线颜色 / 线宽 |
| | `cpu_freq_color` `cpu_temp_color` `net_speed_color` `disk_io_color` | 频率 / 温度 / 网速 / 磁盘文字颜色 |
| | `text_color` | 「悬浮球文字」总项（设置窗口里改它 = 一次同步上面四种文字颜色；单独改分项则分项优先） |
| | `charts_rows` | 图表历史点数（默认 32） |
| `[components_show]` | `cpu_temp_show` `cpu_freq_show` `net_speed_show` `disk_io_show` | 各指标显示开关（1 显示 / 0 隐藏） |
| `[disk]` | `disk_io_mode` / `disk_io_name` | 磁盘读写统计哪块盘：0=IO 最高的盘（默认），1=指定盘 |
| `[aside]` | `snap_to_edge` | 贴边竖条开关（1 启用 / 0 关闭，始终圆球） |
| | `aside_edge` | 当前吸附侧：10=左，11=右（程序自记） |
| | `corner_radius` | 竖条圆角半径（默认 8） |
| `[position]` | `x` `y` | 小球位置 |
| `[ui]` | `language` | 界面语言：0=跟随系统（默认）1=简体中文 2=English（设置窗「窗 口 → 界面语言」切换，重启生效） |
| | `ball_style` | 悬浮球形态：0=球形（默认）1=圆角矩形 2=直角方形 3=长条形 |
| | `dock_view_style` | 数据中转站默认布局：0=图标网格（默认）1=列表 2=详细 |
| | `dock_width` / `dock_height` | 数据中转站弹窗尺寸（px，默认 330×452；屏幕放不下自动收缩）。三处可改：①鼠标拖拽弹窗边缘实时调整（最小 240×260，松手即落盘）；②设置「中转站弹窗 → 弹窗宽×高」输入；③「恢复默认尺寸」按钮一键回到 330×452。均需点「应用」/「确定」才写入配置 |
| | `dock_position` | 弹窗优先展示位置：0=自动（空间大的一侧，默认）1=悬浮球右侧 2=悬浮球左侧 3=屏幕居中 |
| | `dock_density` | 弹窗内容密度：0=紧凑 1=标准（默认）2=宽松 |
| | `dock_opacity` | 弹窗背景不透明度（千分比 0~1000，默认 871 = 87.1%） |
| | `dock_remember_scroll` | 弹窗记住上次滚动位置：1=记住 0=不记住（默认，每次打开从顶部开始） |
| `[timer]` | `update_data_interval` / `update_ui_interval` | 数据采集 / 界面刷新间隔（ms，默认 450） |
| `[window]` | `shape_mask` | 形状蒙版：0=自动（X11 下检测混成）1=强制开启 2=关闭 |
| `[system_monitor]` | `cmd` | 面板「⚙ → 系统监视器」自定义命令，留空自动检测 |
| `[meta]` | `config_version` | 配置结构版本号（自动迁移用，一般不用手改） |

#### 多语言 / 本地化（i18n）

- **实现方案**：Qt 官方 i18n 管线 —— 源码里所有用户可见文本用 `tr()`（或
  `QCoreApplication::translate("Widget", …)` / `QT_TRANSLATE_NOOP("SettingsDialog", …)`
  处理无上下文上下文场景），`popball2.pro` 里 `TRANSLATIONS` 声明
  `popball2_zh_CN.ts` / `popball2_en.ts`，`CONFIG += lrelease embed_translations`
  把编译好的 `.qm` 嵌入可执行文件资源 `:/i18n/`，`main.cpp` 启动时按用户设置加载。
- **语言选择优先级**（`main.cpp`）：配置 `[ui] language` → `1` 强制简体中文、
  `2` 强制 English、`0`（默认）跟随系统 —— 按 `QLocale::system().uiLanguages()`
  逐级匹配（zh-CN → zh → …，与 Qt 官方示例一致）。
- **新增/修改界面文案后，更新翻译**（两行命令）：
  ```bash
  lupdate popball2.pro -ts popball2_zh_CN.ts popball2_en.ts   # 扫描源码里的 tr() 更新 .ts
  lrelease popball2_zh_CN.ts popball2_en.ts                    # 生成 .qm（构建时自动嵌入）
  ```
  然后重新 `qmake && make` 即可。macOS 上工具在 `/opt/homebrew/bin/`（qttools）。
- **翻译约定**：源代码字符串永远用中文（zh_CN.ts 的翻译即源串）；en.ts 填英文；
  语言选项名用自身语言显示（「简体中文」/「English」不翻译，业界惯例）；
  占位符 `%1/%2/%3` 保留；颜色项标签与主题名等「运行期变量」用
  `QT_TRANSLATE_NOOP` 登记上下文，保证 lupdate 能提取。
- **注意事项**：`tr()` 不能包运行时变量（lupdate 提取不到）——需要查表的数组
  字符串必须用 `QT_TRANSLATE_NOOP`；改了 `TRANSLATIONS` 后必须重新 `qmake`，
  `embed_translations` 才会把新 qm 编进资源。

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
