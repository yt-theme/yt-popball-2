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
    this->_file_obj->setFileName("/proc/meminfo");
    this->_file_obj->open(QIODevice::ReadOnly|QIODevice::Text);
    QString mem_data_string = this->_file_obj->readAll().replace("\t", "");
    this->_file_obj->close();
    QStringList mem_data_list   = mem_data_string.split("\n", Qt::SkipEmptyParts);
    for (const QString &ite : mem_data_list) {
        // every item for lines
        QStringList ite_list = ite.split(":", Qt::SkipEmptyParts);
        if (ite_list[0] == "MemTotal")     { memoryInfo.mem_total       = ite_list[1].trimmed().split(" ")[0].toULongLong(); }
        if (ite_list[0] == "MemFree")      { memoryInfo.mem_free        = ite_list[1].trimmed().split(" ")[0].toULongLong(); }
        if (ite_list[0] == "MemAvailable") { memoryInfo.mem_available   = ite_list[1].trimmed().split(" ")[0].toULongLong(); }
        if (ite_list[0] == "Cached")       { memoryInfo.cached          = ite_list[1].trimmed().split(" ")[0].toULongLong(); }
        if (ite_list[0] == "Buffers")      { memoryInfo.buffers         = ite_list[1].trimmed().split(" ")[0].toULongLong(); }
        if (ite_list[0] == "SwapTotal")    { memoryInfo.swap_total      = ite_list[1].trimmed().split(" ")[0].toULongLong(); }
        if (ite_list[0] == "SwapFree")     { memoryInfo.swap_free       = ite_list[1].trimmed().split(" ")[0].toULongLong(); }
    }

    memoryInfo.mem_used  = memoryInfo.mem_total - memoryInfo.buffers - memoryInfo.mem_free - memoryInfo.cached;
    memoryInfo.swap_used = memoryInfo.swap_total - memoryInfo.swap_free;

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
        if (cpuInfoStr_arr[i].contains("cpu MHz"))
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
    if (cpuUsageStr_1st_arr.size() > 3)
    {
        double use = cpuUsageStr_1st_arr[1].toDouble() + cpuUsageStr_1st_arr[2].toDouble() + cpuUsageStr_1st_arr[3].toDouble();
        double total = 0;
        for (int i=0; i<cpuUsageStr_1st_arr.size(); i++)
        {
            total += cpuUsageStr_1st_arr[i].toDouble();
        }

        // cpu usage
        this->cpuUsage = (use - this->cpuUsage_use_last) / (total - this->cpuUsage_total_last) * 100.0;

        // store last
        this->cpuUsage_total_last = total;
        this->cpuUsage_use_last = use;
    }





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
    this->transmit = tmp_transmit - this->transmit_last;


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
qulonglong SysInfo::getMemTotal()
{
    return this->memoryInfo.mem_total;
}

qulonglong SysInfo::getMemUsed()
{
    return this->memoryInfo.mem_used;
}

qulonglong SysInfo::getMemFree()
{
    return this->memoryInfo.mem_free;
}

qulonglong SysInfo::getSwapTotal()
{
    return this->memoryInfo.swap_total;
}

qulonglong SysInfo::getSwapUsed()
{
    return this->memoryInfo.swap_used;
}

qulonglong SysInfo::getSwapFree()
{
    return this->memoryInfo.swap_free;
}

double SysInfo::getCpuFreq()
{
    return this->cpuFreq;
}

double SysInfo::getCpuUsage()
{
    return this->cpuUsage;
}

double SysInfo::getCpuTemperature()
{
    return this->cpuTemperature;
}

qulonglong SysInfo::getReceive() {
    return this->receive;
}

qulonglong SysInfo::getTransmit() {
    return this->transmit;
}
