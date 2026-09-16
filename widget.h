#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QPen>
#include <QColor>
#include <QSize>
#include <QVector>
#include <QGuiApplication>
#include <QScreen>
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

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEnterEvent>

class PopDock;   // 悬浮球 hover 300ms 弹出的工具/中转站面板

// 贴边竖条上的一根窄柱 = 一个指标
// （拖动到屏幕左右边缘吸附成竖条后，用它代替小球上的曲线图）
struct AsideMetric
{
    QString label;   // 指标名（鼠标悬停气泡里显示）
    double  ratio;   // 当前值占比 0.0 ~ 1.0
    QColor  color;   // 柱身/轨道颜色（取自配置里对应的指标颜色）
};

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
    // 最近一次蒙版是按哪个形态做的：圆球和竖条的蒙版形状不同，
    // 形态切换后即使"要不要蒙版"没变，也必须按新形状重做一次。
    qint32                       shapeMaskAppliedShape = -1;
    // 窗口 flags / 半透明属性是否已在原生窗口创建前设置过。
    // 对已显示的窗口再次调用 setWindowFlags()/setAttribute(WA_TranslucentBackground)
    // 会触发平台层窗口重建：X11 上 KWin 会因此解除对窗口的管理（WM_STATE 丢失），
    // 重建后的半透明窗口不再被合成，小球整窗透明"消失"。这些 flags 恒定不变，
    // 只需在首次构造（show 之前）设置一次。
    bool                         windowFrameInitialized = false;
    bool                         isMousePressed = false;
    // 按下那一刻面板是否是开着的：松手若判定为"单击"就据此做开/关切换
    bool                         dockWasVisibleOnPress = false;
    // 本次按下是否已经产生了"真正的拖动"（位移超过单击阈值）
    bool                         pressMoved = false;
    QPoint                      curWindowPos;
    // 按下时的全局光标位置。判断"沿边缘上下拖 / 往回拖脱离边缘 / 单击"必须用
    // 全局坐标：拖动时窗口是跟着光标一起动的，窗口内坐标几乎不变。
    QPoint                      pressGlobalPos;

    // history data
    QVector<quint64> mem_data_history;
    QVector<quint64> swap_data_history;
    QVector<double>  cpuUsage_data_history;

    // widgets
    QLCDNumber *cpuTempLCD       = nullptr;
    QLCDNumber *cpuFreqLCD       = nullptr;
    QLCDNumber *netUploadLCD     = nullptr;
    QLCDNumber *netDownloadLCD   = nullptr;
    QLCDNumber *diskIoLCD        = nullptr;   // 磁盘读写总速度（MB/s，LCD 数码字体，两位小数）

    // 圆球形态下各"信息行"的排布。
    //
    // 顺序即从上到下的显示顺序：温度 / 频率 / 磁盘 / 网速上 / 网速下。
    // 磁盘行用 QLCDNumber 与其它行保持一致的数码字体，显示"读+写"总速度的 MB/s 数值、
    // 保留两位小数。QLCDNumber 的分段字体画不出字母（如 "MB/s" 单位），故单位省略，
    // 数值本身已是 MB/s。
    // 竖直排布按实际可见的行数自适应：只显示 3 行时行高更大，5 行时自动压缩，
    // 既不留空洞、也不会被挤出球外。
    enum LcdRow {
        ROW_TEMP = 0, ROW_FREQ, ROW_DISK, ROW_NET_UP, ROW_NET_DOWN, ROW_COUNT
    };
    quint8 lcdRowMask = 0;                    // 各行的"应显示"位图
    QRect  lcdRowRect[ROW_COUNT];             // 各行在球内的矩形（LCD 摆放用）

    // 依据 lcdRowMask 重新排布各行几何（宽高变化、显隐变化后调用）
    void relayoutVisibleLcds();

    // 右键菜单（设置 / 系统监视器 / 退出）
    QMenu   *contextMenu        = nullptr;
    QAction *actSettings        = nullptr;
    QAction *actSystemMonitor   = nullptr;
    QAction *actQuit            = nullptr;

    // 设置窗口（右键菜单 -> 设置）
    SettingsDialog *settingsDialog = nullptr;

    // hover 300ms 弹出的工具/中转站面板（PopDock）
    PopDock                     *popDock        = nullptr;
    QTimer                      *hoverTimer     = nullptr;   // 悬浮球上停留 300ms → 打开面板
    QTimer                      *hideTimer      = nullptr;   // 鼠标离开球与面板后延迟隐藏面板
    QTimer                      *hoverPollTimer = nullptr;   // 轻量轮询：即使窗口/应用未激活也能靠全局光标位置弹出面板
    bool                         cursorOverUi   = false;     // 轮询判定的"光标是否在球或面板上"状态（用于检测边界跳变）
    bool                         dragOpening    = false;     // 当前是否正处于文件拖入（拖到球上）状态

public:
    Widget(QWidget *parent = nullptr);
    ~Widget();

    void setPosition();
    void setUiFrame();
    // 仅做拖动松手后的屏幕边界限制与贴边吸附（不重设窗口 flags/属性，避免原生窗口重建）
    void applyEdgeSnap();

    // ---------- 贴边竖条（SHAPE_ASIDE） ----------
    // 某个形状对应的窗口尺寸（含四周为投影/半透明预留的留边）
    QSize sizeForShape(qint32 shape) const;
    // 窗口当前所在的屏幕区域（多显示器下按窗口中心取屏，取不到再退回主屏）
    QRect currentScreenRect() const;
    // 形态切换后若正开着形状蒙版，需要按新形状重做一次蒙版
    void refreshMaskAfterShapeChange();
    // 由"贴边竖条"变回小球：
    //   keepUnderCursor = true  拖动脱离边缘：小球紧贴光标摆开，可以接着继续拖
    //   keepUnderCursor = false 单击竖条：小球从边缘"弹"出来一点，不贴着边（否则松手又被吸回去）
    void detachToCircle(const QPoint &globalCursor, bool keepUnderCursor);
    // 竖条形态下要显示的各指标（顺序即柱子的左右顺序）
    QVector<AsideMetric> collectAsideMetrics() const;
    // 小球上的 LCD 在竖条形态下必须全部隐藏（窗口只有几十像素宽，LCD 会挤在竖条里）
    void hideAllLcds();
    // 绘制贴边竖条：圆角矩形 + 一组窄柱图
    void drawAsideBar(QPainter &painter);

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
    void onMenuSystemMonitor();
    void onMenuQuit();
    // 设置保存后刷新界面
    void onSettingsApplied();

    // ---------- hover 弹出的工具/中转站面板（PopDock） ----------
    // 悬浮球上停留超过阈值 → 在球旁打开面板（带划出动画）
    void showPopDock();
    // 面板落点计算（避开屏幕边缘/菜单栏/任务栏，不压住悬浮球，小屏自动收缩尺寸）
    QPoint popDockTargetPos();
    // 鼠标离开球与面板 → 延迟隐藏面板（进入任一区域则取消）
    void scheduleHidePopDock();
    void cancelHidePopDock();
    void onHoverTimeout();
    void onHideTimeout();
    // 轮询：用全局光标坐标判断是否在球/面板上，未激活窗口也能触发弹出（见需求：不激活时移上去也要出窗口）
    void onHoverPoll();

    // 拖放：把文件拖到球上（停留 300ms 自动开面板）/ 直接松手落到球上也收下
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
};
#endif // WIDGET_H
