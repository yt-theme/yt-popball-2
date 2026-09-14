QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

# 版本号（打包脚本会读取这里的值，macOS 也会用它填 CFBundleVersion）
VERSION = 1.0.0

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    config.cpp \
    main.cpp \
    settingsdialog.cpp \
    sysInfo.cpp \
    widget.cpp

HEADERS += \
    config.h \
    macro_def.h \
    settingsdialog.h \
    struct_def.h \
    sysInfo.h \
    widget.h

TRANSLATIONS += \
    popball2_zh_CN.ts
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

RESOURCES += \
    default_config.qrc \
    resources.qrc
