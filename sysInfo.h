#ifndef SYSINFO_H
#define SYSINFO_H

#include <qglobal.h>
#include <QDebug>
#include <QDateTime>
#include <QFile>
#include <QString>
#include <QDir>
#include <QStringList>
#include <QList>
#include <QVector>
#include <QHash>
#include <QPair>

#include <cstdio>

#if defined(Q_OS_LINUX)
#include <sys/sysinfo.h>
#include <dirent.h>
#endif

#if defined(Q_OS_MACOS)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_dl.h>
#endif

#if defined(Q_OS_WIN)
// 必须在 <windows.h> 之前定义，才能启用 GetIfTable2 等较新的 API
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX                 // 避免 min/max 宏与 qMax 冲突
#endif
#include <windows.h>
// 必须先包含 Winsock 的 IP 定义头：
// MinGW-w64 的 netioapi.h 把 MIB_IF_ROW2 / MIB_IF_TABLE2 / GetIfTable2 / FreeMibTable
// 全包在 `#ifdef _WS2IPDEF_` 里，而它只有在「自己是被首次包含 + iphlpapi.h 的 include
// guard 还没定义」时才会去 include <ws2ipdef.h>。若像下面这样先 iphlpapi.h 再 netioapi.h，
// 就会走 netioapi.h 里 `#ifdef __IPHLPAPI_H__` 那条分支，ws2ipdef.h 从未被包含，
// _WS2IPDEF_ 未定义 → 新 API 全部消失（报 MIB_IF_TABLE2 / GetIfTable2 未声明）。
// MSVC 的 SDK 头内部已处理这层依赖，所以原先在 MSVC 上不会暴露。
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>            // 网速：GetIfTable2 / FreeMibTable
#include <netioapi.h>            // MIB_IF_ROW2 / IF 类型与状态常量
#include <rpcdce.h>              // CoInitializeSecurity 的 RPC_C_AUTHN/IMP 常量
#include <wbemidl.h>             // CPU 温度：WMI MSAcpi_ThermalZoneTemperature
#include <oaidl.h>               // VARIANT
#include <oleauto.h>
#include <winhttp.h>             // LibreHardwareMonitor 的 Web 服务（读真实 CPU 核心温度）
#include <winioctl.h>            // CTL_CODE / METHOD_BUFFERED（WinRing0 的 IOCTL）
#include <winsvc.h>              // 安装/启动 WinRing0 内核驱动服务
#include <shellapi.h>            // ShellExecuteExW：非管理员时的 UAC 自提权装驱动
#include <pdh.h>                 // 磁盘 IO：PDH 性能计数器（比 WMI 快两个数量级）
#endif

#include "struct_def.h"

class SysInfo {
private:
#if defined(Q_OS_LINUX)
    // CPU 温度候选文件（启动时探测一次，之后只读这些文件）
    QStringList         _temp_paths = {};
#endif

    // mem & swap
    MemoryInfo memoryInfo;
    bool        memOk           = false;
    bool        swapOk          = false;

    // cpu
    double cpuTemperature       = 0.0;
    bool   cpuTemperatureOk     = false;
    double cpuFreq              = 0.0;   // MHz
    bool   cpuFreqOk            = false;

    double cpuUsageTotalLast    = 0.0;
    double cpuUsageIdleLast     = 0.0;
    double cpuUsage             = 0.0;
    bool   cpuUsageHasPrev      = false;

    // last update time
    qlonglong lastUpdateTime    = 0;

    // net
    quint64 receive             = 0;
    quint64 receive_last        = 0;
    quint64 transmit            = 0;
    quint64 transmit_last       = 0;
    bool    netInited           = false;   // 首帧网速保护：第一帧只记基准，不当作速率

    // disk I/O（读写字节数，与网速一样用差值算速度）
    // 每个磁盘单独记累计值，再按"指定盘 / IO 最高的盘"选出生效盘。
    struct DiskIoStat {
        quint64 read_total  = 0;   // 累计读字节（单调增长，Linux/macOS 用）
        quint64 write_total = 0;   // 累计写字节（单调增长，Linux/macOS 用）
        quint64 read_speed  = 0;   // 本间隔读字节数（差值，即生效速度来源）
        quint64 write_speed = 0;   // 本间隔写字节数（差值）
        bool    seen        = false;
    };
    QHash<QString, DiskIoStat> diskStats;     // 设备名 -> 统计
    QStringList diskAvailableNames;           // 最近一次能取到的磁盘名列表（供设置 UI）
    QHash<QString, QString> diskLabelMap;     // 设备名 -> 友好显示名（如 disk0 · APPLE SSD）
    QString  diskActiveName;                  // 当前生效的磁盘名
    quint64  disk_read  = 0;                  // 生效盘：本间隔读字节数（差值）
    quint64  disk_write = 0;                  // 生效盘：本间隔写字节数（差值）
    qint8    diskSelectMode = 0;              // 0 = IO 最高的盘（默认）, 1 = 指定盘
    QString  diskSelectName;                  // mode=1 时指定的磁盘名
    bool     diskIoOk = false;                // 当前平台能否取到磁盘 IO
#if defined(Q_OS_WIN)
    qlonglong diskLastSampleMs = 0;           // 上次采样时间戳（Windows 用：把速率换算成字节数）
    // PDH 磁盘性能计数器（读 PhysicalDisk 各实例的每秒读/写字节数）。
    // 之前用 WMI Win32_PerfFormattedData_PerfDisk_PhysicalDisk：首次查询 2~3 秒、
    // 之后每次仍要 300ms+，会卡住 450ms 周期的采样线程，而且初始化一旦失败就
    // 永久跳过（wmiTried），磁盘速度从此恒为 0。PDH 每次查询 <10ms，不阻塞 UI。
    struct PdhDiskCounter {
        QString      name;      // PDH 实例名（如 "0 C:"，与 WMI 一致，可直接用于 disk_io_name）
        PDH_HCOUNTER hRead  = nullptr;
        PDH_HCOUNTER hWrite = nullptr;
    };
    QVector<PdhDiskCounter> pdhDisks;         // 各物理盘对应的读/写计数器
    PDH_HQUERY              pdhQuery  = nullptr;
    bool                    pdhReady  = false;   // PDH 查询已初始化成功
    bool                    pdhTried  = false;   // 是否已尝试过初始化（失败后隔段时间重试）
    qlonglong               pdhTryMs  = 0;       // 上次初始化尝试时间（重试退避用）
#endif

#if defined(Q_OS_MACOS)
    // AppleSMC：用于读取 CPU 温度（Intel / Apple Silicon 通用）
    unsigned int   _smcConn     = 0;     // io_connect_t
    bool           _smcOpen     = false;
    QList<quint32> _smcCpuKeys  = {};    // 启动时枚举出的 CPU 温度键
#endif

    // 公共收尾：按各盘本间隔 (读,写) 差值，按"指定盘 / IO 最高的盘"选出生效盘
    void finalizeDiskIo(const QHash<QString, QPair<quint64, quint64>> &delta);

public:
    SysInfo();
    ~SysInfo();
    // 平台相关的初始化探测
    void checkTemperatorFilePath();

    // call system api to get sys info
    void updateSysinfo();

    qulonglong getMemTotal();
    qulonglong getMemUsed();
    qulonglong getMemFree();
    qulonglong getSwapTotal();
    qulonglong getSwapUsed();
    qulonglong getSwapFree();
    double  getCpuFreq();
    double  getCpuUsage();
    double  getCpuTemperature();
    qulonglong getReceive();
    qulonglong getTransmit();
    qulonglong getDiskReadBytes();
    qulonglong getDiskWriteBytes();
    // 磁盘选择：mode 0=IO最高的盘, 1=指定盘(name)
    void    setDiskSelection(qint8 mode, const QString &name);
    QString getDiskActiveName() const;
    QStringList getDiskNames() const;
    // 磁盘的友好显示名（取不到时返回原名本身）
    QString getDiskLabel(const QString &name) const;

    // 各项指标在当前平台/当前发行版上是否可用（不可用则 UI 不显示，避免假数据）
    bool isMemAvailable();
    bool isSwapAvailable();
    bool isCpuFreqAvailable();
    bool isCpuTemperatureAvailable();
    bool isDiskIoAvailable();
};

#if defined(Q_OS_WIN)
// 以当前(管理员)进程身份把 WinRing0 驱动注册成内核服务并启动。
// 供 main.cpp 处理 "--install-winring0-driver <sys路径>" 参数使用：
// 正常进程是非管理员时，通过 UAC 自提权调用自身带此参数，完成一次性驱动安装。
bool installWinRing0DriverService(const QString &sysPath);
#endif

#endif // SYSINFO_H
