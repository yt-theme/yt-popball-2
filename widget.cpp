#include "widget.h"

#include <QBitmap>
#include <QImage>
#include <QRegion>
#include <QMessageBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QStandardPaths>

#if defined(POPBALL_HAVE_X11)
#  include <QtGui/qguiapplication_platform.h>
// 顺序很重要：必须在 Qt 头文件之后再包含 Xlib.h。
// Xlib 会把 None / True / False 定义成宏，与 Qt 里的同名枚举冲突；
// 用完立刻清理，避免影响后面可能被包含进来的 Qt 头文件。
#  include <X11/Xlib.h>
#  undef None
#  undef True
#  undef False
#endif

#if defined(Q_OS_MACOS)
#  include "macwindow.h"
#endif

namespace {

// 网速自适应格式化。
// 输入字节/秒，输出固定 4 字符宽的字段（含单位字母），例如：
//     1.23K   12.3K   123K   1.23M   12.3M   123M   1.23G
// 规则：
//   * 单位按 1024 进制自适应（KB/s → MB/s → GB/s），数值到达 1024 就升单位；
//   * 有效位恒为 3 位 —— 1 位整数带 2 位小数、2 位整数带 1 位小数、
//     3 位整数不带小数。也就是说"数字一多就升单位"，永远只显示 3 位有效数字。
// 固定宽度对 LCD 很重要：QLCDNumber 是 7 段数码管，字段等宽意味着
// 小球上 "u xx.xx" / "d xx.xx" 的位置不会随网速大小跳动，
// 也不会出现网速一高数字被截断的问题。
QString formatNetSpeedField(double bytesPerSec)
{
    // 固定单位 MiB/s（不显示单位字母），小数位按数值大小自适应：
    //   <1        → 3 位小数   "0.048"   （几 KB/s 的空闲流量也有读数）
    //   <10       → 2 位小数   "01.23"
    //   <100      → 2 位小数   "12.34"
    //   <1000     → 1 位小数   "123.4"
    //   ≥1000     → 无小数     "1234"    （上限 9999）
    // 字段恒为 5 字符宽（前导 0 补齐），LCD 上 "u 0.048" / "d 123.4" 不会跳动。
    double v = bytesPerSec / (1024.0 * 1024.0);   // MiB/s
    QString s;
    if (v < 1.0)         s = QString::number(v, 'f', 3);   // "0.048"
    else if (v < 100.0)  s = QString::number(v, 'f', 2);   // "01.23" / "45.67"
    else if (v < 1000.0) s = QString::number(v, 'f', 1);   // "123.4"
    else {
        s = QString::number(qRound(v), 'f', 0);            // "1234"
        if (s.size() > 4) s = QStringLiteral("9999");      // 钳到上限
        return s;                                          // 4 位整数不再补零
    }
    while (s.size() < 5) s.prepend(QLatin1Char('0'));      // 等宽
    return s;
}

// X11 混成状态探测频率：每 N 次 updateUITimer 触发查一次。
// update_ui_interval 默认 450ms，N=10 ≈ 每 4.5s 查一次 —— 不额外占一个定时器、
// 也不频繁打扰 X 服务器；用户切换混成后能在几秒内自动去/回黑框。
constexpr int kCompositingCheckEveryUiTicks = 10;

#if defined(POPBALL_HAVE_X11)
// X11：检测桌面上有没有「混成管理器(compositing manager)」在跑。
// 没有混成时，WA_TranslucentBackground 不会被真正混合，小球四周就会露出一块
// 黑色矩形 —— 这正是「关掉桌面混成后小球旁边出现黑框」的根因。
// 标准判断方式：看 _NET_WM_CM_Sx 这个选择(selection)有没有被占用。
bool x11CompositingMissing()
{
    QNativeInterface::QX11Application *x11 =
        qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    if (x11 == nullptr)
        return false;                    // 拿不到 X11 连接，按「有混成」处理
    Display *dpy = x11->display();
    if (dpy == nullptr)
        return false;

    // 1 = only_if_exists(即 Xlib 的 True)；0 = Xlib 的 None
    const Atom cm = XInternAtom(dpy, "_NET_WM_CM_S0", 1);
    if (cm == 0)
        return true;                     // 连这个 atom 都不存在 → 肯定没有混成
    return XGetSelectionOwner(dpy, cm) == 0;
}
#endif

}   // namespace

Widget::Widget(QWidget *parent)
    : QWidget(parent)
{
    // config file check and read
    this->config = new Config();
    // get system info
    this->sysInfo = new SysInfo();

    // update data timer
    this->updateDataTimer = new QTimer();
    connect(updateDataTimer, &QTimer::timeout, this, &Widget::onTimerIntervalForUpdateData);
    // update UI timer
    this->updateUITimer = new QTimer();
    connect(updateUITimer, &QTimer::timeout, this, &Widget::onTimerIntervalForUpdateUI);

    // widget set
    this->setPosition();
    this->setUiFrame();

    // ################# widget ###################
    // cpu temp LCD
    this->cpuTempLCD = new QLCDNumber(this);
    this->cpuTempLCD->setDigitCount(5);
    this->cpuTempLCD->setMode(QLCDNumber::Dec);
    this->cpuTempLCD->setSegmentStyle(QLCDNumber::Flat);
    this->cpuTempLCD->setGeometry(0, config->getHeight()/5.9, config->getWidth(), config->getWidth()/5.5);
    this->cpuTempLCD->display("00'c");
    // cpu freq LCD
    this->cpuFreqLCD = new QLCDNumber(this);
    this->cpuFreqLCD->setDigitCount(9);
    this->cpuFreqLCD->setMode(QLCDNumber::Dec);
    this->cpuFreqLCD->setSegmentStyle(QLCDNumber::Flat);
    this->cpuFreqLCD->setGeometry(0, config->getHeight()/5.7 + config->getWidth()/5.5, config->getWidth(), config->getWidth()/8);
    this->cpuFreqLCD->display("000000000");
    // net upload LCD
    this->netUploadLCD = new QLCDNumber(this);
    this->netUploadLCD->setDigitCount(7);   // "u 01.23" 共 7 字符（小数点占 1 位）
    this->netUploadLCD->setMode(QLCDNumber::Dec);
    this->netUploadLCD->setSegmentStyle(QLCDNumber::Flat);
    this->netUploadLCD->setGeometry(0, config->getHeight()/5.7 + config->getWidth()/5.5 * 1.9, config->getWidth(), config->getWidth()/6.5);
    this->netUploadLCD->display("u 00.00");
    // net downlod LCD
    this->netDownloadLCD = new QLCDNumber(this);
    this->netDownloadLCD->setDigitCount(7);
    this->netDownloadLCD->setMode(QLCDNumber::Dec);
    this->netDownloadLCD->setSegmentStyle(QLCDNumber::Flat);
    this->netDownloadLCD->setGeometry(0, config->getHeight()/5.7 + config->getWidth()/5.5*2.8, config->getWidth(), config->getWidth()/6.5);
    this->netDownloadLCD->display("d 00.00");

    // LCD 前景色（设置里改颜色后也会重新套用）
    this->applyLcdStyle();

    // 右键菜单（设置 / 退出）——只构建一次，右键时直接弹出
    this->buildContextMenu();
}

// 按配置尺寸重新摆放各 LCD
// 改"大小"后必须调用：LCD 的几何位置只在构造时算过一次，
// 不重摆的话小球变大了数字还挤在原来的小区域里
void Widget::applyLcdLayout()
{
    const qint32 w = config->getWidth();
    const qint32 h = config->getHeight();
    this->cpuTempLCD->setGeometry(0, h/5.9, w, w/5.5);
    this->cpuFreqLCD->setGeometry(0, h/5.7 + w/5.5, w, w/8);
    this->netUploadLCD->setGeometry(0, h/5.7 + w/5.5 * 1.9, w, w/6.5);
    this->netDownloadLCD->setGeometry(0, h/5.7 + w/5.5*2.8, w, w/6.5);
}

// 把配置里的前景色套到各 LCD
void Widget::applyLcdStyle()
{
    this->cpuTempLCD->setStyleSheet("border: 0;color:" + config->getCpuTempColor() + ";");
    this->cpuFreqLCD->setStyleSheet("border: 0;color:" + config->getCpuFreqColor() + ";");
    this->netUploadLCD->setStyleSheet("border: 0;color:" + config->getNetSpeedColor() + ";");
    this->netDownloadLCD->setStyleSheet("border: 0;color:" + config->getNetSpeedColor() + ";");
}

Widget::~Widget()
{
    delete this->contextMenu;   // 菜单里的 action 由菜单自己管理
    delete this->config;
    delete this->sysInfo;
    delete this->updateDataTimer;
    delete this->updateUITimer;
    // winShadow 已通过 setGraphicsEffect() 交给 QWidget 托管，不在这里删除（会重复释放）
    delete this->cpuTempLCD;
    delete this->cpuFreqLCD;
    delete this->netUploadLCD;
    delete this->netDownloadLCD;
}

void Widget::setPosition()
{
    this->setGeometry(config->getX(), config->getY(), config->getWidth(), config->getHeight());
}

// 屏幕边界限制与贴边吸附（setUiFrame 初始化时、以及每次拖动松手时调用）。
// 只调整 shape 标记和几何位置，不碰窗口 flags / 半透明属性，
// 因此可以安全地在窗口已显示后反复调用。
void Widget::applyEdgeSnap()
{
    QRect primaryScreenRect = QGuiApplication::primaryScreen()->geometry();

    // set default shape
    this->config->setShape(SHAPE_CIRCLE);

    // check y
    if (this->config->getY() <= 0)
    {
        this->config->setY(0);
    }
    if (this->config->getY() >= (primaryScreenRect.height() - this->frameGeometry().height()))
    {
        this->config->setY(primaryScreenRect.height() - this->frameGeometry().height());
    }

    // windowif at aside
    if ((this->config->getX() + this->frameGeometry().width()) >= primaryScreenRect.width()
            || this->config->getX() <= 0)
    {
        // set shape
        this->config->setShape(SHAPE_ASIDE);
        // check at aside left or right
        // left
        if (this->config->getX() <= 0)
        {
            this->config->setX(0 - this->config->getShadowRadius());
        }
        // right
        if ((this->config->getX() + this->frameGeometry().width()) >= primaryScreenRect.width())
        {
            this->config->setX(primaryScreenRect.width() - this->frameGeometry().width() + this->config->getShadowRadius());
        }
    }
    // set geometry
    this->setGeometry(this->config->getX(), this->config->getY(), this->config->getWidth(), this->config->getHeight());
}

void Widget::setUiFrame()
{
    // ###################### 窗口类型 ######################
    // 无边框 + 始终置顶 + 工具窗口（不占任务栏、不进 Dock / Cmd-Tab），
    // 效果就类似 360 悬浮球。
    //
    // 注意：这里刻意不再使用 Qt::BypassWindowManagerHint。
    // 它会让窗口完全绕过窗口管理器：在 X11 上合成器因此不接管该窗口，
    // 半透明区域直接漏出黑色矩形（"关掉混成后小球旁出现黑框"的根因之一）；
    // 在 Wayland 上它也没有意义。改用标准的 无边框 + 置顶 组合即可。
    // 窗口 flags 与半透明属性恒定不变，且必须在原生窗口创建之前设置。
    // 切忌对已显示的窗口重复调用：setWindowFlags()/setAttribute(WA_TranslucentBackground)
    // 会触发平台层重建窗口 —— X11 上 KWin 会解除对窗口的管理（WM_STATE 丢失），
    // 重建后的半透明窗口不再被合成，表现为小球整窗透明、从桌面上"消失"。
    if (!this->windowFrameInitialized)
    {
        Qt::WindowFlags flags = Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool;
#if defined(Q_OS_MACOS)
        // macOS：不接收键盘焦点，点小球不会把当前正在用的应用切走
        flags |= Qt::WindowDoesNotAcceptFocus;
#endif
        this->setWindowFlags(flags);

        // 半透明背景：必须在窗口真正创建之前设置
        this->setAttribute(Qt::WA_TranslucentBackground);
        this->windowFrameInitialized = true;
    }

    this->setFixedSize(config->getWidth(), config->getHeight());
    this->setWindowOpacity(config->getOpacity());

    // 屏幕边界限制 / 贴边吸附
    this->applyEdgeSnap();


    // shadow 投影
    // 只创建一次：QWidget::setGraphicsEffect 会接管 effect 的所有权，
    // 重复创建（每次拖动松手都会 setUiFrame）会在析构时造成重复释放。
    if (this->winShadow == nullptr)
    {
        this->winShadow = new QGraphicsDropShadowEffect();
        winShadow->setOffset(0, 0);
        winShadow->setColor(Qt::black);
        this->setGraphicsEffect(winShadow);
    }
    // 阴影颜色与长度都可配置；长度设 0 = 无阴影（直接停用效果，
    // 否则模糊半径 0 会沿着小球画出一圈实心色边）
    winShadow->setColor(QColor(config->getShadowColor()));
    winShadow->setBlurRadius(config->getShadowRadius());
    winShadow->setEnabled(config->getShadowRadius() > 0);

    // charts rows
    // charts rows
    qint32 charts_rows = config->getChartsRows();
    if (charts_rows < 1) { charts_rows = 1; }
    // 长度不够就补 0，太长就裁掉 —— 两个方向都要处理，
    // 否则把"图表行数"调小后曲线会一直画出球外
    auto fitHistory = [charts_rows](QVector<quint64> *v) {
        while (v->size() > charts_rows) v->pop_front();
        while (v->size() < charts_rows) v->push_back(0);
    };
    fitHistory(&this->mem_data_history);
    fitHistory(&this->swap_data_history);
    {   // CPU 占用是 double，单独处理
        QVector<double> &v = this->cpuUsage_data_history;
        while (v.size() > charts_rows) v.pop_front();
        while (v.size() < charts_rows) v.push_back(0.0);
    }

    // timer
    this->updateDataTimer->stop();
    this->updateDataTimer->setInterval(config->getUpdateDataInterval());
    this->updateDataTimer->start();

    this->updateUITimer->stop();
    this->updateUITimer->setInterval(config->getUpdateUIInterval());
    this->updateUITimer->start();

    // 桌面环境适配（Xorg / Wayland / macOS 行为不同）
    this->applyDesktopBehavior();

//    this->update();

}

// 桌面环境适配
void Widget::applyDesktopBehavior()
{
    const QString platform = QGuiApplication::platformName();
    const bool isX11     = platform.startsWith(QLatin1String("xcb"));
    const bool isWayland = platform.startsWith(QLatin1String("wayland"));
    const bool isMac     = platform.startsWith(QLatin1String("cocoa"));

    // ------------------------------------------------ macOS
    if (isMac)
    {
#if defined(Q_OS_MACOS)
        // 让小球跟随所有桌面(Space)、切到别的应用时不隐藏、全屏应用之上也可见。
        // 配合 Qt::Tool，它不会出现在 Dock 和 Cmd-Tab 里。
        const unsigned long long wid = static_cast<unsigned long long>(this->winId());
        popballApplyMacWindowBehavior(wid);
#endif
        return;
    }

    // ------------------------------------------------ Wayland
    // Wayland 协议不允许客户端给顶层窗口定位，setGeometry 里的 x/y 会被合成器忽略，
    // 因此拖动小球、以及"关掉窗口后记住位置"在 Wayland 下不会生效 —— 这是协议限制，
    // 不是程序 bug。半透明与无边框本身是支持的，正常显示即可，不做额外处理。
    // 另外，多数合成器也不提供"始终置顶"，实际层级由桌面环境决定。
    if (isWayland)
        return;

    // ------------------------------------------------ X11 (Xorg)
    // 若桌面没开混成(compositing)，半透明不会被混合，窗口四角会露出黑色矩形。
    // 这时退回「圆形形状蒙版」：直接把窗口裁成圆的，矩形四角根本不存在。
    //
    // 混成是"会变的"——用户可能在运行中开关 compositor。这里只做首次判断；
    // 之后的重复检测搭在 updateUITimer 上（每 kCompositingCheckEveryUiTicks 次
    // UI 刷新查一次，见 onTimerIntervalForUpdateUI），不额外占一个定时器。
    Q_UNUSED(isX11);
    this->reevaluateShapeMask();
}

// 重新评估"是否需要圆形蒙版"，并在结果发生变化时才应用。
// 只有「自动」模式跟随桌面混成；「强制开/关」时结果恒定（自动判断不准时用来手动兜底）。
void Widget::reevaluateShapeMask()
{
    // 只有 Xorg 需要：Wayland / macOS 半透明恒可用，从不加蒙版。
    // 改由 updateUITimer 周期调用后，这里必须自带平台判断，否则会在别的平台上误加蒙版。
    if (!QGuiApplication::platformName().startsWith(QLatin1String("xcb")))
        return;

    bool needMask = false;
    switch (config->getShapeMask())
    {
    case 1:   needMask = true;  break;      // 强制开启
    case 2:   needMask = false; break;      // 强制关闭
    default:                                // 自动：跟随桌面混成
#if defined(POPBALL_HAVE_X11)
        needMask = x11CompositingMissing();
#endif
        break;
    }

    // 状态没变就别重复 setMask/clearMask —— 那会触发多余的窗口系统调用
    if (this->shapeMaskInitialized && needMask == this->shapeMaskApplied)
        return;
    this->shapeMaskInitialized = true;
    this->shapeMaskApplied     = needMask;
    this->applyShapeMask(needMask);
}

// 圆形形状蒙版
void Widget::applyShapeMask(bool on)
{
    if (!on)
    {
        if (!this->mask().isEmpty())
            this->clearMask();
        if (this->winShadow != nullptr)
            this->winShadow->setEnabled(config->getShadowRadius() > 0);
        this->setWindowOpacity(config->getOpacity());
        return;
    }

    // 没有混成时，投影和窗口整体透明度都无法真正混合（只会变成黑边或无效），先关掉
    if (this->winShadow != nullptr)
        this->winShadow->setEnabled(false);
    this->setWindowOpacity(1.0);

    // 画一个圆取它的 alpha 蒙版：范围正好覆盖小球(含边框)。
    // 内缩 1px 是为了避开抗锯齿边缘残留的半透明像素 —— 没混成时那圈会显示成黑边。
    const qint32 sr = config->getShadowRadius();
    QImage maskImg(this->size(), QImage::Format_ARGB32_Premultiplied);
    maskImg.fill(Qt::transparent);
    {
        QPainter p(&maskImg);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.drawEllipse(QRect(sr, sr,
                            this->width()  - sr * 2,
                            this->height() - sr * 2).adjusted(1, 1, -1, -1));
    }
    this->setMask(QBitmap::fromImage(maskImg.createAlphaMask()));
}

void Widget::onTimerIntervalForUpdateData()
{
    this->updateDataAndHistory();
}

void Widget::onTimerIntervalForUpdateUI()
{
    // 桌面混成状态不必每次 UI 刷新都查（XGetSelectionOwner 是一次 X 往返）。
    // 每 kCompositingCheckEveryUiTicks 次刷新查一次即可自适应（默认 10 × 450ms ≈ 4.5s）。
    // reevaluateShapeMask() 内部已做平台判断，非 Xorg 平台会直接返回，零副作用。
    if (++this->compositingTickCounter >= kCompositingCheckEveryUiTicks)
    {
        this->compositingTickCounter = 0;
        this->reevaluateShapeMask();
    }
    this->update();
}

void Widget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        this->isMousePressed = true;
        this->curWindowPos = event->pos();
    } else if (event->button() == Qt::RightButton) {
        // 右键弹出菜单（设置 / 退出）
        if (this->contextMenu != nullptr) {
            this->contextMenu->popup(event->globalPosition().toPoint());
        }
    }
}

void Widget::mouseMoveEvent(QMouseEvent *event)
{
    if (this->isMousePressed == true)
    {
        this->move(event->pos() - this->curWindowPos + this->pos());
    }
}

void Widget::mouseReleaseEvent(QMouseEvent *event)
{
    // store to config
    this->config->setX(this->frameGeometry().x());
    this->config->setY(this->frameGeometry().y());

    // 只做边界限制/贴边吸附。
    // 不能调 setUiFrame()：它会重设窗口 flags 与半透明属性，
    // 触发原生窗口重建，KWin 解除管理后小球会整窗透明消失。
    this->applyEdgeSnap();

    Q_UNUSED(event);
    this->isMousePressed = false;
}

// ---------------------------------------------------------------- 右键菜单
void Widget::buildContextMenu()
{
    if (this->contextMenu != nullptr)
        return;

    this->contextMenu = new QMenu(this);
    this->actSettings = this->contextMenu->addAction(tr("设置"));
    this->actSystemMonitor = this->contextMenu->addAction(tr("系统监视器"));
    this->contextMenu->addSeparator();
    this->actQuit     = this->contextMenu->addAction(tr("退出"));

    connect(this->actSettings,      &QAction::triggered, this, &Widget::onMenuSettings);
    connect(this->actSystemMonitor, &QAction::triggered, this, &Widget::onMenuSystemMonitor);
    connect(this->actQuit,          &QAction::triggered, this, &Widget::onMenuQuit);
}

void Widget::onMenuSettings()
{
    // 打开扁平化风格的设置窗口（非模态，方便边改边看小球效果）
    if (this->settingsDialog == nullptr) {
        this->settingsDialog = new SettingsDialog(this->config, this);
        connect(this->settingsDialog, &SettingsDialog::settingsApplied,
                this, &Widget::onSettingsApplied);
    }
    this->settingsDialog->loadFromConfig();
    this->settingsDialog->show();
    this->settingsDialog->raise();
    this->settingsDialog->activateWindow();
}

// —————————————————————— 系统监视器通用启动辅助（所有平台） ——————————————————————

// 同一监视器“单实例 + 成败感知”：用堆上“永不释放”的 QProcess 跟踪。
// 子进程结束时才释放进程对象；popball 退出时这些对象仍未被销毁，
// 因此不会误杀已经打开的监视器窗口，也让“是否仍在运行”可被查询。
static QHash<QString, QProcess *> &liveMonitors()
{
    static QHash<QString, QProcess *> *m = new QHash<QString, QProcess *>;
    return *m;
}

// 启动一个监视器进程；返回 true = 启动成功或在运行，false = 启动失败。
// key 决定“单实例”归属：同一 key 已在运行就不再重复启动（避免堆窗口）。
static bool launchMonitorOnce(const QString &key, const QString &program,
                              const QStringList &args, QString *errMsg)
{
    auto &live = liveMonitors();
    if (QProcess *p = live.value(key))
        if (p->state() != QProcess::NotRunning) {
            if (errMsg)
                errMsg->clear();
            return true;
        }

    auto *p = new QProcess;
    p->setWorkingDirectory(QDir::homePath());
    p->setProgram(program);
    p->setArguments(args);
    p->start();
    if (!p->waitForStarted(1500)) {
        if (errMsg)
            *errMsg = p->errorString().isEmpty()
                      ? QStringLiteral("无法启动 %1").arg(program)
                      : p->errorString();
        p->deleteLater();
        live.remove(key);
        return false;
    }

    live.insert(key, p);
    QObject::connect(p, &QProcess::stateChanged, p, [p, key](QProcess::ProcessState st) {
        if (st == QProcess::NotRunning) {
            auto &m = liveMonitors();
            if (m.value(key) == p)
                m.remove(key);
            p->deleteLater();
        }
    });
    return true;
}

// 展开路径里的 ~（如 ~/bin/htop → /home/xxx/bin/htop）。
static QString expandHome(const QString &p)
{
    if (p == QLatin1String("~"))
        return QDir::homePath();
    if (p.startsWith(QLatin1String("~/")))
        return QDir::homePath() + p.mid(1);
    return p;
}

// 解释器 / 代码执行器：若拿它们去执行一段代码片段，等价于放行任意命令。
// 系统监视器只会是普通程序，绝无可能用 `sh -c 代码`、`python 脚本` 这类调用。
static bool isShellLikeInterpreter(const QString &program)
{
    const QString base = QFileInfo(program).fileName().toLower();
    static const QStringList iters = {
        QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("dash"),
        QStringLiteral("zsh"), QStringLiteral("fish"), QStringLiteral("ksh"),
        QStringLiteral("csh"), QStringLiteral("tcsh"), QStringLiteral("pwsh"),
        QStringLiteral("python"), QStringLiteral("python2"), QStringLiteral("python3"),
        QStringLiteral("php"), QStringLiteral("perl"), QStringLiteral("ruby"),
        QStringLiteral("node"), QStringLiteral("awk"), QStringLiteral("busybox"),
        QStringLiteral("env"), QStringLiteral("xargs"),
    };
    return iters.contains(base);
}

// 判断参数里是否带“执行代码”类开关（-c / -e / --command / -exec / -eval）。
static bool hasEvalFlag(const QStringList &args)
{
    static const QStringList flags = {
        QStringLiteral("-c"), QStringLiteral("-e"), QStringLiteral("--command"),
        QStringLiteral("-eval"), QStringLiteral("-exec"),
    };
    for (const QString &a : args)
        for (const QString &f : flags)
            if (a == f)
                return true;
    return false;
}

// 危险的系统命令：作为“系统监视器”被调用毫无正当性，
// 一旦执行可能删数据 / 提权 / 关系统 / 杀进程，因此一律拒绝（即使不带 shell 符号）。
static bool isDangerousProgram(const QString &program)
{
    QString base = QFileInfo(program).fileName().toLower();
    if (base.endsWith(QLatin1String(".exe")))
        base.chop(4);
    static const QStringList danger = {
        // 删除 / 破坏数据
        QStringLiteral("rm"), QStringLiteral("rmdir"), QStringLiteral("unlink"),
        QStringLiteral("shred"), QStringLiteral("wipe"), QStringLiteral("wipefs"),
        QStringLiteral("dd"), QStringLiteral("mkfs"), QStringLiteral("format"),
        QStringLiteral("parted"), QStringLiteral("fdisk"), QStringLiteral("sfdisk"),
        QStringLiteral("fsutil"), QStringLiteral("diskpart"),
        // 提权
        QStringLiteral("sudo"), QStringLiteral("doas"), QStringLiteral("su"),
        QStringLiteral("gksu"), QStringLiteral("gksudo"), QStringLiteral("kdesudo"),
        QStringLiteral("pkexec"), QStringLiteral("runas"), QStringLiteral("kdesu"),
        // 电源 / 系统控制
        QStringLiteral("reboot"), QStringLiteral("shutdown"), QStringLiteral("poweroff"),
        QStringLiteral("halt"), QStringLiteral("telinit"), QStringLiteral("init"),
        QStringLiteral("systemctl"), QStringLiteral("sv"), QStringLiteral("rcctl"),
        // 杀进程
        QStringLiteral("kill"), QStringLiteral("killall"), QStringLiteral("killall5"),
        QStringLiteral("pkill"), QStringLiteral("taskkill"),
    };
    return danger.contains(base) || base.startsWith(QLatin1String("mkfs."));
}

// 统一入口：无论命令从“整串路径”还是“程序+参数”进来，都先过这道闸。
// 命中任一危险情况就返回 true（已写好给用户的理由），并拒绝执行。
static bool isBlockedCommand(const QString &program, const QStringList &args, QString *errMsg)
{
    if (isShellLikeInterpreter(program) && hasEvalFlag(args)) {
        if (errMsg) *errMsg = QStringLiteral("为避免执行任意代码，不允许用解释器（sh/bash/python 等）执行代码片段");
        return true;
    }
    if (isDangerousProgram(program)) {
        if (errMsg) *errMsg = QStringLiteral("不允许执行危险的系统命令（如 rm / sudo / reboot / kill 等）");
        return true;
    }
    return false;
}

// 解析“命令串”并尽力启动。支持的写法：
//   * 程序名（在 PATH 里找）
//   * 绝对/相对路径，含形如 `C:\Program Files\…\mon.exe` 这种“没引号但含空格”的整串路径
//   * 带引号与参数的命令，例如 `open -a "Activity Monitor"`
//   * 带 ~ 的路径
//   * macOS 纯应用名（如 "Activity Monitor"）→ 用 `open -a` 启动
static bool launchMonitorPath(const QString &exe, const QStringList &args, QString *errMsg);
static bool launchMonitorCommand(const QString &cmdline, QString *errMsg)
{
    const QString cmd = cmdline.trimmed();
    if (cmd.isEmpty()) {
        if (errMsg) *errMsg = QStringLiteral("命令为空");
        return false;
    }
    // 危险字符拦截（关键）：只允许“单条程序 + 参数”，杜绝 shell 运算符。
    // 即使配置被篡改成 shell 一行式（如 `rm -rf ~ ; reboot`），也在此拒绝执行。
    if (!isSafeMonitorCommandLine(cmd)) {
        if (errMsg) *errMsg = QStringLiteral("命令包含不允许的字符（; | & < > ` $() 或换行）");
        return false;
    }
    const QString expanded = expandHome(cmd);            // 处理 ~
    const QStringList parts = QProcess::splitCommand(expanded);
    if (parts.isEmpty()) {
        if (errMsg) *errMsg = QStringLiteral("无法解析命令");
        return false;
    }

    // ① 整串本身就是一个“真实存在”的路径 → 整个当单个程序起，不再拆参数。
    //    典型场景是 Windows 上直接粘贴 `C:\Program Files\…\mon.exe`（含空格没引号）。
    //    注意只能用 exists()：若用 isAbsolute() 会把“路径+参数”这种串误判成路径，
    //    例如 `C:\Program Files\mon.exe --flag` 也会被当成完整程序名去 exec。
    {
        const QFileInfo whole(expanded);
        if (whole.exists()) {
            // 整串当单个程序，也照样过“危险命令”闸，防止用 /bin/rm 这类绕过。
            if (isBlockedCommand(expanded, {}, errMsg))
                return false;
            return launchMonitorPath(expanded, QStringList(), errMsg);
        }
    }

    // ② 常规：程序 + 参数
    const QString program = expandHome(parts.first());
    const QStringList args = parts.mid(1);

    // 危险拦截：禁止解释器执行代码 / 禁止 rm 等破坏性、提权、系统控制、杀进程命令。
    if (isBlockedCommand(program, args, errMsg))
        return false;

    QString exe = QStandardPaths::findExecutable(program);
    const bool pathLike = program.contains(QLatin1Char('/'))
                          || program.contains(QLatin1Char('\\'))
                          || program.startsWith(QLatin1Char('~'));
    if (exe.isEmpty() && (pathLike || QFileInfo(program).exists()))
        exe = program;

    if (exe.isEmpty()) {
        // ③ macOS：再试按“应用名”启动（如 "Activity Monitor"）
#if defined(Q_OS_MACOS)
        if (launchMonitorOnce(QStringLiteral("openapp:") + program,
                              QStringLiteral("open"),
                              QStringList{ QStringLiteral("-a"), program }, nullptr))
            return true;
#endif
        if (errMsg) *errMsg = QStringLiteral("未找到程序：%1").arg(program);
        return false;
    }
    return launchMonitorPath(exe, args, errMsg);
}

// 启动一条可执行路径（自动处理 macOS 的 .app 目录包）。
static bool launchMonitorPath(const QString &exe, const QStringList &args, QString *errMsg)
{
#if defined(Q_OS_MACOS)
    if (exe.endsWith(QLatin1String(".app"), Qt::CaseInsensitive)) {
        return launchMonitorOnce(QStringLiteral("open:") + QDir::cleanPath(exe),
                                 QStringLiteral("open"),
                                 { QDir::toNativeSeparators(exe) }, errMsg);
    }
#endif
    return launchMonitorOnce(QStringLiteral("prog:") + exe, exe, args, errMsg);
}

#if defined(Q_OS_LINUX)
// htop / top 是 TTY 程序，没有界面，必须放进一个终端模拟器里跑。
// 优先用 $TERMINAL；再按 Wayland 友好的现代终端优先排序，xterm 兜底。
static bool launchInTerminal(const QString &program)
{
    const QString termEnv = QString::fromLocal8Bit(qgetenv("TERMINAL")).trimmed();
    if (!termEnv.isEmpty()) {
        QStringList preArgs{ QStringLiteral("-e"), program };
        // 无参风格终端（kitty 等）不接受 -e，改用直接接命令的形式
        const QStringList bare{ QStringLiteral("--"), program };
        if (launchMonitorOnce(QStringLiteral("tty:") + program, termEnv, preArgs, nullptr)
            || launchMonitorOnce(QStringLiteral("tty:") + program, termEnv, bare, nullptr))
            return true;
    }
    struct TermDef { const char *bin; QStringList prefix; };
    const TermDef terms[] = {
        { "x-terminal-emulator", { QStringLiteral("-e") } },
        { "gnome-terminal",      { QStringLiteral("--") } },
        { "xfce4-terminal",      { QStringLiteral("-x") } },
        { "konsole",             { QStringLiteral("-e") } },
        { "kitty",               { } },
        { "foot",                { QStringLiteral("-e") } },
        { "alacritty",           { QStringLiteral("-e") } },
        { "mate-terminal",       { QStringLiteral("-e") } },
        { "lxterminal",          { QStringLiteral("-e") } },
        { "terminator",          { QStringLiteral("-e") } },
        { "wezterm",             { QStringLiteral("start"), QStringLiteral("--") } },
        { "xterm",               { QStringLiteral("-e") } },
    };
    for (const TermDef &t : terms) {
        const QString exe = QStandardPaths::findExecutable(QString::fromLatin1(t.bin));
        if (exe.isEmpty())
            continue;
        QStringList args = t.prefix;
        args << program;
        if (launchMonitorOnce(QStringLiteral("tty:") + program, exe, args, nullptr))
            return true;
    }
    return false;
}

// 桌面监视器找不到（很可能装的是 Flatpak 版）时的 Flatpak 包名映射。
static const QHash<QString, QString> &flatpakMonitorMap()
{
    static const QHash<QString, QString> m = {
        { QStringLiteral("gnome-system-monitor"), QStringLiteral("org.gnome.SystemMonitor") },
        { QStringLiteral("plasma-systemmonitor"), QStringLiteral("org.kde.systemmonitor") },
        { QStringLiteral("systemmonitor"),         QStringLiteral("org.kde.systemmonitor") },
        { QStringLiteral("ksysguard"),             QStringLiteral("org.kde.ksysguard") },
        { QStringLiteral("xfce4-taskmanager"),     QStringLiteral("org.xfce.xtaskmanager") },
        { QStringLiteral("mate-system-monitor"),   QStringLiteral("org.mate.SystemMonitor") },
        { QStringLiteral("lxtask"),                QStringLiteral("org.lxde.lxtask") },
    };
    return m;
}

// PATH 里没有该监视器时，判断它是否以 Flatpak 方式安装并启动。
static bool launchFlatpakMonitor(const QString &name, QString *errMsg)
{
    const QString appId = flatpakMonitorMap().value(name);
    if (appId.isEmpty())
        return false;
    if (QStandardPaths::findExecutable(QStringLiteral("flatpak")).isEmpty())
        return false;

    // 会话内缓存一次“已安装的 flatpak 应用”列表，避免每次点菜单都起子进程。
    static QStringList cached;
    if (cached.isEmpty()) {
        QProcess qp;
        qp.start(QStringLiteral("flatpak"),
                 { QStringLiteral("list"), QStringLiteral("--app"),
                   QStringLiteral("--columns=application") });
        if (!qp.waitForStarted(1500) || !qp.waitForFinished(3000))
            return false;
        const QList<QByteArray> lines = qp.readAllStandardOutput().split('\n');
        for (const QByteArray &line : lines) {
            const QString s = QString::fromUtf8(line).trimmed();
            if (!s.isEmpty())
                cached << s;
        }
    }
    if (!cached.contains(appId))
        return false;
    return launchMonitorOnce(QStringLiteral("flatpak:") + appId,
                             QStringLiteral("flatpak"), { QStringLiteral("run"), appId }, errMsg);
}

// 按名字启动一个 GUI 监视器：优先 PATH，找不到再试 Flatpak 版。
static bool launchGuiMonitor(const QString &name, QString *errMsg)
{
    const QString exe = QStandardPaths::findExecutable(name);
    if (!exe.isEmpty())
        return launchMonitorOnce(QStringLiteral("prog:") + exe, exe, {}, errMsg);
    return launchFlatpakMonitor(name, errMsg);
}
#endif // Q_OS_LINUX

// 系统监视器右键菜单动作。规则：
//   1. 配置了自定义命令 → 严格启动它；启动失败才弹窗提示（不再回退自动检测）。
//   2. 未配置 → 按 操作系统 + 桌面环境 自动挑选，整套统一走“单实例 + 成败感知”。
void Widget::onMenuSystemMonitor()
{
    const QString customCmd = this->config->getSystemMonitorCmd();

    // ---------------- ① 用户自定义命令 ----------------
    if (!customCmd.trimmed().isEmpty()) {
        QString err;
        if (!launchMonitorCommand(customCmd, &err)) {
            qWarning() << "系统监视器命令启动失败:" << customCmd << err;
            QMessageBox::warning(this, tr("系统监视器启动失败"),
                tr("配置的「系统监视器」命令无法启动：\n%1\n\n%2\n\n"
                   "请确认它是可执行命令或程序路径（可在「设置 → 系统监视器」"
                   "用“选择程序”指定），\n或暂时留空以走自动检测。")
                    .arg(customCmd, err));
        }
        return;
    }

    // ---------------- ② 自动检测 ----------------
#if defined(Q_OS_MACOS)
    QString macErr;
    if (!launchMonitorOnce(QStringLiteral("openapp:Activity Monitor"),
                           QStringLiteral("open"),
                           QStringList{ QStringLiteral("-a"), QStringLiteral("Activity Monitor") },
                           &macErr))
        QMessageBox::warning(this, tr("系统监视器"),
            tr("无法打开「活动监视器」。\n%1").arg(macErr));
#elif defined(Q_OS_WIN)
    QString winErr;
    if (!launchMonitorOnce(QStringLiteral("taskmgr"), QStringLiteral("taskmgr"), {}, &winErr))
        QMessageBox::warning(this, tr("系统监视器"),
            tr("无法打开「任务管理器」。\n%1").arg(winErr));
#elif defined(Q_OS_LINUX)
    // 收集桌面环境/会话提示（可能多个来源，合并匹配）
    const QStringList hints = {
        QString::fromLocal8Bit(qgetenv("XDG_CURRENT_DESKTOP")),
        QString::fromLocal8Bit(qgetenv("XDG_SESSION_DESKTOP")),
        QString::fromLocal8Bit(qgetenv("DESKTOP_SESSION")),
    };
    const QString joined = hints.join(QLatin1Char(' ')).toLower();

    QStringList specific;
    if (joined.contains(QLatin1String("kde")) || joined.contains(QLatin1String("plasma")))
        specific = { QStringLiteral("plasma-systemmonitor"),
                     QStringLiteral("systemmonitor"),
                     QStringLiteral("ksysguard") };
    else if (joined.contains(QLatin1String("xfce")))
        specific = { QStringLiteral("xfce4-taskmanager") };
    else if (joined.contains(QLatin1String("mate")))
        specific = { QStringLiteral("mate-system-monitor") };
    else if (joined.contains(QLatin1String("lxqt")))
        specific = { QStringLiteral("qps"), QStringLiteral("lxtask") };
    else if (joined.contains(QLatin1String("lxde")))
        specific = { QStringLiteral("lxtask") };
    else if (joined.contains(QLatin1String("deepin")) || joined.contains(QLatin1String("dde")))
        specific = { QStringLiteral("deepin-system-monitor") };
    else if (joined.contains(QLatin1String("cinnamon")))
        specific = { QStringLiteral("gnome-system-monitor") };
    // else：GNOME / Unity / Budgie / Pantheon / GNOME Flashback 及纯 WM，首选留空

    // 桌面和“实际装了哪个”未必一致（如用 LXDE 却只装 gnome-system-monitor）。
    // 把“首选”之外的所有已知 GUI 监视器追加进去兜底 —— 哪个装了用哪个。
    const QStringList pool = {
        QStringLiteral("gnome-system-monitor"),
        QStringLiteral("plasma-systemmonitor"),
        QStringLiteral("systemmonitor"),
        QStringLiteral("ksysguard"),
        QStringLiteral("mate-system-monitor"),
        QStringLiteral("xfce4-taskmanager"),
        QStringLiteral("lxtask"),
        QStringLiteral("qps"),
        QStringLiteral("deepin-system-monitor"),
    };

    QStringList candidates = specific;
    for (const QString &name : pool)
        if (!candidates.contains(name))
            candidates.append(name);

    QString err;
    bool launched = false;
    for (const QString &name : candidates)
        if (launchGuiMonitor(name, &err)) {
            launched = true;
            break;
        }

    // 桌面 GUI 监视器都没有 → 退回「终端里的 htop / top」。
    if (!launched) {
        const QString tty = QStandardPaths::findExecutable(QStringLiteral("htop")).isEmpty()
                            ? QStringLiteral("top")
                            : QStringLiteral("htop");
        launched = launchInTerminal(tty);
    }

    if (!launched) {
        qWarning() << "未找到可用的系统监视器或终端." << err;
        QMessageBox::warning(this, tr("系统监视器"),
            tr("找不到可用的系统监视器或终端模拟器。\n%1\n\n"
               "可安装 gnome-system-monitor / plasma-systemmonitor / ksysguard /\n"
               "xfce4-taskmanager / mate-system-monitor / lxtask / qps /\n"
               "deepin-system-monitor（或 htop/… + 终端），\n"
               "也可在「设置 → 系统监视器」里指定命令。").arg(err));
    }
#else
    qWarning() << "当前平台不支持系统监视器";
#endif
}

// 设置保存后：把新颜色套到 LCD、按新的不透明度/显示项重刷界面
void Widget::onSettingsApplied()
{
    this->applyLcdLayout();   // 尺寸可能变了
    this->applyLcdStyle();
    this->setUiFrame();       // 不透明度/阴影/定时器/形状蒙版在这里重套
    this->update();
}

void Widget::quitApplication()
{
    // 安全退出：先停掉定时器、隐藏窗口，再让事件循环退出。
    // 先隐藏是为了避免退出瞬间在桌面上残留半透明窗口或黑框。
    if (this->updateDataTimer != nullptr) this->updateDataTimer->stop();
    if (this->updateUITimer   != nullptr) this->updateUITimer->stop();
    if (this->contextMenu     != nullptr) this->contextMenu->hide();
    this->hide();
    QCoreApplication::quit();
}

void Widget::onMenuQuit()
{
    this->quitApplication();
}

void Widget::updateDataAndHistory()
{
    this->sysInfo->updateSysinfo();
    // cpu usage history
    if ((cpuUsage_data_history.size() + 1) >= config->getChartsRows()) cpuUsage_data_history.pop_front();
    this->cpuUsage_data_history.push_back(this->sysInfo->getCpuUsage());
    // mem history
    if ((mem_data_history.size() + 1) >= config->getChartsRows()) mem_data_history.pop_front();
    this->mem_data_history.push_back(this->sysInfo->getMemUsed());
    // swap history
    if ((swap_data_history.size() + 1) >= config->getChartsRows()) swap_data_history.pop_front();
    this->swap_data_history.push_back(this->sysInfo->getSwapUsed());
}

void Widget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // ui shape
    switch (this->config->getShape())
    {
    case SHAPE_CIRCLE:
    case SHAPE_ASIDE: // tmp
    {

        qint32 main_border_width    = config->getMainBorderWidth();
        qint32 shadow_radius        = config->getShadowRadius();
        qint32 main_width           = config->getWidth();
        qint32 main_height          = config->getHeight();
        qint32 charts_rows          = config->getChartsRows();
        qint32 edging_width         = config->getMainBorderWidth() + config->getShadowRadius();

        if (charts_rows < 1) { charts_rows = 1; }   // 防止除零


        // main circle && border
        painter.setBrush(QColor(config->getMainColor()));
        QPen pen(QColor(config->getMainBorderColor()), main_border_width, Qt::SolidLine, Qt::SquareCap, Qt::RoundJoin);
        painter.setPen(pen);
        painter.drawEllipse(
                    main_border_width/2 + shadow_radius,
                    main_border_width/2 + shadow_radius,
                    main_width  - main_border_width - (shadow_radius * 2),
                    main_height - main_border_width - (shadow_radius * 2) );

        // clip
        QPainterPath clipPath;
        clipPath.moveTo(main_border_width/2 + shadow_radius, main_border_width/2 + shadow_radius);
        clipPath.arcTo(main_border_width/2 + shadow_radius + 2,
                       main_border_width/2 + shadow_radius + 2,
                       main_width  - main_border_width - (shadow_radius * 2) - 4,
                       main_height - main_border_width - (shadow_radius * 2) - 4,
                       0, 360);
        painter.setClipPath(clipPath);

        // cpu freq LCD（平台/发行版取不到频率时不显示）
        if (config->getCpuFreqShow() == SHOW && this->sysInfo->isCpuFreqAvailable())
        {
            this->cpuFreqLCD->display(QString("CPU %1").arg(qRound(this->sysInfo->getCpuFreq())));
            if (this->cpuFreqLCD->isHidden())
            {
                this->cpuFreqLCD->show();
            }

        }
        else
        {
            if (this->cpuFreqLCD->isHidden() == false) {
                this->cpuFreqLCD->hide();
            }
        }

        // temp LCD（当前平台无可用温度传感器时不显示，避免一直显示 0）
        if (config->getCpuTempShow() == SHOW && this->sysInfo->isCpuTemperatureAvailable())
        {
            this->cpuTempLCD->display(QString("%1'c").arg(qRound(this->sysInfo->getCpuTemperature())));
            if (this->cpuTempLCD->isHidden())
            {
                this->cpuTempLCD->show();
            }
        }
        else
        {
            if (this->cpuTempLCD->isHidden() == false)
            {
                this->cpuTempLCD->hide();
            }
        }

        if (config->getNetSpeedShow() == SHOW)
        {

            // 网速自适应显示：固定 MiB/s，小数位随数值自适应。
            // 注意单位：update_data_interval 是 QTimer 的毫秒数（450 = 0.45 秒），
            // getTransmit() 是"这个间隔内"的字节数，必须除以"秒数"才是 B/s。
            // 原实现直接除以 450，数值小了 1000 倍（下载几 MB/s 只显示 0.00x）。
            const int intervalMs = this->config->getUpdateDataInterval();
            const double seconds = intervalMs > 0 ? intervalMs / 1000.0 : 1.0;
            this->netUploadLCD->display(
                QString("u %1").arg(formatNetSpeedField(this->sysInfo->getTransmit() / seconds)));
            this->netDownloadLCD->display(
                QString("d %1").arg(formatNetSpeedField(this->sysInfo->getReceive() / seconds)));

            if (this->netUploadLCD->isHidden() || this->netDownloadLCD->isHidden())
            {
                this->netUploadLCD->show();
                this->netDownloadLCD->show();
            }
        }
        else
        {
            if (this->netUploadLCD->isHidden() == false || this->netDownloadLCD->isHidden() == false)
            {
                this->netUploadLCD->hide();
                this->netDownloadLCD->hide();
            }
        }

        // mem charts（内存不可用或总量为 0 时不绘制，避免除零）
        quint64 mem_total = this->sysInfo->getMemTotal();
        if (this->sysInfo->isMemAvailable() && mem_total > 0)
        {
            QPainterPath memPath;
            memPath.moveTo(0, main_height - edging_width);
            for (int i=0; i<this->mem_data_history.size(); i++)
            {
                const double ratio = double(this->mem_data_history[i]) / double(mem_total);
                memPath.lineTo(double(main_width) / charts_rows * i,
                               main_height - ratio * main_height - edging_width);
            }
            memPath.lineTo(main_width, main_height - edging_width);
            memPath.lineTo(0, main_height - edging_width);
            painter.fillPath(memPath, QColor(this->config->getMemColor()));
        }

        // swap charts（没有交换分区时不绘制，避免除零）
        quint64 swap_total = this->sysInfo->getSwapTotal();
        if (this->sysInfo->isSwapAvailable() && swap_total > 0)
        {
            QPainterPath swapPath;
            swapPath.moveTo(0, main_height - edging_width);
            for (int i=0; i<this->swap_data_history.size(); i++)
            {
                const double ratio = double(this->swap_data_history[i]) / double(swap_total);
                swapPath.lineTo(double(main_width) / charts_rows * i,
                               main_height - ratio * main_height - edging_width);
            }
            swapPath.lineTo(main_width, main_height - edging_width);
            swapPath.lineTo(0, main_height);
            painter.fillPath(swapPath, QColor(this->config->getSwapColor()));
        }

        // cpu usage charts
        QPen cpuUsagePen;
        cpuUsagePen.setColor(config->getCpuUsageColor());
        cpuUsagePen.setStyle(Qt::SolidLine);
        cpuUsagePen.setWidthF(config->getCpuUsageWidth());
        painter.setPen(cpuUsagePen);
        QVector<double> cpuUsageData = this->cpuUsage_data_history;
        // QPointF cpuUsagePoints[charts_rows];
//        for (int i=0; i<charts_rows; i++)
//        {
//            cpuUsagePoints[i] = QPointF(main_width / charts_rows * i, main_height - (cpuUsageData[i]) - edging_width);
//        }
//        painter.drawPolyline(cpuUsagePoints, charts_rows);
        QPainterPath cpuUsagePath;
        for (int i=0; i<cpuUsageData.size(); i++)
        {
            double usage = cpuUsageData[i];
            if (!qIsFinite(usage)) { usage = 0.0; }   // 首次采样无基准，可能是 NaN
            if (usage < 0.0)   { usage = 0.0; }
            if (usage > 100.0) { usage = 100.0; }
            cpuUsagePath.lineTo(double(main_width) / charts_rows * i,
                                main_height - (usage / 100.0) * main_height - edging_width);

        }
        cpuUsagePath.lineTo(main_width, main_height - edging_width);
        cpuUsagePath.lineTo(0, main_height);
        painter.fillPath(cpuUsagePath, QColor(this->config->getCpuUsageColor()));


        painter.end();
        break;
    }
//    case SHAPE_ASIDE:
//    {
//        this->cpuFreqLCD->hide();
//        qint32 main_border_width    = config->getMainBorderWidth();
//        qint32 shadow_radius        = config->getShadowRadius();
//        qint32 main_width           = config->getWidth();
//        qint32 main_height          = config->getHeight();

//        // main color
//        painter.setBrush(QColor(config->getMainColor()));
//        QPen pen(QColor(config->getMainBorderColor()), main_border_width, Qt::SolidLine, Qt::SquareCap, Qt::RoundJoin);
//        painter.setPen(pen);
//        painter.drawEllipse(
//                    main_border_width/2 + shadow_radius,
//                    main_border_width/2 + shadow_radius,
//                    main_width  - main_border_width - (shadow_radius * 2),
//                    main_height - main_border_width - (shadow_radius * 2) );



//        painter.end();
//        break;
//    }

    default: break;

    }
}
