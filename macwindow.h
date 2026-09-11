#ifndef MACWINDOW_H
#define MACWINDOW_H

#include <qglobal.h>

#if defined(Q_OS_MACOS)
// macOS 上让悬浮球更"桌面挂件"一些：
//   * 出现在所有桌面(Space)上，切换桌面不会消失
//   * 应用失去焦点时不自动隐藏
//   * 全屏应用之上也可见
// wId 传 QWidget::winId()。
void popballApplyMacWindowBehavior(unsigned long long wId);

// 把应用切成「配件型(accessory)」：不在 Dock 显示图标，也不出现在 Cmd+Tab 里，
// 但窗口照常显示在最上层 —— 这正是 360 悬浮球那类桌面挂件的行为。
// 建议在 QApplication 构造之后尽快调用。
void popballHideFromDock();
#endif

#endif // MACWINDOW_H
