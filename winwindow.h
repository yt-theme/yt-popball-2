#ifndef WINWINDOW_H
#define WINWINDOW_H

#include <qglobal.h>
#include <QString>

#if defined(Q_OS_WIN)
// Windows：让悬浮球跟随所有虚拟桌面(Win+Ctrl+D 新建的桌面都能看到)。
// wId 传 WId(即 HWND)。失败静默降级 —— 不影响基本置顶功能。
void popballPinToAllDesktops(unsigned long long wId);

// 立即关闭显示器（等价于系统"关闭显示器"），任意鼠标/键盘活动由系统自动唤醒。
void popballWinScreenOff();

// 当前前台窗口是否处于全屏（全屏游戏 / 无边框窗口化 / 视频）。
// 判定：GetForegroundWindow 的矩形铺满其所在显示器（含无边框全屏与独占全屏）。
bool popballWinFullscreenAppActive();

// 进程可执行文件完整路径；失败返回空字符串。
QString popballWinProcessPath(qint64 pid);

// 结束进程（TerminateProcess）。失败时通过 errMsg 返回错误描述。
bool popballWinTerminateProcess(qint64 pid, QString *errMsg);
#endif

#endif // WINWINDOW_H
