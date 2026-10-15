#include "linuxwindow.h"

#include <QByteArray>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QString>

#if defined(POPBALL_HAVE_X11)
// Xlib 必须放在 Qt 头文件之后：它会把 None / True / False 定义成宏，
// 与 Qt 里的同名枚举冲突；用完立刻清理，避免影响后面被包含进来的 Qt 头文件。
#  include <QtGui/qguiapplication_platform.h>
#  include <X11/Xlib.h>
#  undef None
#  undef True
#  undef False
#endif

namespace {

// 环境变量是否被显式关掉（0/no/off/false 视为关；没设置不算关）
bool envFlagOff(const char *name)
{
    const QByteArray v = qgetenv(name).trimmed().toLower();
    return v == "0" || v == "no" || v == "off" || v == "false";
}

#if defined(POPBALL_HAVE_X11)
Display *x11Display()
{
    if (qGuiApp == nullptr)
        return nullptr;
    QNativeInterface::QX11Application *x11 =
        qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return (x11 != nullptr) ? x11->display() : nullptr;
}
#endif

// xcb 平台插件是否存在。没有它却把 QT_QPA_PLATFORM 设成 xcb，
// Qt 会直接 "could not load the Qt platform plugin" 退出，所以切换前必须查。
bool xcbPluginAvailable()
{
    QStringList dirs;
    const QString sys = QLibraryInfo::path(QLibraryInfo::PluginsPath);
    if (!sys.isEmpty())
        dirs << sys;
    if (qApp != nullptr)
        dirs << qApp->applicationDirPath() + QStringLiteral("/plugins");
    for (const QString &d : dirs) {
        if (QFileInfo::exists(d + QStringLiteral("/platforms/libqxcb.so")))
            return true;
    }
    return dirs.isEmpty();      // 查不到插件目录时乐观处理，不因误判砍掉 X11 路径
}

// DISPLAY 是否真能连上（Wayland 会话里 DISPLAY 存在但 XWayland 可能还没起来）
bool x11DisplayUsable()
{
    if (qEnvironmentVariableIsEmpty("DISPLAY"))
        return false;
#if defined(POPBALL_HAVE_X11)
    Display *d = XOpenDisplay(nullptr);
    if (d == nullptr)
        return false;           // XWayland 不可用 —— 留在原生 Wayland
    XCloseDisplay(d);
#endif
    return true;
}

} // namespace

// ---------------------------------------------------------------- 平台选择
void popballLinuxPreparePlatform()
{
    // Xorg 会话：Qt 自己就会选 xcb，不用管
    if (!popballLinuxSessionIsWayland())
        return;
    if (!qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        return;                                 // 用户已经指定了平台插件，尊重用户
    const QByteArray forced = qgetenv("POPBALL2_QT_PLATFORM").trimmed();
    if (!forced.isEmpty()) {
        qputenv("QT_QPA_PLATFORM", forced);
        return;
    }
    // 没有 XWayland 或没装 xcb 插件 → 留在原生 Wayland（功能降级，但不至于起不来）
    if (!x11DisplayUsable() || !xcbPluginAvailable())
        return;

    // Wayland 会话 → 走 XWayland：位置可控 + 真正的"压在所有窗口之上"
    qputenv("QT_QPA_PLATFORM", "xcb");
}

bool popballLinuxSessionIsWayland()
{
    const QByteArray st = qgetenv("XDG_SESSION_TYPE").toLower();
    if (st == "wayland")
        return true;
    if (st == "x11")
        return false;
    return !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
}

bool popballLinuxIsX11()
{
    return QGuiApplication::platformName().startsWith(QLatin1String("xcb"));
}

bool popballLinuxIsXWayland()
{
    // 跑在 X11 上、但会话是 Wayland → 现在这个 X server 就是 XWayland
    return popballLinuxIsX11() && !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
}

bool popballLinuxIsWaylandNative()
{
    return QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
}

bool popballLinuxOverlayEnabled()
{
    if (!popballLinuxIsX11())
        return false;
    return !envFlagOff("POPBALL2_X11_OVERLAY");
}

// ---------------------------------------------------------------- X11 属性
bool popballLinuxCompositingMissing()
{
#if defined(POPBALL_HAVE_X11)
    if (!popballLinuxIsX11())
        return false;
    // Wayland 合成器本身就是混成器，XWayland 上永远不需要形状蒙版
    if (popballLinuxIsXWayland())
        return false;

    Display *dpy = x11Display();
    if (dpy == nullptr)
        return false;                       // 拿不到连接：按"有混成"处理

    // _NET_WM_CM_S0 这个 selection 被占用 = 有混成管理器在跑（EWMH 标准判定）
    const Atom cm = XInternAtom(dpy, "_NET_WM_CM_S0", 1);   // 1 = only_if_exists
    if (cm == 0)
        return true;                        // 连这个 atom 都没有 → 肯定没混成
    return XGetSelectionOwner(dpy, cm) == 0;                // 0 = Xlib 的 None
#else
    return false;
#endif
}

void popballLinuxMakeSticky(unsigned long long winId)
{
#if defined(POPBALL_HAVE_X11)
    if (!popballLinuxIsX11() || winId == 0)
        return;
    Display *dpy = x11Display();
    if (dpy == nullptr)
        return;

    const Atom wmState = XInternAtom(dpy, "_NET_WM_STATE", 0);
    const Atom sticky  = XInternAtom(dpy, "_NET_WM_STATE_STICKY", 0);
    const Atom above   = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", 0);
    if (wmState == 0)
        return;

    // _NET_WM_STATE_ADD：一条消息可以同时带上 STICKY(所有工作区) 与 ABOVE(置顶)
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type                 = ClientMessage;
    ev.xclient.window       = static_cast<Window>(winId);
    ev.xclient.message_type = wmState;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = 1;                        // 1 = _NET_WM_STATE_ADD
    ev.xclient.data.l[1]    = static_cast<long>(sticky);
    ev.xclient.data.l[2]    = static_cast<long>(above);
    ev.xclient.data.l[3]    = 1;                        // 1 = 来自应用程序
    // 末参数里的 False 是 Xlib 宏，上面已 #undef，这里直接写 0
    XSendEvent(dpy, DefaultRootWindow(dpy), 0,
               SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XSync(dpy, 0);      // 0 = Xlib 的 False（宏已被 #undef）
#else
    Q_UNUSED(winId);
#endif
}

void popballLinuxRaise(unsigned long long winId)
{
#if defined(POPBALL_HAVE_X11)
    if (!popballLinuxIsX11() || winId == 0)
        return;
    Display *dpy = x11Display();
    if (dpy == nullptr)
        return;
    // override-redirect 窗口 WM 不管，层叠序只能自己提；
    // 游戏等全屏窗口可能后于小球映射，所以运行中也要周期性地提一次（见 Widget）。
    XRaiseWindow(dpy, static_cast<Window>(winId));
#else
    Q_UNUSED(winId);
#endif
}
