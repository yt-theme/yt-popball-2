#include "config.h"

Config::Config()
{
    // check
    bool checkOrCreateRet = this->checkOrCreateConfig();
    // create setting obj
    if (checkOrCreateRet == true)
    {
        this->settingsObj = new QSettings(this->configFilePath, QSettings::IniFormat);
    }

    // read config
    this->readConfig();


}

Config::~Config()
{
    delete this->settingsObj;
}

// check config file isExists and create
bool Config::checkOrCreateConfig()
{
    QFile cfgFile(this->configFilePath);
    if (cfgFile.exists() == false)
    {

        cfgFile.open(QIODevice::WriteOnly);
        cfgFile.close();

        cfgFile.open(QIODevice::WriteOnly);
        QFile defaultConfFile = QFile(this->defaultConfigRes);
        defaultConfFile.open(QIODevice::ReadOnly);

        qint64 cfgFileWriteRet = cfgFile.write(defaultConfFile.readAll());
        cfgFile.close();
        if (cfgFileWriteRet <= 0)
        {
            qDebug() << "CheckOrCreateConfig: copy default configure file err" << "\n";
            return false;
        }
    }
    return true;
}

// read configure file content
void Config::readConfig()
{
    this->x                     = this->settingsObj->value("/position/x").toInt();
    this->y                     = this->settingsObj->value("/position/y").toInt();
    this->width                 = this->settingsObj->value("/appearance/width").toInt();
    this->height                = this->settingsObj->value("/appearance/height").toInt();
    this->aside_width           = this->settingsObj->value("/appearance/aside_width").toInt();
    this->aside_height          = this->settingsObj->value("/appearance/aside_height").toInt();
    this->opacity               = this->settingsObj->value("/appearance/opacity").toDouble();
    this->shadow_radius         = this->settingsObj->value("/appearance/shadow_radius").toInt();
    this->shape                 = this->settingsObj->value("/appearance/shape").toInt();
    this->main_color            = this->settingsObj->value("/appearance/main_color").toString();
    this->main_border_color     = this->settingsObj->value("/appearance/main_border_color").toString();
    this->main_border_width     = this->settingsObj->value("/appearance/main_border_width").toInt();
    this->mem_color             = this->settingsObj->value("/appearance/mem_color").toString();
    this->swap_color            = this->settingsObj->value("/appearance/swap_color").toString();
    this->cpu_usage_color       = this->settingsObj->value("/appearance/cpu_usage_color").toString();
    this->cpu_usage_width       = this->settingsObj->value("/appearance/cpu_usage_width").toDouble();
    this->cpu_freq_color        = this->settingsObj->value("/appearance/cpu_freq_color").toString();
    this->cpu_temp_color        = this->settingsObj->value("/appearance/cpu_temp_color").toString();
    this->net_speed_color       = this->settingsObj->value("/appearance/net_speed_color").toString();
    this->charts_rows           = this->settingsObj->value("/appearance/charts_rows").toInt();
    this->update_data_interval  = this->settingsObj->value("/timer/update_data_interval").toInt();
    this->update_ui_interval    = this->settingsObj->value("/timer/update_ui_interval").toInt();

    // [components_show]
    this->cpu_temp_show         = this->settingsObj->value("/components_show/cpu_temp_show").toInt();
    this->cpu_freq_show         = this->settingsObj->value("/components_show/cpu_freq_show").toInt();
    this->net_speed_show        = this->settingsObj->value("/components_show/net_speed_show").toInt();
}


/** *********************************************************
                           set
********************************************************* */
void Config::setX(qint32 val)
{
    this->settingsObj->setValue("/position/x", val);
    this->x = val;
}

void Config::setY(qint32 val)
{
    this->settingsObj->setValue("/position/y", val);
    this->y = val;
}

void Config::setWidth(qint32 val)
{
    this->settingsObj->setValue("/appearance/width", val);
    this->width = val;
}

void Config::setHeight(qint32 val)
{
    this->settingsObj->setValue("/appearance/height", val);
    this->height = val;
}

void Config::setAsideWidth(qint32 val)
{
    this->settingsObj->setValue("/appearance/aside_width", val);
    this->aside_width = val;
}

void Config::setAsideHeight(qint32 val)
{
    this->settingsObj->setValue("/appearance/aside_height", val);
    this->aside_height = val;
}

void Config::setOpacity(double val)
{
    this->settingsObj->setValue("/appearance/opacity", val);
    this->opacity = val;
}

void Config::setShadowRadius(qint32 val)
{
    this->settingsObj->setValue("/appearance/shadow_radius", val);
    this->shadow_radius = val;
}

void Config::setShape(qint32 val)
{
    this->settingsObj->setValue("/appearance/shape", val);
    this->shape = val;
}

void Config::setMainColor(QString val)
{
    this->settingsObj->setValue("/appearance/main_color", val);
    this->main_color = val;
}
void Config::setMainBorderColor(QString val)
{
    this->settingsObj->setValue("/appearance/main_border_color", val);
    this->main_border_color = val;
}

void Config::setMainBorderWidth(qint32 val)
{
    this->settingsObj->setValue("/appearance/main_border_width", val);
    this->main_border_width = val;
}

void Config::setMemColor(QString val)
{
    this->settingsObj->setValue("/appearance/mem_color", val);
    this->mem_color = val;

}
void Config::setSwapColor(QString val)
{
    this->settingsObj->setValue("/appearance/swap_color", val);
    this->swap_color = val;
}
void Config::setCpuUsageColor(QString val)
{
    this->settingsObj->setValue("/appearance/cpu_usage_color", val);
    this->cpu_usage_color = val;
}

void Config::setCpuUsageWidth(double val)
{
    this->settingsObj->setValue("/appearance/cpu_usage_width", val);
    this->cpu_usage_width = val;
}

void Config::setCpuFreqColor(QString val)
{
    this->settingsObj->setValue("/appearance/cpu_freq_color", val);
    this->cpu_freq_color = val;
}
void Config::setCpuTempColor(QString val)
{
    this->settingsObj->setValue("/appearance/cpu_temp_color", val);
}
void Config::setNetSpeedColor(QString val)
{
    this->settingsObj->setValue("/appearance/net_speed_color", val);
    this->net_speed_color = val;
}

void Config::setChartsRows(qint32 val)
{
    this->settingsObj->setValue("/appearance/charts_rows", val);
    this->charts_rows = val;
}

void Config::setUpdateDataInterval(qint32 val)
{
    this->settingsObj->setValue("/timer/update_data_interval", val);
    this->update_data_interval = val;
}

void Config::setUpdateUIInterval(qint32 val)
{
    this->settingsObj->setValue("/timer/update_ui_interval", val);
    this->update_ui_interval = val;
}

// [components_show]
void Config::setCpuTempShow(qint8 val)
{
    this->settingsObj->setValue("/components_show/cpu_temp_show", val);
    this->cpu_temp_show = val;
}
void Config::setCpuFreqShow(qint8 val)
{
    this->settingsObj->setValue("/components_show/cpu_freq_show", val);
    this->cpu_freq_show = val;
}
void Config::setNetSpeedShow(qint8 val)
{
    this->settingsObj->setValue("/components_show/net_speed_show", val);
    this->net_speed_show = val;
}

/** *********************************************************
                           get
********************************************************* */
qint32 Config::getX()
{
    return this->x;
}

qint32 Config::getY()
{
    return this->y;
}

qint32 Config::getWidth()
{
    return this->width;
}

qint32 Config::getHeight()
{
    return this->height;
}

qint32 Config::getAsideWidth()
{
    return this->aside_width;
}

qint32 Config::getAsideHeight()
{
    return this->aside_height;
}

double Config::getOpacity()
{
    return this->opacity;
}

qint32 Config::getShadowRadius()
{
    return this->shadow_radius;
}

qint32 Config::getShape()
{
    return this->shape;
}

QString Config::getMainColor()
{
    return this->main_color;
}
QString Config::getMainBorderColor()
{
    return this->main_border_color;
}
qint32 Config::getMainBorderWidth()
{
    return this->main_border_width;
}
QString Config::getMemColor()
{
    return this->mem_color;
}
QString Config::getSwapColor()
{
    return this->swap_color;
}
QString Config::getCpuUsageColor()
{
    return this->cpu_usage_color;
}
double Config::getCpuUsageWidth()
{
    return this->cpu_usage_width;
}
QString Config::getCpuFreqColor()
{
    return this->cpu_freq_color;
}
QString Config::getCpuTempColor()
{
    return this->cpu_temp_color;
}
QString Config::getNetSpeedColor()
{
    return this->net_speed_color;
}

qint32 Config::getChartsRows()
{
    return this->charts_rows;
}

qint32 Config::getUpdateDataInterval()
{
    return this->update_data_interval;
}

qint32 Config::getUpdateUIInterval()
{
    return this->update_ui_interval;
}

// [components_show]
qint8 Config::getCpuTempShow()
{
    return this->cpu_temp_show;
}
qint8 Config::getCpuFreqShow()
{
    return this->cpu_freq_show;
}
qint8 Config::getNetSpeedShow()
{
    return this->net_speed_show;
}


