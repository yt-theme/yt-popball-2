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
#include <QMenu>
#include <QAction>
#include "macro_def.h"
#include "config.h"
#include "sysInfo.h"
#include "settingsdialog.h"

class Widget : public QWidget
{
    Q_OBJECT
private:
    // base
    // 注意：这些指针必须初始化为 nullptr。像 winShadow 这种"先判断是否已创建、
    // 再决定要不要 new"的写法，如果指针是未初始化的垃圾值，判断会失败/误判，
    // 随后在野指针上调用方法就会直接段错误。
    Config                      *config          = nullptr;
    SysInfo                     *sysInfo         = nullptr;
    QGraphicsDropShadowEffect   *winShadow       = nullptr;
    QTimer                      *updateDataTimer = nullptr;
    QTimer                      *updateUITimer   = nullptr;
    bool                        isMousePressed = false;
    QPoint                      curWindowPos;

    // history data
    QVector<quint64> mem_data_history;
    QVector<quint64> swap_data_history;
    QVector<double>  cpuUsage_data_history;

    // widgets
    QLCDNumber *cpuTempLCD       = nullptr;
    QLCDNumber *cpuFreqLCD       = nullptr;
    QLCDNumber *netUploadLCD     = nullptr;
    QLCDNumber *netDownloadLCD   = nullptr;

    // 右键菜单（设置 / 退出）
    QMenu   *contextMenu    = nullptr;
    QAction *actSettings    = nullptr;
    QAction *actQuit        = nullptr;

    // 设置窗口（右键菜单 -> 设置）
    SettingsDialog *settingsDialog = nullptr;

public:
    Widget(QWidget *parent = nullptr);
    ~Widget();

    void setPosition();
    void setUiFrame();

    // 桌面环境适配：不同平台/不同桌面（Xorg / Wayland / macOS）的窗口行为不同
    void applyDesktopBehavior();
    // 圆形形状蒙版：桌面没开混成时，用它把窗口裁成圆的，避免露出黑色矩形
    void applyShapeMask(bool on);

    // 构建右键菜单（在构造函数里调用一次）
    void buildContextMenu();
    // 退出：先隐藏窗口、停掉定时器，再安全地结束事件循环
    void quitApplication();
    // 把配置里的颜色套到各 LCD 上（设置保存后也要调用）
    void applyLcdStyle();
    // 按配置里的尺寸重新摆放各 LCD（改"大小"后必须调用，否则数字会留在旧位置）
    void applyLcdLayout();

    // update data && history
    void updateDataAndHistory();

private slots:
    void paintEvent(QPaintEvent *event);
    void mousePressEvent(QMouseEvent *event);
    void mouseMoveEvent(QMouseEvent *event);
    void mouseReleaseEvent(QMouseEvent *event);

    void onTimerIntervalForUpdateData();
    void onTimerIntervalForUpdateUI();

    // 右键菜单动作
    void onMenuSettings();
    void onMenuQuit();
    // 设置保存后刷新界面
    void onSettingsApplied();
};
#endif // WIDGET_H
