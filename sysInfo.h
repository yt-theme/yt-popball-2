#ifndef SYSINFO_H
#define SYSINFO_H

#include <qglobal.h>
#include <sys/sysinfo.h>
#include <QDebug>
#include <QDateTime>
#include <QFile>
#include <QString>
#include <QDir>
#include <QStringList>

#include <dirent.h>
#include <cstdio>

#include "struct_def.h"

class SysInfo {
private:
    // file obj
    QFile *_file_obj            = new QFile();

    // temperator path
    QStringList         _temp_paths = {};
    DIR *               _temp_parentDir;
    struct dirent *     _temp_entry;
    char                _temp_buffer[512];
    FILE *              _temp_fp;

    // mem & swap
    MemoryInfo memoryInfo;


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
};

#endif // SYSINFO_H
