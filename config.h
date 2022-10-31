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
    qint32 aside_width;
    qint32 aside_height;
    double opacity;
    qint32 shadow_radius;
    qint16 shape;

    QString main_color;
    QString main_border_color;
    qint32  main_border_width;
    QString mem_color;
    QString swap_color;
    QString cpu_usage_color;
    QString cpu_freq_color;
    QString cpu_temp_color;
    QString net_speed_color;

    qint32 charts_rows;
    qint32 update_data_interval;
    qint32 update_ui_interval;

public:
    Config();
    ~Config();

    // ########### check config file ###########
    bool checkOrCreateConfig();
    // get arg value
    void readConfig();

    // ########### write to config #############
    void setX(qint32 val);
    void setY(qint32 val);
    void setWidth(qint32 val);
    void setHeight(qint32 val);
    void setAsideWidth(qint32 val);
    void setAsideHeight(qint32 val);
    void setOpacity(double val);
    void setShadowRadius(qint32 val);
    void setShape(qint32 val);

    void setMainColor(QString val);
    void setMainBorderColor(QString val);
    void setMainBorderWidth(qint32 val);
    void setMemColor(QString val);
    void setSwapColor(QString val);
    void setCpuUsageColor(QString val);
    void setCpuFreqColor(QString val);
    void setCpuTempColor(QString val);
    void setNetSpeedColor(QString val);

    void setChartsRows(qint32 val);
    void setUpdateDataInterval(qint32 val);
    void setUpdateUIInterval(qint32 val);

    // ########### get config item values ###########
    qint32  getX();
    qint32  getY();
    qint32  getWidth();
    qint32  getHeight();
    qint32  getAsideWidth();
    qint32  getAsideHeight();
    double  getOpacity();
    qint32  getShadowRadius();
    qint32  getShape();

    QString getMainColor();
    QString getMainBorderColor();
    qint32  getMainBorderWidth();
    QString getMemColor();
    QString getSwapColor();
    QString getCpuUsageColor();
    QString getCpuFreqColor();
    QString getCpuTempColor();
    QString getNetSpeedColor();

    qint32  getChartsRows();
    qint32  getUpdateDataInterval();
    qint32  getUpdateUIInterval();
};


#endif // CONFIG_H
