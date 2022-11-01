#ifndef SYSINFO_H
#define SYSINFO_H

#include <qglobal.h>
#include <sys/sysinfo.h>
#include <QDebug>
#include <QDateTime>
#include <QFile>
#include <QString>
#include <QDir>

class SysInfo {
private:
    // file obj
    QFile *_file_obj            = new QFile();

    // temperator path
    QStringList temperatorPaths = {};

    // sys state mem & swap
    struct sysinfo sys_info;

    // cpu
    double cpuTemperature       = 0.0;
    double cpuFreq              = 0.0; // mhz

    double cpuUsage_total_last  = 0.0;
    double cpuUsage_use_last    = 0.0;
    double cpuUsage             = 0.0;

    // last update time
    qlonglong lastUpdateTime    = 0;

    // net
    quint64 receive             = 0;
    quint64 receive_last        = 0;
    quint64 transmit            = 0;
    quint64 transmit_last       = 0;

public:
    SysInfo();
    ~SysInfo();
    // check temperator file path
    void checkTemperatorFilePath();

    // call system api to get sys info
    void updateSysinfo();

    quint64 getMemTotal();
    quint64 getMemFree();
    quint64 getSwapTotal();
    quint64 getSwapFree();
    double  getCpuFreq();
    double  getCpuUsage();
    double  getCpuTemperature();
    quint64 getReceive();
    quint64 getTransmit();
};

#endif // SYSINFO_H
