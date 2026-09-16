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
#include <iphlpapi.h>            // 网速：GetIfTable2 / FreeMibTable
#include <netioapi.h>            // MIB_IF_ROW2 / IF 类型与状态常量
#include <rpcdce.h>              // CoInitializeSecurity 的 RPC_C_AUTHN/IMP 常量
#include <wbemidl.h>             // CPU 温度：WMI MSAcpi_ThermalZoneTemperature
#include <oaidl.h>               // VARIANT
#include <oleauto.h>
#endif

#include "struct_def.h"

class SysInfo {
private:
    // file obj
    QFile *_file_obj            = new QFile();

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

#endif // SYSINFO_H
