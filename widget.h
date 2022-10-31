#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QPen>
#include <QVector>
#include <QGuiApplication>
#include <QLineF>
#include <QPointF>
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
    Config                      *config;
    SysInfo                     *sysInfo;
    QGraphicsDropShadowEffect   *winShadow;
    QTimer                      *updateDataTimer;
    QTimer                      *updateUITimer;
    bool                        isMousePressed = false;
    QPoint                      curWindowPos;

    // history data
    QVector<quint64> mem_data_history;
    QVector<quint64> swap_data_history;
    QVector<double>  cpuUsage_data_history;

    // widgets
    QLCDNumber *cpuTempLCD;
    QLCDNumber *cpuFreqLCD;
    QLCDNumber *netUploadLCD;
    QLCDNumber *netDownloadLCD;

public:
    Widget(QWidget *parent = nullptr);
    ~Widget();

    void setPosition();
    void setUiFrame();

    // update data && history
    void updateDataAndHistory();

private slots:
    void paintEvent(QPaintEvent *event);
    void mousePressEvent(QMouseEvent *event);
    void mouseMoveEvent(QMouseEvent *event);
    void mouseReleaseEvent(QMouseEvent *event);

    void onTimerIntervalForUpdateData();
    void onTimerIntervalForUpdateUI();


};
#endif // WIDGET_H
