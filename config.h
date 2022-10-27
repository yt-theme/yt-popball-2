#ifndef CONFIG_H
#define CONFIG_H

#include <QSettings>
#include <QString>
#include <QFile>
#include <QDebug>
#include <QDir>

class Config {
private:
    QSettings *settingsObj;
    QString configFilePath = QDir( QDir::homePath()).absoluteFilePath(".popball2_config.ini");
    QString defaultConfigRes = ":/config/default_config.ini";

private:
    // configure items
    qint32 x;
    qint32 y;
    qint32 width;
    qint32 height;
    double opacity;

public:
    Config();
    ~Config();

    // check config file
    bool checkOrCreateConfig();
    // get arg value
    void readConfig();

    // write to config
    void setX(qint32 val);
    void setY(qint32 val);
    void setWidth(qint32 val);
    void setHeight(qint32 val);
    void setOpacity(double val);

    // get config item values
    qint32 getX();
    qint32 getY();
    qint32 getWidth();
    qint32 getHeight();
    double getOpacity();
};


#endif // CONFIG_H
