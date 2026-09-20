QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

# 剪贴板历史持久化用 QtSql(QSQLITE)。
# 用 qtHaveModule 守卫：万一目标环境没装 Qt6 的 Sql 开发包，也照样能构建/运行，
# 只是历史不落盘（代码里按 POPBALL2_HAVE_QT_SQL 退化）。
# 注意：Linux 运行时还需要 QSQLITE 插件包（Debian/Ubuntu: libqt6sql6-sqlite）。
qtHaveModule(sql) {
    QT += sql
    DEFINES += POPBALL2_HAVE_QT_SQL
}

# 视频悬停预览播放用 QtMultimedia。同样用 qtHaveModule 守卫：
# 没有该模块时编译不进去，运行时退化成"静态封面 + 提示双击用系统播放器打开"。
# 注意：Linux 上播放还需要 gstreamer 及其插件（见 DEPENDENCIES.md）。
qtHaveModule(multimedia) {
    QT += multimedia
    DEFINES += POPBALL2_HAVE_QT_MULTIMEDIA
}

# 中转站面板右上角按钮用 SVG 图标（用户提供的线性图标），需 QtSvg 渲染。
# qtHaveModule 守卫：缺模块时退化为自绘图标（见 PopDock::svgThemeIcon 的回退分支）。
qtHaveModule(svg) {
    QT += svg
    DEFINES += POPBALL2_HAVE_QT_SVG
}

CONFIG += c++17

# 版本号：自动从 package.json 读取（单一来源，改 package.json 即全局生效）
VERSION = $$system(grep version $$PWD/package.json 2>/dev/null | head -1 | sed 's/[^0-9.]//g')
isEmpty(VERSION): VERSION = 0.0.0

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    clipstore.cpp \
    config.cpp \
    main.cpp \
    settingsdialog.cpp \
    sysInfo.cpp \
    widget.cpp \
    popdock.cpp

HEADERS += \
    clipstore.h \
    config.h \
    macro_def.h \
    settingsdialog.h \
    struct_def.h \
    sysInfo.h \
    widget.h \
    popdock.h

TRANSLATIONS += \
    popball2_zh_CN.ts \
    popball2_en.ts
CONFIG += lrelease
CONFIG += embed_translations

# Default rules for deployment.
# 安装位置默认 /usr/local，可在 qmake 时覆盖：qmake PREFIX=/usr
isEmpty(PREFIX): PREFIX = /usr/local
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = $$PREFIX/bin
win32: target.path = $$PREFIX/bin
!isEmpty(target.path): INSTALLS += target

# macOS 应用图标（Linux 侧的图标由 package.sh 安装到 hicolor 主题目录）
macx: ICON = resources/popball2.icns

DISTFILES +=

# ---------------- 平台相关 ----------------

# macOS：AppleSMC(CPU 温度) 需要 IOKit；桌面窗口行为用一小段 Objective-C++
macx: LIBS += -framework IOKit
macx: OBJECTIVE_SOURCES += macwindow.mm
macx: HEADERS += macwindow.h

# Linux/X11：检测桌面有没有开混成(compositing)，没开时改用形状蒙版避免黑色矩形。
# 只有在系统装了 libX11 开发包(pkg-config 能找到 x11)时才启用，找不到也能正常构建，
# 只是自动检测失效 —— 这时可以在配置里用 shape_mask=1 手动打开蒙版。
unix:!macx {
    packagesExist(x11) {
        CONFIG += link_pkgconfig
        DEFINES += POPBALL_HAVE_X11
        PKGCONFIG += x11
    }
}

# Windows：
#   * iphlpapi —— 网速 GetIfTable2；advapi32 —— 注册表读 CPU 频率
#   * ole32 / oleaut32 / wbemuuid —— CPU 温度经 WMI(MSAcpi_ThermalZoneTemperature)
#   * _WIN32_WINNT=0x0601(Win7+) 启用 GetIfTable2 等较新 API
win32 {
    DEFINES += WINVER=0x0601
    DEFINES += _WIN32_WINNT=0x0601
    LIBS += -liphlpapi -ladvapi32 -lole32 -loleaut32 -lwbemuuid

    # Windows 应用图标（可选，存在才生效）
    exists(resources/popball2.ico) {
        RC_ICONS = resources/popball2.ico
    }
}

RESOURCES += \
    default_config.qrc \
    resources.qrc \
    icons.qrc
