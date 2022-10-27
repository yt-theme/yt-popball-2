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
    this->x         = this->settingsObj->value("/position/x").toInt();
    this->y         = this->settingsObj->value("/position/y").toInt();
    this->width     = this->settingsObj->value("/appearance/width").toInt();
    this->height    = this->settingsObj->value("/appearance/height").toInt();
    this->opacity   = this->settingsObj->value("/appearance/opacity").toDouble();
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

void Config::setOpacity(double val)
{
    this->settingsObj->setValue("/appearance/opacity", val);
    this->opacity = val;
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

double Config::getOpacity()
{
    return this->opacity;
}

