#include "sysInfo.h"

SysInfo::SysInfo()
{
    this->checkTemperatorFilePath();
    this->updateSysinfo();


}

SysInfo::~SysInfo()
{
    delete this->_file_obj;
}

void SysInfo::checkTemperatorFilePath()
{
    QString hwmonDirPath = "/sys/class/thermal/";
    QDir hwmonDir(hwmonDirPath);
    QStringList hwmonFiles = hwmonDir.entryList();
    for (int i=0; i<hwmonFiles.size(); i++)
    {
        if ((hwmonFiles[i] != ".") && (hwmonFiles[i] != "..") && (hwmonFiles[i].split("_zone")[0] == "thermal"))
        {
            // read hwmonX files
            this->_file_obj->setFileName(hwmonDirPath + hwmonFiles[i] + "/temp" );
            if (this->_file_obj->exists() == true)
            {
                this->temperatorPaths.append(this->_file_obj->fileName());
            }
        }
    }
}

void SysInfo::updateSysinfo()
{

    // ##################################################################################################
    //                          mem swap sys info
    // ##################################################################################################
    if (sysinfo(&sys_info) != 0)
    {
        // err
    }

    // #########################################QString filePath : this->temperatorPaths#########################################################
    //                          cpu temperature
    // ##################################################################################################
    double tmp_cpuTemperature_max = 0.0;
    for (quint32 i=0; i<this->temperatorPaths.size(); i++)
    {
        this->_file_obj->setFileName(temperatorPaths[i]);
        this->_file_obj->open(QIODevice::ReadOnly|QIODevice::Text);
        double val = this->_file_obj->readLine().replace("\t", "").toDouble();
        this->_file_obj->close();
        if (val > tmp_cpuTemperature_max)
        {
            tmp_cpuTemperature_max = val;
        }
    }
    this->cpuTemperature = tmp_cpuTemperature_max / 1000;


    // ##################################################################################################
    //                          cpu info freq
    // ##################################################################################################
    this->_file_obj->setFileName("/proc/cpuinfo");
    this->_file_obj->open(QIODevice::ReadOnly|QIODevice::Text);
    QString cpuInfoStr = this->_file_obj->readAll().replace("\t", "");
    this->_file_obj->close();
    QStringList cpuInfoStr_arr = cpuInfoStr.split("\n");

    double tmp_maxFreq = 0.0;
    for (quint32 i=0; i<cpuInfoStr_arr.size(); i++)
    {
        if (cpuInfoStr_arr[i].contains("cpu MHZ"))
        {
            double val = cpuInfoStr_arr[i].split(":")[1].trimmed().toDouble();
            if (val > tmp_maxFreq) {
                tmp_maxFreq = val;
            }
        }
    }
    this->cpuFreq = tmp_maxFreq;

    // ##################################################################################################
    //                          cpu usage
    // ##################################################################################################
    this->_file_obj->setFileName("/proc/stat");
    this->_file_obj->open(QIODevice::ReadOnly|QIODevice::Text);
    QString cpuUsageStr_1st = _file_obj->readLine().replace("\t", "").replace("\n", "");
    this->_file_obj->close();

    // cpu usage data split
    QStringList cpuUsageStr_1st_arr = cpuUsageStr_1st.split(" ", Qt::SkipEmptyParts);
    double tmp_cpuUsage_total = 0.0;
    for (quint32 i = 0; i < cpuUsageStr_1st_arr.size(); i++)
    {
        if (i == 0) continue;
        tmp_cpuUsage_total += cpuUsageStr_1st_arr[i].toDouble();
    }
    this->cpuUsage_total = tmp_cpuUsage_total;
    this->cpuUsage_idle = cpuUsageStr_1st_arr[4].toDouble();

    // cpu usage
    this->cpuUsage = (
                (this->cpuUsage_total - this->cpuUsage_total_last) -
                (this->cpuUsage_idle - this->cpuUsage_total_last)
                ) / (
                    this->cpuUsage_total - this->cpuUsage_total_last
                ) * 100;

    // store last
    this->cpuUsage_total_last = this->cpuUsage_total;
    this->cpuUsage_idle_last = this->cpuUsage_idle;

    // ##################################################################################################
    //                          net
    // ##################################################################################################
    this->_file_obj->setFileName("/proc/net/dev");
    this->_file_obj->open(QIODevice::ReadOnly|QIODevice::Text);
    QString netDataStr = this->_file_obj->readAll().replace("\t", "");
    this->_file_obj->close();
    QStringList netDataStr_arr = netDataStr.split("\n", Qt::SkipEmptyParts);
    netDataStr_arr.pop_front();
    netDataStr_arr.pop_front();
    // every line data
    quint64 tmp_receive = 0;
    quint64 tmp_transmit = 0;
    if (netDataStr.size() > 0)
    {
        for (QString line : netDataStr_arr)
        {
            if (line.split(":")[0] == "lo") continue;
            QStringList ite = line.split(" ", Qt::SkipEmptyParts);
            tmp_receive     += ite[1].toULongLong();
            tmp_transmit    += ite[9].toULongLong();
        }
    }
    this->receive = tmp_receive - this->receive_last;
    this->transmit = tmp_transmit - this->receive_last;
    // store last
    this->receive_last = tmp_receive;
    this->transmit_last = tmp_transmit;


    // ##################################################################################################
    //                          last update time
    // ##################################################################################################
    this->lastUpdateTime = QDateTime::currentMSecsSinceEpoch();
}

/** *********************************************************
                           get
********************************************************* */
quint64 SysInfo::getMemTotal()
{
    return this->sys_info.totalram;
}

quint64 SysInfo::getMemFree()
{
    return this->sys_info.freeram;
}

quint64 SysInfo::getSwapTotal()
{
    return this->sys_info.totalswap;
}

quint64 SysInfo::getSwapFree()
{
    return this->sys_info.freeswap;
}

double SysInfo::getCpuUsage()
{
    return this->cpuUsage;
}

double SysInfo::getCpuTemperature()
{
    return this->cpuTemperature;
}

quint64 SysInfo::getReceive() {
    return this->receive;
}

quint64 SysInfo::getTransmit() {
    return this->transmit;
}
