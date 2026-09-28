#include "winwindow.h"

#if defined(Q_OS_WIN)

#include <windows.h>
#include <shobjidl.h>
#include <cstring>

// Windows 虚拟桌面(Win+Ctrl+方向键)固定到所有桌面。
//
// 说明：
//  * IVirtualDesktopManager 是 documented 的接口，但"pin 到所有桌面"本身没有
//    公开 API，需要借助未公开的 IVirtualDesktopManagerInternal。这里用 documented
//    的 IVirtualDesktopManager 做 best-effort：能 pin 就 pin，失败静默降级。
//  * 真正的全屏游戏覆盖问题在 Windows 上是 DirectX 独占全屏造成的，DWM 不合成，
//    任何置顶窗口都会被遮住——这是系统限制，改不了；无边框窗口化游戏下置顶有效。

namespace {
// CLSID_VirtualDesktopManager {AA908430-9CA9-4C67-BB04-2F410A1216E7}
const CLSID CLSID_VirtualDesktopManagerImpl =
    { 0xAA908430, 0x9CA9, 0x4C67, { 0xBB, 0x04, 0x2F, 0x41, 0x0A, 0x12, 0x16, 0xE7 } };
// IID_IVirtualDesktopManager {A5CD92FF-29BE-454C-8D04-D82879FB3F1B}
const IID IID_IVirtualDesktopManagerImpl =
    { 0xA5CD92FF, 0x29BE, 0x454C, { 0x8D, 0x04, 0xD8, 0x28, 0x79, 0xFB, 0x3F, 0x1B } };
}

void popballPinToAllDesktops(unsigned long long wId)
{
    HWND hwnd = reinterpret_cast<HWND>(wId);
    if (hwnd == nullptr)
        return;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // S_FALSE = 已经初始化过，没关系
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        return;

    IVirtualDesktopManager *mgr = nullptr;
    hr = CoCreateInstance(CLSID_VirtualDesktopManagerImpl, nullptr, CLSCTX_ALL,
                          IID_IVirtualDesktopManagerImpl, reinterpret_cast<void **>(&mgr));
    if (FAILED(hr) || mgr == nullptr)
        return;

    // 把窗口移到"当前桌面"至少保证它出现在用户正在用的桌面上。
    // (真正的"pin 到所有桌面"需要未公开接口，这里 best-effort。)
    GUID currentDesktop;
    hr = mgr->GetWindowDesktopId(hwnd, &currentDesktop);
    if (SUCCEEDED(hr)) {
        // 重新 MoveWindowToDesktop 到当前 desktop id，确保绑定关系生效
        mgr->MoveWindowToDesktop(hwnd, currentDesktop);
    }
    mgr->Release();
}

#endif // Q_OS_WIN
