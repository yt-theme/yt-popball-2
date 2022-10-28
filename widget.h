#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>
#include <QVector>
#include <QTimer>
#include <QLCDNumber>
#include <QGraphicsDropShadowEffect>
#include "macro_def.h"
#include "config.h"
#include "sysInfo.h"

class Widget : public QWidget
{
    Q_OBJECT
private:
    // base
    Config *config;
    SysInfo *sysInfo;
    QGraphicsDropShadowEffect *winShadow;
    QTimer *updateDataTimer;

    // history data
    QVector<quint64> mem_data_history;
    QVector<quint64> swap_data_history;
    QVector<double>  cpuUsage_data_history;

    // widgets
    QLCDNumber *cpuFreqLCD;

public:
    Widget(QWidget *parent = nullptr);
    ~Widget();

    void setPosition();
    void setUiFrame();

    // update data && history
    void updateDataAndHistory();

private slots:
    void paintEvent(QPaintEvent *event);
    void onTimerIntervalForUpdateData();

};
#endif // WIDGET_H
