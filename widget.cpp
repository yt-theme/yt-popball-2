#include "widget.h"

#include <QBitmap>
#include <QImage>
#include <QRegion>
#include <QMessageBox>
#include <QCoreApplication>
#include <QDir>

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
    double v = bytesPerSec / 1024.0;                 // KB/s
    const char *unit = "K";
    if (v >= 1024.0) { v /= 1024.0; unit = "M"; }
    if (v >= 1024.0) { v /= 1024.0; unit = "G"; }    // 再往上就到 1024G/s 了，不考虑

    QString s;
    if (v < 10.0)       s = QString::number(v, 'f', 2);   // 1.23
    else if (v < 100.0) s = QString::number(v, 'f', 1);   // 12.3
    else {
        // 3 位整数无小数。四舍五入可能把 999.5 进成 "1000"（4 位），
        // 字段就超宽了 —— 钳到 999，下一帧数值继续增大会自然升单位。
        s = QString::number(qRound(v), 'f', 0);
        if (s.size() > 3) s = QStringLiteral("999");
    }

    while (s.size() < 4) s.prepend(QLatin1Char('0'));     // 0.12 之类保持等宽
    return s + QLatin1String(unit);
}

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
    this->netUploadLCD = new QLabel(this);
    this->netUploadLCD->setAlignment(Qt::AlignCenter);
    this->netUploadLCD->setGeometry(0, config->getHeight()/5.7 + config->getWidth()/5.5 * 1.9, config->getWidth(), config->getWidth()/6.5);
    this->netUploadLCD->setText("u --");
    // net downlod LCD
    this->netDownloadLCD = new QLabel(this);
    this->netDownloadLCD->setAlignment(Qt::AlignCenter);
    this->netDownloadLCD->setGeometry(0, config->getHeight()/5.7 + config->getWidth()/5.5*2.8, config->getWidth(), config->getWidth()/6.5);
    this->netDownloadLCD->setText("d --");

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
    // QLabel 的字号不随控件尺寸自动缩放（QLCDNumber 会），改大小后必须重设
    QFont netFont = this->netUploadLCD->font();
    netFont.setPixelSize(qMax(6, int(w/6.5 * 0.72)));
    netFont.setBold(true);
    this->netUploadLCD->setFont(netFont);
    this->netDownloadLCD->setFont(netFont);
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
    Qt::WindowFlags flags = Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool;
#if defined(Q_OS_MACOS)
    // macOS：不接收键盘焦点，点小球不会把当前正在用的应用切走
    flags |= Qt::WindowDoesNotAcceptFocus;
#endif
    this->setWindowFlags(flags);

    // 半透明背景：必须在窗口真正创建之前设置
    this->setAttribute(Qt::WA_TranslucentBackground);
    this->setFixedSize(config->getWidth(), config->getHeight());
    this->setWindowOpacity(config->getOpacity());

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
    bool needMask = false;
    switch (config->getShapeMask())
    {
    case 1:   needMask = true;  break;      // 强制开启（自动判断不准时手动打开）
    case 2:   needMask = false; break;      // 强制关闭
    default:                                // 自动
#if defined(POPBALL_HAVE_X11)
        needMask = x11CompositingMissing();
#endif
        break;
    }
    Q_UNUSED(isX11);
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

    // reset ui frame
    this->setUiFrame();

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
    this->actQuit     = this->contextMenu->addAction(tr("退出"));

    connect(this->actSettings, &QAction::triggered, this, &Widget::onMenuSettings);
    connect(this->actQuit,     &QAction::triggered, this, &Widget::onMenuQuit);
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

            // 网速自适应显示：单位随大小升（K→M→G），恒定 3 位有效数字，
            // 字段宽度固定，LCD 不会因网速变大而溢出或跳动
            const double interval = this->config->getUpdateDataInterval() > 0
                                        ? this->config->getUpdateDataInterval() : 1.0;
            this->netUploadLCD->setText(
                QString("u %1").arg(formatNetSpeedField(this->sysInfo->getTransmit() / interval)));
            this->netDownloadLCD->setText(
                QString("d %1").arg(formatNetSpeedField(this->sysInfo->getReceive() / interval)));

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
