#include "macwindow.h"

#if defined(Q_OS_MACOS)

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#include <QProcess>
#include <cstdint>
#include <vector>
#include <libproc.h>     // proc_pidpath：直接问内核要进程的可执行文件路径

QString popballMacProcessPath(qint64 pid)
{
    if (pid <= 0)
        return QString();

    // 首选 proc_pidpath：纯内核查询，微秒级，不会像启 ps 那样卡住主线程。
    char buf[PROC_PIDPATHINFO_MAXSIZE] = { 0 };
    const int rc = proc_pidpath(int(pid), buf, sizeof(buf));
    if (rc > 0)
        return QString::fromLocal8Bit(buf, rc);

    // 少数情况下（权限 / 已退出）读不到，再退回 ps（可能慢 ~1s，只在这类边缘情况发生）
    QProcess ps;
    ps.start(QStringLiteral("/bin/ps"),
             { QStringLiteral("-p"), QString::number(pid),
               QStringLiteral("-o"), QStringLiteral("comm=") });
    if (!ps.waitForStarted(800) || !ps.waitForFinished(1500))
        return QString();
    return QString::fromLocal8Bit(ps.readAllStandardOutput()).trimmed();
}

void popballApplyMacWindowBehavior(unsigned long long wId)
{
    NSView *view = (NSView *)(uintptr_t)wId;
    if (view == nil)
        return;

    NSWindow *win = [view window];
    if (win == nil)
        return;

    // 跟随所有桌面(Space) + 切桌面时保持不动 + 全屏应用之上也可见。
    //
    // 注意：NSWindowCollectionBehaviorCanJoinAllSpaces 与
    // NSWindowCollectionBehaviorMoveToActiveSpace 是互斥的，同时设置会抛
    // NSInternalInconsistencyException 直接崩溃。Qt 默认会给窗口设置
    // MoveToActiveSpace（表示"只存在于当前 Space"），所以这里必须先把它清掉。
    win.collectionBehavior = (win.collectionBehavior
        & ~NSWindowCollectionBehaviorMoveToActiveSpace)
        | NSWindowCollectionBehaviorCanJoinAllSpaces
        | NSWindowCollectionBehaviorStationary
        | NSWindowCollectionBehaviorFullScreenAuxiliary;

    // 应用失去焦点时不要自动隐藏 —— 悬浮球要一直看得见
    [win setHidesOnDeactivate:NO];
}

void popballHideFromDock()
{
    // NSApplicationActivationPolicyAccessory：
    //   * 不出现在 Dock
    //   * 不在 Cmd+Tab 应用切换器里
    //   * 但窗口仍可正常显示在最上层
    // 相比在 Info.plist 里写 LSUIElement，这种方式对开发期直接跑二进制也生效。
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
}

// 从 CGWindowList 的窗口信息字典里取窗口矩形（kCGWindowBounds 是 X/Y/Width/Height 字典）。
static bool popballDictCGRect(CFDictionaryRef win, CGRect *outRect)
{
    CFDictionaryRef b = (CFDictionaryRef)CFDictionaryGetValue(win, kCGWindowBounds);
    if (b == nullptr)
        return false;
    return CGRectMakeWithDictionaryRepresentation(b, outRect);
}

bool popballMacFullscreenAppActive()
{
    @autoreleasepool {
        NSRunningApplication *front = [[NSWorkspace sharedWorkspace] frontmostApplication];
        if (front == nil)
            return false;
        const int frontPid = (int)front.processIdentifier;

        const CGRect screen = CGDisplayBounds(CGMainDisplayID());
        CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly,
                                                     kCGNullWindowID);
        if (list == nullptr)
            return false;

        bool fullscreen = false;
        const CFIndex n = CFArrayGetCount(list);
        for (CFIndex i = 0; i < n && !fullscreen; ++i) {
            CFDictionaryRef win = (CFDictionaryRef)CFArrayGetValueAtIndex(list, i);

            int pid = 0;
            CFNumberRef pidRef = (CFNumberRef)CFDictionaryGetValue(win, kCGWindowOwnerPID);
            if (pidRef != nullptr)
                CFNumberGetValue(pidRef, kCFNumberIntType, &pid);
            if (pid != frontPid)
                continue;

            // 只关心普通窗口层（0 层）：菜单栏/状态栏/悬浮条等更高层窗口不算
            int layer = 0;
            CFNumberRef layerRef = (CFNumberRef)CFDictionaryGetValue(win, kCGWindowLayer);
            if (layerRef != nullptr)
                CFNumberGetValue(layerRef, kCFNumberIntType, &layer);
            if (layer != 0)
                continue;

            CGRect bounds = CGRectZero;
            if (!popballDictCGRect(win, &bounds))
                continue;

            // 铺满主屏（留几像素容差，兼容不同分辨率缩放导致的小数误差）
            if (bounds.size.width  >= screen.size.width  - 16.0
                && bounds.size.height >= screen.size.height - 16.0)
            {
                fullscreen = true;
            }
        }
        CFRelease(list);
        return fullscreen;
    }
}

#endif // Q_OS_MACOS
