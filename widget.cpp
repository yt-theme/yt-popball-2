#include "widget.h"

#include "popdock.h"
#if defined(Q_OS_MACOS)
#  include "macwindow.h"
#endif

#include <QBitmap>
#include <QImage>
#include <QRegion>
#include <QMimeData>
#include <QMessageBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QStandardPaths>
#include <QtMath>

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

// 磁盘 IO 速度格式化：MB/s、保留两位小数、居中文本（UI 不显示单位，纯数值）。
//   返回如 "12.34" / "123.45" 的字符串；超过 999999 时钳住，避免数字过宽被圆边切掉。
// update_ui_interval 默认 450ms，N=10 ≈ 每 4.5s 查一次 —— 不额外占一个定时器、
// 也不频繁打扰 X 服务器；用户切换混成后能在几秒内自动去/回黑框。
constexpr int kCompositingCheckEveryUiTicks = 10;

// ---------------- 贴边竖条（SHAPE_ASIDE）的几个距离常量 ----------------
// 小球"可视边缘"离屏幕左右边缘这么近（px）就吸上去变成竖条。
// 留一点容差，不用非得把球怼到屏幕外面才触发。
constexpr int kAsideSnapThreshold = 12;// 竖条状态下，往屏幕里侧拖超过这么多像素算"要把球拉出来"。
// 必须明显大于 kAsideSnapThreshold：松开时小球正好离边缘十几像素，
// 否则松开又会被立刻吸回竖条，看起来像"拉不出来"。
constexpr int kAsideDetachPx = 16;
// 单击展开后小球离屏幕边缘留出的空隙（要比 kAsideSnapThreshold 大，免得一松手又吸回去）
constexpr int kAsidePopOutGap = kAsideSnapThreshold + 8;

// 按下后总位移不超过这么多像素 = 单击（用于"单击球/竖条开关 PopDock 面板"的判定）。
// 超过它就算拖动：拖动会立刻收起面板，避免面板挡着球。
constexpr int kBallClickPx = 4;

// ---------------- 悬浮球形态（ball_style）相关常量 ----------------
// 0=球形（默认） 1=圆角矩形 2=直角方形 3=长条形。
// 长条形是横向长条：宽 190、高 64（含阴影留边；LCD 按 2 行横排适配）。
constexpr int kBallBar      = 3;
constexpr int kBallBarW     = 190;
constexpr int kBallBarH     = 64;

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
    // 磁盘读写：按配置选定生效盘（0=IO最高的盘，1=指定盘）
    this->sysInfo->setDiskSelection(config->getDiskIoMode(), config->getDiskIoName());

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
    this->cpuTempLCD->display("00'c");
    // cpu freq LCD
    this->cpuFreqLCD = new QLCDNumber(this);
    this->cpuFreqLCD->setDigitCount(9);
    this->cpuFreqLCD->setMode(QLCDNumber::Dec);
    this->cpuFreqLCD->setSegmentStyle(QLCDNumber::Flat);
    this->cpuFreqLCD->display("000000000");
    // net upload LCD
    this->netUploadLCD = new QLCDNumber(this);
    this->netUploadLCD->setDigitCount(7);   // "u 01.23" 共 7 字符（小数点占 1 位）
    this->netUploadLCD->setMode(QLCDNumber::Dec);
    this->netUploadLCD->setSegmentStyle(QLCDNumber::Flat);
    this->netUploadLCD->display("u 00.00");
    // net downlod LCD
    this->netDownloadLCD = new QLCDNumber(this);
    this->netDownloadLCD->setDigitCount(7);
    this->netDownloadLCD->setMode(QLCDNumber::Dec);
    this->netDownloadLCD->setSegmentStyle(QLCDNumber::Flat);
    this->netDownloadLCD->display("d 00.00");
    // 磁盘总速度 LCD：MB/s、两位小数（数值本身已是 MB，单位无法用数码字体呈现故省略）
    this->diskIoLCD = new QLCDNumber(this);
    this->diskIoLCD->setDigitCount(6);          // "123.45" 共 6 字符预留
    this->diskIoLCD->setMode(QLCDNumber::Dec);
    this->diskIoLCD->setSegmentStyle(QLCDNumber::Flat);
    this->diskIoLCD->display("0.00");

    // 各行按"当前可见行"自适应排布（首帧 paintEvent 会按真实显隐再收紧一次）
    this->applyLcdLayout();

    // LCD 前景色（设置里改颜色后也会重新套用）
    this->applyLcdStyle();

    // hover 300ms 弹出的工具/中转站面板：球上停留 / 拖文件到球上 → 打开面板
    this->hoverTimer = new QTimer(this);
    this->hoverTimer->setSingleShot(true);
    this->hoverTimer->setInterval(300);
    connect(this->hoverTimer, &QTimer::timeout, this, &Widget::onHoverTimeout);
    this->hideTimer = new QTimer(this);
    this->hideTimer->setSingleShot(true);
    this->hideTimer->setInterval(250);
    connect(this->hideTimer, &QTimer::timeout, this, &Widget::onHideTimeout);

    // 轻量轮询：即使窗口/应用未激活（如 macOS 后台不投递 enter/move 事件），
    // 也靠全局光标坐标判断是否在球/面板上，从而在未激活时移上去也能弹出面板。
    this->hoverPollTimer = new QTimer(this);
    this->hoverPollTimer->setInterval(100);
    connect(this->hoverPollTimer, &QTimer::timeout, this, &Widget::onHoverPoll);
    this->hoverPollTimer->start();

    this->popDock = new PopDock(this);
    connect(this->popDock, &PopDock::mouseEntered, this, &Widget::cancelHidePopDock);
    connect(this->popDock, &PopDock::mouseLeft,   this, &Widget::scheduleHidePopDock);
    // 面板里正在操作（右键菜单 / 拖出条目）时取消已排队的收起
    connect(this->popDock, &PopDock::interactionStarted, this, &Widget::cancelHidePopDock);
    // 中转站面板的展示布局（图标/列表/详细）：按配置回填，用户切换时落盘
    connect(this->popDock, &PopDock::viewStyleChanged, this, [this](int style) {
        this->config->setDockViewStyle(style);
    });
    this->popDock->setViewStyle(this->config->getDockViewStyle());
    // 弹窗设置（尺寸 / 内容密度 / 背景不透明度）按配置回填
    this->popDock->applyDockSettings(this->config->getDockWidth(),
                                     this->config->getDockHeight(),
                                     this->config->getDockDensity());
    this->popDock->setDockOpacity(this->config->getDockOpacity());
    // 弹窗是否记住上次滚动位置（默认不记住 → 每次打开从顶部开始）
    this->popDock->setRememberScroll(this->config->getRememberScroll());
    // 面板内"激活/选中"样式跟随主题强调色（main_border_color）
    this->popDock->setAccentColor(QColor(this->config->getMainBorderColor()));
    // 面板右上角"操作"菜单（设置 / 系统监视器 / 退出）→ 原悬浮球右键菜单的动作
    connect(this->popDock, &PopDock::settingsRequested, this, &Widget::onMenuSettings);
    connect(this->popDock, &PopDock::systemMonitorRequested, this, &Widget::onMenuSystemMonitor);
    connect(this->popDock, &PopDock::quitRequested, this, &Widget::onMenuQuit);
    this->setAcceptDrops(true);   // 允许把文件拖到球上
}

// 按配置尺寸重新摆放各 LCD
// 改"大小"后必须调用：LCD 的几何位置只在构造时算过一次，
// 不重摆的话小球变大了数字还挤在原来的小区域里
void Widget::applyLcdLayout()
{
    this->relayoutVisibleLcds();
}

// 依 lcdRowMask 把"当前可见"的信息行铺在球内。
//   球形：自上而下均匀铺开，行宽按所在高度的弦长收缩（文字不被圆边切掉）；
//   圆角矩形 / 直角方形：同样竖排，但可用宽度是满的（没有圆边挤压）；
//   长条形：改为 2 行横排 —— 第 1 行 温度|频率，第 2 行 网速上|网速下|磁盘，
//   每行内按实际可见项数均分列宽，字号由 QLCDNumber 按控件尺寸自动缩放。
//   只显示 3 行时行高自动变大，5 行时自动压缩——既不留空洞，也不会被挤出球外。
void Widget::relayoutVisibleLcds()
{
    const qint32 w = this->width();     // 窗口实际尺寸（长条形态与配置宽不同）
    const qint32 h = this->height();
    const int ballStyle = config->getBallStyle();

    // mask 为 0 时（还没跑过 paintEvent）按"全显示"先摆一套合理几何，首帧后会按真实显隐收紧
    const quint8 mask = (this->lcdRowMask != 0) ? this->lcdRowMask : quint8(0x1F);
    int visible = 0;
    for (int i = 0; i < ROW_COUNT; ++i)
        if (mask & (1 << i)) ++visible;
    if (visible <= 0) return;

    // ---------------- 长条形：2 行横排 ----------------
    if (ballStyle == kBallBar) {
        // 第 1 行：温度 / 频率；第 2 行：网速上 / 网速下 / 磁盘
        const int rowOf[ROW_COUNT] = { 0, 0, 1, 1, 1 };
        const int topH  = qRound(h * 0.40);   // 第一行（温度/频率，数值更宽）
        const int botY  = qRound(h * 0.44);
        const int botH  = h - botY;
        const int pad   = 2;
        int row0Cnt = 0, row1Cnt = 0;
        for (int i = 0; i < ROW_COUNT; ++i)
            if (mask & (1 << i)) { (rowOf[i] == 0 ? row0Cnt : row1Cnt)++; }
        const int x0 = pad, w0 = (w - pad * 2) / qMax(1, row0Cnt);
        const int x1 = pad, w1 = (w - pad * 2) / qMax(1, row1Cnt);
        int c0 = 0, c1 = 0;
        for (int i = 0; i < ROW_COUNT; ++i) {
            if (!(mask & (1 << i))) continue;
            if (rowOf[i] == 0) {
                this->lcdRowRect[i] = QRect(x0 + c0 * w0, qMax(1, topH / 4), w0 - pad, topH - topH / 2);
                ++c0;
            } else {
                this->lcdRowRect[i] = QRect(x1 + c1 * w1, botY, w1 - pad, botH);
                ++c1;
            }
        }
        const double netScale = 0.8;
        QLCDNumber *lcds[ROW_COUNT] = {
            this->cpuTempLCD, this->cpuFreqLCD,
            this->diskIoLCD,
            this->netUploadLCD, this->netDownloadLCD
        };
        for (int i = 0; i < ROW_COUNT; ++i) {
            if (!(mask & (1 << i)) || lcds[i] == nullptr) continue;
            QRect r = this->lcdRowRect[i];
            if (i == ROW_DISK || i == ROW_NET_UP || i == ROW_NET_DOWN) {
                const int hh = qMax(1, qRound(r.height() * netScale));
                r.setTop(r.top() + (r.height() - hh) / 2);
                r.setHeight(hh);
            }
            lcds[i]->setGeometry(r);
        }
        return;
    }

    // ---------------- 球形 / 圆角矩形 / 直角方形：竖直排布 ----------------
    const bool isCircle = (ballStyle == 0);

    // 行高上限同时参考宽度和高度，避免可见行少时字体被撑得离谱
    const double maxRowH = qMin(double(w) / 5.0, double(h) / 5.5);
    const double avail   = double(h) * 0.84;      // 可用于信息行的总高度（居中区域）
    const double rowH    = qMin(maxRowH, avail / visible);

    // 行按"类"分组：网速的上行/下行属于同一类（类内不加间距、贴在一起），
    // 温度 / 频率 / 磁盘各自成类。富余空间只匀给类与类之间的间隔，
    // 所以"温度+网速"、"温度+频率"时上下带间距，网速上下行之间没有空隙。
    const int group[ROW_COUNT] = { 0, 1, 2, 3, 3 };   // NET_UP / NET_DOWN 同组
    int classGapCount = 0;
    int prevGroup = -1;
    for (int i = 0; i < ROW_COUNT; ++i) {
        if (!(mask & (1 << i))) continue;
        if (prevGroup >= 0 && group[i] != prevGroup) ++classGapCount;
        prevGroup = group[i];
    }
    const double gap = (classGapCount > 0)
                       ? qMax(0.0, qMin((avail - rowH * visible) / classGapCount, rowH * 0.9))
                       : 0.0;
    const double totalH  = rowH * visible + gap * classGapCount;
    const double startY  = (h - totalH) / 2.0;   // 整体垂直居中
    const double r       = qMin(double(w), double(h)) / 2.0;

    double y = startY;
    prevGroup = -1;
    for (int i = 0; i < ROW_COUNT; ++i) {
        if (!(mask & (1 << i))) continue;
        // 跨"类"才插间距；同一类内（网速上下行）贴在一起
        if (prevGroup >= 0 && group[i] != prevGroup) y += gap;

        // 球是圆的：越靠上/下的行可用宽度越窄，按该行中心高度处的弦长收缩左右边界，
        // 免得文字被球体圆边切掉两头。方形 / 圆角矩形没有圆边挤压，可用全宽。
        double usable = double(w) * 0.88;
        if (isCircle) {
            const double cy = y + rowH / 2.0;
            const double dy = qAbs(cy - double(h) / 2.0);
            const double halfChord = (dy < r) ? qSqrt(r * r - dy * dy) : 0.0;
            usable = qMax(double(w) * 0.35, halfChord * 2.0 * 0.98);
        }

        const QRect rect(qRound((w - usable) / 2.0), qRound(y),
                         qRound(usable), qRound(rowH));
        this->lcdRowRect[i] = rect;
        y += rowH;
        prevGroup = group[i];
    }

    // 各信息行都是 QLCDNumber（它会把数字按控件尺寸缩放）
    const double netScale = 0.82;   // 网速行比温度/频率小一点，但比之前略大（用户要求"再大一点点"）
    QLCDNumber *lcds[ROW_COUNT] = {
        this->cpuTempLCD, this->cpuFreqLCD,
        this->diskIoLCD,                          // 磁盘行也用 LCD 数码字体（与其它行一致）
        this->netUploadLCD, this->netDownloadLCD
    };
    for (int i = 0; i < ROW_COUNT; ++i) {
        if (!(mask & (1 << i)) || lcds[i] == nullptr) continue;
        QRect r = this->lcdRowRect[i];
        if (i == ROW_DISK || i == ROW_NET_UP || i == ROW_NET_DOWN) {
            // 磁盘行与网速行：同样压缩高度并垂直居中，使数字字号一致（用户要求"硬盘速度字大小和网速一样"）
            const int hh = qMax(1, qRound(r.height() * netScale));
            r.setTop(r.top() + (r.height() - hh) / 2);
            r.setHeight(hh);
        }
        lcds[i]->setGeometry(r);
    }
}

// 把配置里的前景色套到各 LCD
void Widget::applyLcdStyle()
{
    this->cpuTempLCD->setStyleSheet("border: 0;color:" + config->getCpuTempColor() + ";");
    this->cpuFreqLCD->setStyleSheet("border: 0;color:" + config->getCpuFreqColor() + ";");
    this->netUploadLCD->setStyleSheet("border: 0;color:" + config->getNetSpeedColor() + ";");
    this->netDownloadLCD->setStyleSheet("border: 0;color:" + config->getNetSpeedColor() + ";");
    this->diskIoLCD->setStyleSheet("border: 0;color:" + config->getDiskIoColor() + ";");
}

Widget::~Widget()
{
    delete this->config;
    delete this->sysInfo;
    delete this->updateDataTimer;
    delete this->updateUITimer;
    // winShadow 已通过 setGraphicsEffect() 交给 QWidget 托管，不在这里删除（会重复释放）
    delete this->cpuTempLCD;
    delete this->cpuFreqLCD;
    delete this->netUploadLCD;
    delete this->netDownloadLCD;
    delete this->diskIoLCD;
    // popDock 以 this 为父对象，由 Qt 在 QWidget 析构时自动回收，这里不可重复 delete
}

// 某个形状对应的窗口尺寸。
// 窗口比"看得见的形状"大一圈：四周留出 shadow_radius，用来放投影/半透明过渡。
// 圆形形态 = 配置里的 width/height；竖条形态 = aside_width/aside_height + 留边，
// 这样竖条本身（不含留边）正好是用户设置的宽高。
QSize Widget::sizeForShape(qint32 shape) const
{
    if (shape == SHAPE_ASIDE && this->config->getSnapToEdge())
        return QSize(this->config->getAsideWidth()  + this->config->getShadowRadius() * 2,
                     this->config->getAsideHeight() + this->config->getShadowRadius() * 2);
    // 长条形：横向长条，尺寸固定（LCD 文字按此适配）
    if (this->config->getBallStyle() == kBallBar)
        return QSize(kBallBarW + this->config->getShadowRadius() * 2,
                     kBallBarH + this->config->getShadowRadius() * 2);
    return QSize(this->config->getWidth(), this->config->getHeight());
}

// 窗口当前所在的屏幕区域。多显示器时按窗口中心取屏，避免副屏上的球被吸到主屏边缘。
QRect Widget::currentScreenRect() const
{
    if (QScreen *scr = QGuiApplication::screenAt(this->geometry().center()))
        return scr->geometry();
    if (QScreen *scr = QGuiApplication::primaryScreen())
        return scr->geometry();
    return QRect(0, 0, 1920, 1080);
}

void Widget::setPosition()
{
    const QSize s = this->sizeForShape(this->config->getShape());
    this->setGeometry(this->config->getX(), this->config->getY(), s.width(), s.height());
}

// 屏幕边界限制与贴边吸附（setUiFrame 初始化时、以及每次拖动松手时调用）。
// 只调整 shape 标记和几何位置，不碰窗口 flags / 半透明属性，
// 因此可以安全地在窗口已显示后反复调用（setFixedSize/setGeometry 不会重建原生窗口）。
//
// 两种形态：
//   SHAPE_CIRCLE —— 普通圆球，被限制在屏幕内
//   SHAPE_ASIDE  —— 拖到屏幕左/右边缘后吸附成的圆角竖条，只能沿边缘上下移动
void Widget::applyEdgeSnap()
{
    const QRect  screen = this->currentScreenRect();
    const qint32 sr     = this->config->getShadowRadius();   // 四周留边
    // 屏幕的四条边（绝对坐标）。多显示器时副屏的 left/top 可能不为 0，
    // 用 left+width 之类的相对量去算位置会整体偏掉，所以这里统一用绝对边界。
    const qint32 scrLeft   = screen.left();
    const qint32 scrRight  = screen.left() + screen.width();    // 右边界（不含）
    const qint32 scrTop    = screen.top();
    const qint32 scrBottom = screen.top() + screen.height();

    qint32 shape = this->config->getShape();
    // 关掉「贴边变竖条」时永远回到圆球
    if (this->config->getSnapToEdge() == 0
        || this->config->getAsideWidth() <= 0 || this->config->getAsideHeight() <= 0)
        shape = SHAPE_CIRCLE;

    // ------------------------------------------------ 竖条形态
    if (shape == SHAPE_ASIDE)
    {
        const QSize s = this->sizeForShape(SHAPE_ASIDE);
        const bool left = (this->config->getAsideEdge() == ASIDE_LEFT);

        // 贴边：可视区域的外侧边缘正好落在屏幕边缘上
        // （窗口左侧留边恒为 sr，所以左贴边时 x = scrLeft - sr；右贴边时右边要留出 sr）
        qint32 x = left ? (scrLeft - sr) : (scrRight - this->config->getAsideWidth() - sr);
        // 纵向：整根竖条都留在屏幕内
        qint32 y = this->config->getY();
        const qint32 minY = scrTop - sr;
        const qint32 maxY = scrBottom - this->config->getAsideHeight() - sr;
        y = qBound(minY, y, qMax(minY, maxY));

        this->config->setShape(SHAPE_ASIDE);
        this->config->setX(x);
        this->config->setY(y);
        this->hideAllLcds();
        this->setFixedSize(s);
        this->setGeometry(x, y, s.width(), s.height());
        this->refreshMaskAfterShapeChange();
        return;
    }

    // ------------------------------------------------ 圆形形态
    const QSize s = this->sizeForShape(SHAPE_CIRCLE);
    // 屏幕边界限制：以"可视区域"为准（可视区域 = 窗口去掉四周留边 sr）
    qint32 x = qBound(scrLeft - sr, this->config->getX(),
                      qMax(scrLeft - sr, scrRight - s.width() + sr));
    qint32 y = qBound(scrTop - sr, this->config->getY(),
                      qMax(scrTop - sr, scrBottom - s.height() + sr));

    // 可视区域左右边缘
    const qint32 visLeft  = x + sr;
    const qint32 visRight = x + s.width() - sr;

    qint32 snapEdge = -1;
    if (visLeft <= scrLeft + kAsideSnapThreshold)
        snapEdge = ASIDE_LEFT;
    else if (visRight >= scrRight - kAsideSnapThreshold)
        snapEdge = ASIDE_RIGHT;

    if (snapEdge >= 0)
    {
        // 吸成竖条：纵向保持小球原来的中心，避免形态切换时上下跳
        const qint32 asideW = this->config->getAsideWidth();
        const qint32 asideH = this->config->getAsideHeight();
        const QSize  as     = this->sizeForShape(SHAPE_ASIDE);

        qint32 ax = (snapEdge == ASIDE_LEFT) ? (scrLeft - sr)
                                             : (scrRight - asideW - sr);
        qint32 ay = y + s.height() / 2 - asideH / 2;
        const qint32 minY = scrTop - sr;
        const qint32 maxY = scrBottom - asideH - sr;
        ay = qBound(minY, ay, qMax(minY, maxY));

        this->config->setAsideEdge(snapEdge);
        this->config->setShape(SHAPE_ASIDE);
        this->config->setX(ax);
        this->config->setY(ay);
        this->hideAllLcds();
        this->setFixedSize(as);
        this->setGeometry(ax, ay, as.width(), as.height());
        this->refreshMaskAfterShapeChange();
        return;
    }

    // 留在圆球形态
    this->config->setShape(SHAPE_CIRCLE);
    this->config->setX(x);
    this->config->setY(y);
    this->setToolTip(QString());          // 悬停气泡只属于竖条形态
    this->setFixedSize(s);
    this->setGeometry(x, y, s.width(), s.height());
    this->refreshMaskAfterShapeChange();
}

// 形态变了（圆球 ↔ 竖条）之后，若正开着形状蒙版，必须按新形状重做一次：
// 蒙版是按窗口尺寸画的，圆球的圆形蒙版套到竖条上会把竖条裁得只剩中间一坨。
void Widget::refreshMaskAfterShapeChange()
{
    if (!this->shapeMaskApplied)
        return;
    this->applyShapeMask(true);
}

// 小球上的 LCD（温度/频率/磁盘/网速）在竖条形态下一律隐藏：
// 竖条只有几十像素宽，LCD 会被挤在里面显示成一堆残缺数字。
void Widget::hideAllLcds()
{
    if (this->cpuTempLCD     != nullptr && !this->cpuTempLCD->isHidden())     this->cpuTempLCD->hide();
    if (this->cpuFreqLCD     != nullptr && !this->cpuFreqLCD->isHidden())     this->cpuFreqLCD->hide();
    if (this->netUploadLCD   != nullptr && !this->netUploadLCD->isHidden())   this->netUploadLCD->hide();
    if (this->netDownloadLCD != nullptr && !this->netDownloadLCD->isHidden()) this->netDownloadLCD->hide();
    if (this->diskIoLCD      != nullptr && !this->diskIoLCD->isHidden())      this->diskIoLCD->hide();
}

// 由"贴边竖条"变回小球。
// keepUnderCursor = true  → 拖动脱离：小球紧贴光标摆开，松手后接着就能继续拖
// keepUnderCursor = false → 单击展开：小球从边缘"弹"出来一点，不贴着边缘
void Widget::detachToCircle(const QPoint &globalCursor, bool keepUnderCursor)
{
    const QSize  s   = this->sizeForShape(SHAPE_CIRCLE);
    const qint32 sr  = this->config->getShadowRadius();
    const QRect  screen = this->currentScreenRect();
    const bool   left   = (this->config->getAsideEdge() == ASIDE_LEFT);

    // 小球"可视区域"靠边缘的那条边要落在屏幕的哪个 x 上
    const qint32 nearEdge = keepUnderCursor
                            ? globalCursor.x()
                            : (left ? screen.left() + kAsidePopOutGap
                                    : screen.left() + screen.width() - kAsidePopOutGap);

    const qint32 x = left ? (nearEdge - sr) : (nearEdge - (s.width() - sr));
    const qint32 y = keepUnderCursor
                     ? (globalCursor.y() - s.height() / 2)
                     : (this->config->getY() + this->config->getAsideHeight() / 2 - s.height() / 2);

    this->config->setShape(SHAPE_CIRCLE);
    this->setFixedSize(s);
    this->move(x, y);
    this->config->setX(x);
    this->config->setY(y);

    // 抓取点定在"靠边缘那条边、光标所在高度"上：
    // 拖动位移算法用的是窗口内坐标，形态刚变完必须同步一次，才不会有跳变
    this->curWindowPos   = QPoint(left ? sr : (s.width() - sr), globalCursor.y() - y);
    this->pressGlobalPos = globalCursor;

    this->refreshMaskAfterShapeChange();
    this->setToolTip(QString());
    this->update();
}

// 竖条上要画的各指标。顺序 = 柱子的左右顺序：CPU 占用 / 内存 / 交换分区。
// 内存或交换分区在本机拿不到时（比如没有交换分区）对应柱子直接不画。
QVector<AsideMetric> Widget::collectAsideMetrics() const
{
    QVector<AsideMetric> metrics;

    // CPU 占用（百分比）
    {
        double usage = this->cpuUsage_data_history.isEmpty()
                       ? this->sysInfo->getCpuUsage()
                       : this->cpuUsage_data_history.back();
        if (!qIsFinite(usage)) usage = 0.0;                  // 首次采样没有基准，可能是 NaN
        metrics.append({ QStringLiteral("CPU"),
                         qBound(0.0, usage / 100.0, 1.0),
                         QColor(this->config->getCpuUsageColor()) });
    }

    // 内存
    {
        const quint64 total = this->sysInfo->getMemTotal();
        const quint64 used  = this->mem_data_history.isEmpty()
                              ? this->sysInfo->getMemUsed()
                              : this->mem_data_history.back();
        if (this->sysInfo->isMemAvailable() && total > 0)
            metrics.append({ tr("内存"),
                             qBound(0.0, double(used) / double(total), 1.0),
                             QColor(this->config->getMemColor()) });
    }

    // 交换分区
    {
        const quint64 total = this->sysInfo->getSwapTotal();
        const quint64 used  = this->swap_data_history.isEmpty()
                              ? this->sysInfo->getSwapUsed()
                              : this->swap_data_history.back();
        if (this->sysInfo->isSwapAvailable() && total > 0)
            metrics.append({ tr("交换"),
                             qBound(0.0, double(used) / double(total), 1.0),
                             QColor(this->config->getSwapColor()) });
    }

    return metrics;
}

// 绘制贴边竖条：圆角矩形 + 一组窄柱图。
// 一根柱子 = 一个指标，柱高与当前值成正比；柱底固定，柱顶随数值上下走。
void Widget::drawAsideBar(QPainter &painter)
{
    const qint32 bw = this->config->getMainBorderWidth();
    const qint32 sr = this->config->getShadowRadius();

    // 竖条矩形：与圆形形态同样地内缩"边框一半 + 留边"，
    // 这样贴边时条身正好压在屏幕边缘上、投影留在屏幕外。
    const QRectF bar(sr + bw / 2.0, sr + bw / 2.0,
                     this->width()  - bw - sr * 2,
                     this->height() - bw - sr * 2);
    if (bar.width() <= 2.0 || bar.height() <= 2.0)
        return;

    qreal radius = qBound(qreal(0.0), qreal(this->config->getAsideCornerRadius()),
                          qMin(bar.width(), bar.height()) / 2.0);

    QPainterPath barPath;
    barPath.addRoundedRect(bar, radius, radius);

    // 条身 + 边框（复用小球的主色/边框色，皮肤保持一致）
    painter.setBrush(QColor(this->config->getMainColor()));
    painter.setPen(QPen(QColor(this->config->getMainBorderColor()), bw,
                        Qt::SolidLine, Qt::SquareCap, Qt::RoundJoin));
    painter.drawPath(barPath);

    painter.save();
    painter.setClipPath(barPath);       // 柱子不许画出圆角之外
    painter.setPen(Qt::NoPen);

    const QVector<AsideMetric> metrics = this->collectAsideMetrics();
    const int n = metrics.size();

    // 内边距：圆角越大留得越多，柱子不会顶到圆角上；
    // 但条子做得很窄时（用户可把宽度调到 20px 上下），内边距必须跟着缩，
    // 否则左右一扣就没地方画柱子了。所以再叠一个"占条宽比例"的上限。
    const qreal padX = qMax<qreal>(2.0, qMin(radius * 0.45, bar.width()  * 0.18));
    const qreal padY = qMax<qreal>(3.0, qMin(radius * 0.35, bar.height() * 0.06));
    const QRectF inner = bar.adjusted(padX, padY, -padX, -padY);

    if (n > 0 && inner.width() > 1.0 && inner.height() > 2.0)
    {
        qreal gap  = qBound<qreal>(1.5, inner.width() * 0.11, 3.5);
        qreal colW = (inner.width() - gap * (n - 1)) / n;
        if (colW < 1.5) { gap = 0.0; colW = inner.width() / n; }   // 指标多/条窄时不留缝

        for (int i = 0; i < n; ++i)
        {
            const qreal x = inner.left() + i * (colW + gap);

            // 轨道：整条浅色底，让人看出"满格"在哪。
            // 必须够淡：太实的话柱子占三成也会被读成满格。
            QColor trackColor = metrics[i].color;
            trackColor.setAlpha(qBound(22, qRound(trackColor.alpha() * 0.14), 40));
            QPainterPath trackPath;
            trackPath.addRoundedRect(QRectF(x, inner.top(), colW, inner.height()),
                                     colW / 2.0, colW / 2.0);
            painter.fillPath(trackPath, trackColor);

            // 柱身：柱高 = 占比 × 可用高度（有序号也保证至少看得见一小截）
            if (metrics[i].ratio <= 0.0)
                continue;
            qreal h = inner.height() * metrics[i].ratio;
            if (metrics[i].ratio > 0.005 && h < colW)
                h = colW;
            h = qMin(h, inner.height());

            QColor barColor = metrics[i].color;
            barColor.setAlpha(255);      // 指标色在配置里可能带透明度，窄柱上必须实心才看得清
            const qreal r2 = qMin(colW / 2.0, h / 2.0);
            QPainterPath colPath;
            colPath.addRoundedRect(QRectF(x, inner.bottom() - h, colW, h), r2, r2);
            painter.fillPath(colPath, barColor);
        }
    }

    painter.restore();
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

    // 起始尺寸按当前形态取：上次退出时若停在贴边竖条，这次也要以竖条尺寸起步
    this->setFixedSize(this->sizeForShape(this->config->getShape()));
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

// 重新评估"是否需要形状蒙版"，并在结果发生变化时才应用。
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

    // 状态没变就别重复 setMask/clearMask —— 那会触发多余的窗口系统调用。
    // 但形态（圆球 ↔ 竖条）变了必须重做：蒙版是按窗口尺寸画的，形状不对会裁错。
    const qint32 shape = this->config->getShape();
    if (this->shapeMaskInitialized && needMask == this->shapeMaskApplied
        && shape == this->shapeMaskAppliedShape)
        return;
    this->shapeMaskInitialized    = true;
    this->shapeMaskApplied        = needMask;
    this->shapeMaskAppliedShape   = shape;
    this->applyShapeMask(needMask);
}

// 形状蒙版：桌面没开混成时，把窗口裁成"看得见的形状"，
// 没用上的透明区域直接被裁掉，桌面就不会透出黑色矩形。
//   圆球形态 → 圆形蒙版    竖条形态 → 圆角矩形蒙版
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

    // 画一个形状取它的 alpha 蒙版：范围正好覆盖小球/竖条(含边框)。
    // 内缩 1px 是为了避开抗锯齿边缘残留的半透明像素 —— 没混成时那圈会显示成黑边。
    const qint32 sr = config->getShadowRadius();
    QImage maskImg(this->size(), QImage::Format_ARGB32_Premultiplied);
    maskImg.fill(Qt::transparent);
    {
        QPainter p(&maskImg);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);

        const QRect shapeRect = QRect(sr, sr,
                                      this->width()  - sr * 2,
                                      this->height() - sr * 2).adjusted(1, 1, -1, -1);
        if (config->getShape() == SHAPE_ASIDE)
        {
            const qreal radius = qBound(qreal(0.0), qreal(config->getAsideCornerRadius()),
                                        qMin(shapeRect.width(), shapeRect.height()) / 2.0);
            p.drawRoundedRect(shapeRect, radius, radius);
        }
        else
        {
            // 非贴边形态的蒙版与背景形状保持一致（球/圆角矩形/直角方形/长条形）
            const int ballStyle = config->getBallStyle();
            switch (ballStyle) {
            case 1:  p.drawRoundedRect(shapeRect, 18, 18); break;   // 圆角矩形
            case 2:  p.drawRect(shapeRect); break;                  // 直角方形
            case 3:  p.drawRoundedRect(shapeRect, 12, 12); break;   // 长条形
            default: p.drawEllipse(shapeRect); break;               // 球形
            }
        }
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
        // 不在这里立刻收起面板：先记住按下前面板开没开，松手时若判定为"单击"
        // 就做开/关切换；只有真的开始拖动（见 mouseMoveEvent）才收起面板。
        this->dockWasVisibleOnPress = (this->popDock != nullptr && this->popDock->isVisible());
        this->pressMoved = false;
        this->cancelHidePopDock();
        this->isMousePressed = true;
        this->curWindowPos = event->pos();
        this->pressGlobalPos = event->globalPosition().toPoint();
    } else if (event->button() == Qt::RightButton) {
        // 悬浮球右键菜单已移除：操作项（设置 / 系统监视器 / 退出）
        // 全部收进中转站面板右上角的"操作"按钮菜单（见 PopDock）。
        // 右键现在不在这里做任何事，避免误触弹菜单。
    }
}

void Widget::mouseMoveEvent(QMouseEvent *event)
{
    if (this->isMousePressed != true)
        return;

    // 位移一旦超过单击阈值就视为"真的在拖动"：收起面板（别挡着球），
    // 之后松手也不再触发"单击开关面板"。
    if (!this->pressMoved &&
        (event->globalPosition().toPoint() - this->pressGlobalPos).manhattanLength() > kBallClickPx) {
        this->pressMoved = true;
        if (this->popDock != nullptr && this->popDock->isVisible())
            this->popDock->hideAnimated();      // 拖动开始：划入收起，别挡着球
    }

    // ---------------- 竖条形态：只沿边缘上下走 ----------------
    if (this->config->getShape() == SHAPE_ASIDE)
    {
        const QPoint globalPos = event->globalPosition().toPoint();
        const QPoint delta     = globalPos - this->pressGlobalPos;
        // "往屏幕里侧"的位移（左贴边看 +x，右贴边看 -x）
        const int inward = (this->config->getAsideEdge() == ASIDE_LEFT) ? delta.x() : -delta.x();

        // 往屏幕里侧拖够远 → 把球拉出来（变回圆球，之后正常拖动）
        if (inward > kAsideDetachPx)
        {
            this->detachToCircle(globalPos, true);
            return;
        }

        // 否则算"沿着边缘上下拖"：横向始终吸附在边缘上，只跟随纵向，手感更像被吸住
        const QRect  screen = this->currentScreenRect();
        const qint32 sr     = this->config->getShadowRadius();
        const bool   left   = (this->config->getAsideEdge() == ASIDE_LEFT);
        const qint32 glueX  = left ? (screen.left() - sr)
                                   : (screen.left() + screen.width() - this->config->getAsideWidth() - sr);
        const qint32 newY   = event->pos().y() - this->curWindowPos.y() + this->pos().y();

        // 纵向限制在屏幕内（可视区域不越界）
        const qint32 minY = screen.top() - sr;
        const qint32 maxY = screen.top() + screen.height() - this->config->getAsideHeight() - sr;
        this->move(glueX, qBound(minY, newY, qMax(minY, maxY)));
        this->config->setX(glueX);
        this->config->setY(this->pos().y());
        return;
    }

    // ---------------- 圆球形态：自由拖动 ----------------
    this->move(event->pos() - this->curWindowPos + this->pos());
}

void Widget::mouseReleaseEvent(QMouseEvent *event)
{
    // 贴边竖条形态：单击不再还原成小球 —— 只有拖动到内侧超过阈值(kAsideDetachPx)
    // 才会由 mouseMoveEvent 里的 detachToCircle 变回圆球（用户明确要求）。
    // 单击竖条/圆球改为"开关 PopDock 面板"。
    this->dragOpening = false;

    // store to config
    this->config->setX(this->pos().x());
    this->config->setY(this->pos().y());

    // 只做边界限制/贴边吸附。
    // 不能调 setUiFrame()：它会重设窗口 flags 与半透明属性，
    // 触发原生窗口重建，KWin 解除管理后小球会整窗透明消失。
    this->applyEdgeSnap();

    // 单击（按下后没有拖动）= 开关面板：原本没开就打开，原本开着就收起。
    // 拖动过（pressMoved）则不动面板，避免拖完球又被弹出面板。
    const bool wasClick = (!this->pressMoved && event->button() == Qt::LeftButton);
    if (wasClick && this->popDock != nullptr) {
        if (this->dockWasVisibleOnPress || this->popDock->isVisible())
            this->popDock->hideAnimated();
        else
            this->showPopDock();
    }
    this->pressMoved = false;
    this->dockWasVisibleOnPress = false;

    this->isMousePressed = false;
}

// ---------------------------------------------------------------- hover 弹出的工具/中转站面板
void Widget::enterEvent(QEnterEvent *event)
{
    Q_UNUSED(event);
    this->cancelHidePopDock();                 // 回到球上取消隐藏计时（轮询另担弹出职责）
}

void Widget::leaveEvent(QEvent *event)
{
    Q_UNUSED(event);
    // 注：hoverTimer 的启停与隐藏调度统一由 onHoverPoll() 负责（兼容未激活窗口），
    // 这里仅保留隐藏的快速触发，轮询也会兜底。
    if (this->popDock != nullptr && this->popDock->isVisible())
        this->scheduleHidePopDock();
}

// 轮询检测光标是否落在"悬浮球或面板"上。即使窗口/应用未激活（后台窗口收不到
// enter/move 事件），用全局光标坐标也能判定，从而实现"未激活时移上去也弹窗"。
void Widget::onHoverPoll()
{
    if (this->popDock == nullptr)
        return;

    // 悬浮球右键菜单已移除；这里的悬停轮询不再需要为菜单让路，
    // 直接按光标与球/面板的位置关系评估即可。
    const QPoint gp = QCursor::pos();
    const bool overBall = this->geometry().contains(gp);
    // 收起动画进行中窗口正在移动，命中判定要按"落点矩形"算，
    // 否则光标停在面板上却因为窗口正在滑走而被判成"离开了"。
    // 另外：面板里正在操作（右键菜单/拖拽）时一律当作"还在面板上"，不能收起。
    const bool locked = this->popDock->isInteractionLocked();
    const bool overDock = locked
                          || (this->popDock->isVisible()
                              && (this->popDock->geometry().contains(gp)
                                  || this->popDock->targetRect().contains(gp)));
    const bool over = overBall || overDock;

    if (over == this->cursorOverUi)
        return;                                // 状态未变，避免重复重置 300ms 计时
    this->cursorOverUi = over;

    if (over) {
        this->cancelHidePopDock();
        if (!this->popDock->isVisible() && !this->isMousePressed)
            this->hoverTimer->start();         // 停留 300ms 才弹
    } else {
        this->hoverTimer->stop();
        if (this->popDock->isVisible())
            this->scheduleHidePopDock();       // 离开球与面板 → 延迟隐藏
    }
}

void Widget::onHoverTimeout()
{
    if (this->isMousePressed) return;          // 正在拖动小球时不弹
    this->showPopDock();
}

void Widget::onHideTimeout()
{
    if (this->popDock == nullptr)
        return;
    // 用户正在面板里操作（右键菜单弹出、拖出条目）→ 取消这次收起，别打断他
    if (this->popDock->isInteractionLocked())
        return;
    this->popDock->hideAnimated();      // 离开球与面板 → 划入收起
}

void Widget::scheduleHidePopDock()
{
    // 面板里正在操作时不排收起：菜单/拖拽期间光标会离开面板（移到菜单上），
    // 那次"离开"不该被当成"用户走开了"。
    if (this->popDock != nullptr && this->popDock->isInteractionLocked())
        return;
    if (this->hideTimer != nullptr)
        this->hideTimer->start();
}

void Widget::cancelHidePopDock()
{
    if (this->hideTimer != nullptr)
        this->hideTimer->stop();
    // 光标又回到球/面板上时，正在播放的收起动画要立刻撤销，否则面板会"擦身而过"地消失
    if (this->popDock != nullptr)
        this->popDock->cancelHideAnimation();
}

void Widget::showPopDock()
{
    if (this->popDock == nullptr)
        return;

    const QPoint target = this->popDockTargetPos();     // 见下方"智能定位"

    // 尺寸：屏幕装不下就收缩（小屏/投影分辨率下不至于溢出屏幕）
    // 注意每次先按首选尺寸算落点，再按屏幕夹一次，避免用在旧尺寸上算出来的位置。
    this->popDock->showAnimated(target, this->geometry());
    this->popDock->raise();
    this->popDock->activateWindow();                    // 拿到焦点，Ctrl+V 粘贴才生效
}

// 面板落点：按人机工程学摆放 ——
//   1) 用 availableGeometry（自动避开菜单栏 / Dock / 任务栏）并留安全边距；
//   2) 横向优先放在"空间更大的一侧"，保证不压住悬浮球；两侧都放不下时选更宽的一侧；
//   3) 纵向与球的中心对齐，再夹紧在屏内（球靠上/靠下时面板自动贴齐屏幕内边）；
//   4) 屏幕比面板还小时收缩面板尺寸，宁可挤一点也不要溢出屏幕。
QPoint Widget::popDockTargetPos()
{
    const QRect ball = this->geometry();

    QScreen *scr = QGuiApplication::screenAt(ball.center());
    if (scr == nullptr)
        scr = QGuiApplication::primaryScreen();
    const QRect sr = scr->availableGeometry();

    const int margin = 12;      // 与屏幕边缘的安全边距
    const int gap    = 10;      // 与悬浮球之间的间隙（不压住球）

    // ---- 尺寸自适应 ----
    // 首选尺寸来自配置（默认 330×452）；屏幕装不下时收缩
    const int prefW = this->popDock->preferredWidth();
    const int prefH = this->popDock->preferredHeight();
    int pw = qMin(prefW, qMax(240, sr.width()  - margin * 2));
    int ph = qMin(prefH, qMax(260, sr.height() - margin * 2));
    if (this->popDock != nullptr)
        this->popDock->setFixedSize(pw, ph);

    // ---- 展示位置：0=自动 1=悬浮球右侧 2=悬浮球左侧 3=屏幕居中 ----
    const int posMode = (this->config != nullptr) ? this->config->getDockPosition() : 0;
    int x, y;
    const int gap10 = gap;
    if (posMode == 1) {                       // 悬浮球右侧
        x = ball.right() + gap10;
    } else if (posMode == 2) {                // 悬浮球左侧
        x = ball.left() - gap10 - pw;
    } else if (posMode == 3) {                // 屏幕居中
        x = sr.center().x() - pw / 2;
        y = sr.center().y() - ph / 2;
        const int minY3 = sr.top() + margin;
        const int maxY3 = qMax(minY3, sr.bottom() - ph - margin);
        y = qBound(minY3, y, maxY3);
        return QPoint(qBound(sr.left() + margin, x, qMax(sr.left() + margin, sr.right() - pw - margin)), y);
    } else {                                  // 0 = 自动：横向选空间更大的一侧
        const int spaceL = ball.left() - sr.left();          // 球左侧可用宽度
        const int spaceR = sr.right() - ball.right();        // 球右侧可用宽度
        const int need   = pw + gap10;
        bool toRight = (spaceR >= spaceL);                   // 默认开在空间更大的一侧
        if (toRight && spaceR < need)
            toRight = false;                                 // 右侧不够 → 翻到左侧
        else if (!toRight && spaceL < need)
            toRight = (spaceR >= spaceL);                    // 左侧也不够 → 谁宽用谁

        x = toRight ? (ball.right() + gap10) : (ball.left() - gap10 - pw);
    }

    // ---- 夹紧在屏内 ----
    const int minX = sr.left() + margin;
    const int maxX = qMax(minX, sr.right() - pw - margin);
    x = qBound(minX, x, maxX);

    // ---- 纵向：与球中心对齐，再夹紧在屏内 ----
    y = ball.center().y() - ph / 2;
    const int minY = sr.top() + margin;
    const int maxY = qMax(minY, sr.bottom() - ph - margin);
    y = qBound(minY, y, maxY);

    return QPoint(x, y);
}

// 把文件拖到球上（停留 300ms 自动开面板）/ 直接松手落到球上也收下
void Widget::dragEnterEvent(QDragEnterEvent *event)
{
    const QMimeData *md = event->mimeData();
    if (md->hasUrls() || md->hasImage() || md->hasText()) {
        event->acceptProposedAction();
        this->dragOpening = true;
        if (this->popDock == nullptr || !this->popDock->isVisible())
            this->hoverTimer->start();          // 拖到球上 300ms 自动开面板
    } else {
        event->ignore();
    }
}

void Widget::dragMoveEvent(QDragMoveEvent *event)
{
    const QMimeData *md = event->mimeData();
    if (md->hasUrls() || md->hasImage() || md->hasText())
        event->acceptProposedAction();
    else
        event->ignore();
}

void Widget::dropEvent(QDropEvent *event)
{
    this->dragOpening = false;
    const QMimeData *md = event->mimeData();
    if (md->hasUrls()) {
        QStringList paths;
        for (const QUrl &u : md->urls())
            paths << u.toLocalFile();
        if (this->popDock) { this->popDock->addFiles(paths); this->showPopDock(); }
        event->acceptProposedAction();
    } else if (md->hasImage()) {
        if (this->popDock) {
            this->popDock->addImage(qvariant_cast<QImage>(md->imageData()));
            this->showPopDock();
        }
        event->acceptProposedAction();
    } else if (md->hasText()) {
        if (this->popDock) { this->popDock->addText(md->text()); this->showPopDock(); }
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void Widget::onMenuSettings()
{
    // 打开扁平化风格的设置窗口（非模态，方便边改边看小球效果）
    if (this->settingsDialog == nullptr) {
        this->settingsDialog = new SettingsDialog(this->config, this->sysInfo, this);
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
                      ? QCoreApplication::translate("Widget", "无法启动 %1").arg(program)
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
        if (errMsg) *errMsg = QCoreApplication::translate("Widget", "命令为空");
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
        if (errMsg) *errMsg = QCoreApplication::translate("Widget", "未找到程序：%1").arg(program);
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
    // 磁盘读写：设置里可能改了"统计哪块盘"
    this->sysInfo->setDiskSelection(config->getDiskIoMode(), config->getDiskIoName());
    this->applyLcdLayout();   // 尺寸可能变了
    this->applyLcdStyle();
    this->setUiFrame();       // 不透明度/阴影/定时器/形状蒙版在这里重套
    // 主题色可能改了 → 数据中转站里的激活/选中样式跟随刷新
    this->popDock->setAccentColor(QColor(this->config->getMainBorderColor()));
    // 弹窗尺寸 / 内容密度 / 背景不透明度可能改了 → 立即应用；若弹窗正开着就按新尺寸/位置重摆
    this->popDock->applyDockSettings(this->config->getDockWidth(),
                                     this->config->getDockHeight(),
                                     this->config->getDockDensity());
    this->popDock->setDockOpacity(this->config->getDockOpacity());
    // 滚动位置记忆开关：下次打开弹窗时按新规则（默认不记住 → 顶部）
    this->popDock->setRememberScroll(this->config->getRememberScroll());
    if (this->popDock->isVisible()) {
        const QPoint target = this->popDockTargetPos();   // 内部已按新尺寸 setFixedSize
        this->popDock->move(target);                      // 直接落位，不重播滑入动画
    }
    this->update();
}

void Widget::quitApplication()
{
    // 安全退出：先停掉定时器、隐藏窗口，再让事件循环退出。
    // 先隐藏是为了避免退出瞬间在桌面上残留半透明窗口或黑框。
    if (this->updateDataTimer != nullptr) this->updateDataTimer->stop();
    if (this->updateUITimer   != nullptr) this->updateUITimer->stop();
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
    if (cpuUsage_data_history.size() >= config->getChartsRows()) cpuUsage_data_history.pop_front();
    this->cpuUsage_data_history.push_back(this->sysInfo->getCpuUsage());
    // mem history
    if (mem_data_history.size() >= config->getChartsRows()) mem_data_history.pop_front();
    this->mem_data_history.push_back(this->sysInfo->getMemUsed());
    // swap history
    if (swap_data_history.size() >= config->getChartsRows()) swap_data_history.pop_front();
    this->swap_data_history.push_back(this->sysInfo->getSwapUsed());

    // 竖条形态下柱子本身没有文字，鼠标悬停时用气泡给出各指标的具体百分比
    if (this->config->getShape() == SHAPE_ASIDE)
    {
        const QVector<AsideMetric> metrics = this->collectAsideMetrics();
        QStringList parts;
        for (const AsideMetric &m : metrics)
            parts << QStringLiteral("%1 %2%").arg(m.label).arg(qRound(m.ratio * 100.0));
        if (!parts.isEmpty())
            this->setToolTip(parts.join(QStringLiteral("\n")));
    }
}

void Widget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // ui shape
    switch (this->config->getShape())
    {
    // 贴边竖条：圆角矩形 + 窄柱图（小球上的 LCD 一律隐藏）
    case SHAPE_ASIDE:
    {
        this->hideAllLcds();
        this->drawAsideBar(painter);
        painter.end();
        break;
    }
    case SHAPE_CIRCLE:   // 球 + 圆角矩形/直角方形/长条形（由 ball_style 决定具体形状）
    {

        qint32 main_border_width    = config->getMainBorderWidth();
        qint32 shadow_radius        = config->getShadowRadius();
        qint32 main_width           = this->width();    // 窗口实际尺寸（长条形态与配置宽不同）
        qint32 main_height          = this->height();
        qint32 charts_rows          = config->getChartsRows();
        qint32 edging_width         = config->getMainBorderWidth() + config->getShadowRadius();

        if (charts_rows < 1) { charts_rows = 1; }   // 防止除零

        // 非贴边形态的"外观"：0=球形（默认）1=圆角矩形 2=直角方形 3=长条形
        const int ballStyle = config->getBallStyle();

        // main shape && border（按形态：球=椭圆，圆角矩形/长条=圆角矩形，方形=直角矩形）
        QColor ballFill(config->getMainColor());
        painter.setBrush(ballFill);
        QPen pen(QColor(config->getMainBorderColor()), main_border_width, Qt::SolidLine, Qt::SquareCap, Qt::RoundJoin);
        painter.setPen(pen);
        const QRectF shapeRect(main_border_width / 2.0 + shadow_radius,
                               main_border_width / 2.0 + shadow_radius,
                               main_width  - main_border_width - (shadow_radius * 2),
                               main_height - main_border_width - (shadow_radius * 2));
        switch (ballStyle) {
        case 1:  painter.drawRoundedRect(shapeRect, 18, 18); break;   // 圆角矩形
        case 2:  painter.drawRect(shapeRect); break;                  // 直角方形
        case 3:  painter.drawRoundedRect(shapeRect, 12, 12); break;   // 长条形
        default: painter.drawEllipse(shapeRect); break;               // 球形（默认）
        }

        // clip（与背景形状一致，避免曲线/LCD 溢出形状边缘）
        QPainterPath clipPath;
        const QRectF clipRect(main_border_width / 2.0 + shadow_radius + 2,
                              main_border_width / 2.0 + shadow_radius + 2,
                              main_width  - main_border_width - (shadow_radius * 2) - 4,
                              main_height - main_border_width - (shadow_radius * 2) - 4);
        switch (ballStyle) {
        case 1:  clipPath.addRoundedRect(clipRect, 16, 16); break;
        case 2:  clipPath.addRect(clipRect); break;
        case 3:  clipPath.addRoundedRect(clipRect, 10, 10); break;
        default: clipPath.moveTo(clipRect.center().x(), clipRect.top());
                 clipPath.arcTo(clipRect, 90, 360);
                 clipPath.closeSubpath();
                 break;
        }
        painter.setClipPath(clipPath);

        // 本轮各 LCD 行的"应显示"位图：决定竖直排布（见 relayoutVisibleLcds）
        quint8 lcdMask = 0;

        // cpu freq LCD（平台/发行版取不到频率时不显示）
        if (config->getCpuFreqShow() == SHOW && this->sysInfo->isCpuFreqAvailable())
        {
            this->cpuFreqLCD->display(QString("CPU %1").arg(qRound(this->sysInfo->getCpuFreq())));
            lcdMask |= (1 << ROW_FREQ);
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
            lcdMask |= (1 << ROW_TEMP);
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
            lcdMask |= (1 << ROW_NET_UP) | (1 << ROW_NET_DOWN);

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

        // 磁盘总速度：读+写之和，MB/s、居中文本、两位小数（实际绘制在下方 charts 之后）
        if (config->getDiskIoShow() == SHOW && this->sysInfo->isDiskIoAvailable())
        {
            lcdMask |= (1 << ROW_DISK);
        }

        // 可见行集合变了（开关了某个指标 / 某项可用性刚探测出来）→ 重排竖直布局
        if (lcdMask != this->lcdRowMask)
        {
            this->lcdRowMask = lcdMask;
            this->relayoutVisibleLcds();
        }

        // mem charts（内存不可用或总量为 0 时不绘制，避免除零）。
        // 长条形（ballStyle==3）不画曲线图：横条太窄，画了也看不清。
        if (ballStyle != 3)
        {
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
            swapPath.lineTo(0, main_height - edging_width);
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
        cpuUsagePath.lineTo(0, main_height - edging_width);
        painter.fillPath(cpuUsagePath, QColor(this->config->getCpuUsageColor()));
        }   // end if (ballStyle != 3) —— 长条形不画曲线图

        // 磁盘总速度：读+写之和，用 LCD 数码字体显示 MB/s 数值（两位小数）。
        // QLCDNumber 画不出字母，单位"MB/s"省略，数值本身已是 MB/s。
        if (config->getDiskIoShow() == SHOW && this->sysInfo->isDiskIoAvailable())
        {
            const int intervalMs = this->config->getUpdateDataInterval();
            const double seconds = intervalMs > 0 ? intervalMs / 1000.0 : 1.0;
            const double total = this->sysInfo->getDiskReadBytes() + this->sysInfo->getDiskWriteBytes();
            const double mb = total / seconds / (1024.0 * 1024.0);   // MB/s
            const QString val = QString::number(mb, 'f', 2);          // 如 "12.34"
            this->diskIoLCD->setDigitCount(val.length());
            this->diskIoLCD->display(val);
            if (this->diskIoLCD->isHidden())
                this->diskIoLCD->show();
        }
        else
        {
            if (!this->diskIoLCD->isHidden())
                this->diskIoLCD->hide();
        }

        painter.end();
        break;
    }

    default: break;

    }
}
