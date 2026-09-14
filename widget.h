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
    // X11：混成(compositing)状态探测搭在 updateUITimer 上，每 N 次 UI 刷新查一次
    // （独立定时器已移除，避免多一个常驻唤醒；N 见 widget.cpp 的 kCompositingCheckEveryUiTicks）
    int                          compositingTickCounter = 0;
    bool                         shapeMaskApplied     = false;  // 最近一次应用的是否为"开蒙版"
    bool                         shapeMaskInitialized = false;  // 是否已应用过（避免首帧重复）
    // 窗口 flags / 半透明属性是否已在原生窗口创建前设置过。
    // 对已显示的窗口再次调用 setWindowFlags()/setAttribute(WA_TranslucentBackground)
    // 会触发平台层窗口重建：X11 上 KWin 会因此解除对窗口的管理（WM_STATE 丢失），
    // 重建后的半透明窗口不再被合成，小球整窗透明"消失"。这些 flags 恒定不变，
    // 只需在首次构造（show 之前）设置一次。
    bool                         windowFrameInitialized = false;
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
    // 仅做拖动松手后的屏幕边界限制与贴边吸附（不重设窗口 flags/属性，避免原生窗口重建）
    void applyEdgeSnap();

    // 桌面环境适配：不同平台/不同桌面（Xorg / Wayland / macOS）的窗口行为不同
    void applyDesktopBehavior();
    // 圆形形状蒙版：桌面没开混成时，用它把窗口裁成圆的，避免露出黑色矩形
    void applyShapeMask(bool on);
    // X11：重新评估"是否需要圆形蒙版"，结果变化时才应用（桌面混成开/关时自动调用）
    void reevaluateShapeMask();

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
