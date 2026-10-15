#ifndef LINUXWINDOW_H
#define LINUXWINDOW_H

// Linux 桌面适配（Xorg / Wayland）。
//
// 与 winwindow.h / macwindow.h 对应：把"平台相关的窗口把戏"集中在一个文件里，
// widget.cpp / popdock.cpp 只管调用，不再各自 #include <X11/Xlib.h>。
//
// ---------------------------------------------------------------- 为什么需要它
// 悬浮球这类"要摆在屏幕任意位置 + 永远压在所有窗口（含全屏游戏）之上"的窗口，
// 在原生 Wayland 下做不出来：
//   * xdg-shell 协议不允许客户端给顶层窗口定位 —— QWidget::move() 只是自己在记账，
//     合成器根本不理，于是"球拖不动"；
//   * Wayland 也没有给普通顶层窗口的"始终置顶"语义 —— 于是"球浮不到最上面"。
//
// 因此在 Wayland 会话里，程序会主动切到 X11 平台插件（走 XWayland），
// 再用 X11 的 override-redirect（绕过窗口管理器）窗口实现真正的置顶：
//   * KWin / Mutter / wlroots 都把"不受管理的窗口"放在层叠最顶层，
//     全屏游戏也压不住它（这正是"全屏游戏里看不到球"的解法）；
//   * override-redirect 窗口的位置由程序自己 XMoveWindow，拖动立即生效。
//
// 降级策略（XWayland 不可用 / 用户强制 wayland 平台）见 widget.cpp 的 Wayland 分支：
// 拖动交给合成器（QWindow::startSystemMove），层级由合成器决定。
//
// 环境变量（排障 / 兼容特殊环境用）：
//   POPBALL2_QT_PLATFORM=wayland   强制留在原生 Wayland（不做 XWayland 切换）
//   POPBALL2_QT_PLATFORM=xcb       强制 X11
//   POPBALL2_X11_OVERLAY=0         关掉 override-redirect，退回"受窗口管理器管理 +
//                                  _NET_WM_STATE_ABOVE/STICKY"的普通置顶
//   QT_QPA_PLATFORM                用户自己指定的平台插件优先，本模块不再干预

#include <QtGlobal>

// 必须在 QGuiApplication / QApplication 构造**之前**调用：
// Wayland 会话下把 Qt 平台插件切到 xcb（XWayland）。
void popballLinuxPreparePlatform();

// 当前会话是否为 Wayland（与 Qt 此刻用哪个平台插件无关）
bool popballLinuxSessionIsWayland();
// 当前实际是否运行在 X11 上（Xorg 或 XWayland）
bool popballLinuxIsX11();
// 当前是否运行在 XWayland（X11 客户端 + Wayland 合成器）—— 混成必定存在
bool popballLinuxIsXWayland();
// 当前是否"原生 Wayland"（XWayland 不可用时的降级路径）
bool popballLinuxIsWaylandNative();
// 是否启用 override-redirect 置顶（X11 且未被 POPBALL2_X11_OVERLAY=0 关掉）
bool popballLinuxOverlayEnabled();

// X11 是否没有开混成(compositing)：没开时半透明会露出黑框，需要形状蒙版兜底
bool popballLinuxCompositingMissing();

// 受窗口管理器管理时：跟随所有工作区(STICKY) + 置顶(ABOVE)
void popballLinuxMakeSticky(unsigned long long winId);
// 把自己提到层叠最上面（对 override-redirect 窗口同样有效）
void popballLinuxRaise(unsigned long long winId);

#endif // LINUXWINDOW_H
