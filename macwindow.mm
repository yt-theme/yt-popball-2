#include "macwindow.h"

#if defined(Q_OS_MACOS)

#import <Cocoa/Cocoa.h>
#include <cstdint>

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

#endif // Q_OS_MACOS
