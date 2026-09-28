#ifndef WINWINDOW_H
#define WINWINDOW_H

#include <qglobal.h>

#if defined(Q_OS_WIN)
// Windows：让悬浮球跟随所有虚拟桌面(Win+Ctrl+D 新建的桌面都能看到)。
// wId 传 WId(即 HWND)。失败静默降级 —— 不影响基本置顶功能。
void popballPinToAllDesktops(unsigned long long wId);
#endif

#endif // WINWINDOW_H
