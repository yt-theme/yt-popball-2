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

#if defined(Q_OS_MACOS)
    // AppleSMC：用于读取 CPU 温度（Intel / Apple Silicon 通用）
    unsigned int   _smcConn     = 0;     // io_connect_t
    bool           _smcOpen     = false;
    QList<quint32> _smcCpuKeys  = {};    // 启动时枚举出的 CPU 温度键
#endif

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

    // 各项指标在当前平台/当前发行版上是否可用（不可用则 UI 不显示，避免假数据）
    bool isMemAvailable();
    bool isSwapAvailable();
    bool isCpuFreqAvailable();
    bool isCpuTemperatureAvailable();
};

#endif // SYSINFO_H
