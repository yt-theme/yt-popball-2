#include "settingsdialog.h"

#include <QColorDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QSettings>
#include <QFrame>
#include <QPainter>
#include <cstdio>
#include <QTimer>
#include <QIcon>
#include <QPixmap>
#include <QMessageBox>
#include <QGuiApplication>
#include <QScreen>
#include <QtGlobal>
#include <QFileDialog>
#include <QStandardPaths>
#include <QRadioButton>
#include "sysInfo.h"

// ---------------------------------------------------------------- 配置项定义
namespace {

struct ColorDef { const char *key; const char *label; };
// 顺序即界面显示顺序（左右两列排布）；applyPreset / loadDefaults 的取值顺序必须与此一致
// 「悬浮球文字」是总项：一键同步温度/频率/网速/磁盘IO四个文字颜色
const ColorDef kColors[] = {
    { "main_color",        "主球背景" },
    { "main_border_color", "球体边框" },
    { "mem_color",         "内存图" },
    { "swap_color",        "交换分区" },
    { "cpu_usage_color",   "CPU 占用" },
    { "shadow_color",      "阴影" },
    { "text_color",        "悬浮球文字" },
    { "cpu_temp_color",    "温度文字" },
    { "cpu_freq_color",    "频率文字" },
    { "net_speed_color",   "网速文字" },
    { "disk_io_color",     "磁盘IO文字" },
};
const int kColorCount = int(sizeof(kColors) / sizeof(kColors[0]));

// 预制配色方案（点一下整套应用并立即生效）
struct PresetDef {
    const char *name;
    const char *main, *border, *mem, *swap, *cpu, *shadow, *text;
};
const PresetDef kPresets[] = {
    { "经典蓝", "#13191C", "#41B0DD", "#24568F", "#8C0E2B47", "#4590C8FF", "#000000", "#FFFFFF" },
    { "活力蓝", "#13191C", "#41B0DD", "#2E6FC4", "#8C2A5E93", "#4FB7DDFF", "#000000", "#FFFFFF" },
    { "霓虹夜", "#0D1117", "#22D3EE", "#2563EB", "#8C7C3AED", "#4522D3EE", "#000000", "#FFFFFF" },
    { "薄荷",   "#0C1B17", "#34D399", "#059669", "#8C0F3D2E", "#596EE7B7", "#000000", "#FFFFFF" },
    { "落日橙", "#1C1208", "#FB923C", "#EA580C", "#8C7C2D12", "#45FDBA74", "#000000", "#FFFFFF" },
    { "极光紫", "#150F1E", "#A78BFA", "#7C3AED", "#8C2E1065", "#59E879F9", "#000000", "#FFFFFF" },
};
const int kPresetCount = int(sizeof(kPresets) / sizeof(kPresets[0]));

// 预制方案的图标：四条竖色带（边框/内存/交换/CPU）让人一眼看出配色
QIcon presetIcon(const PresetDef &p)
{
    QPixmap pm(44, 16);
    pm.fill(Qt::transparent);
    QPainter pt(&pm);
    pt.setPen(Qt::NoPen);
    const QColor cs[4] = { QColor(p.border), QColor(p.mem), QColor(p.swap), QColor(p.cpu) };
    const qreal w = 44.0 / 4;
    for (int i = 0; i < 4; ++i) {
        QColor c = cs[i]; c.setAlpha(255);
        pt.setBrush(c);
        pt.drawRect(QRectF(i * w, 0, w + 0.5, 16));
    }
    pt.end();
    return QIcon(pm);
}

} // namespace

// ---------------------------------------------------------------- 扁平化样式
static const char *kDialogStyle = R"(
QDialog { background-color:#FFFFFF; }
QScrollArea { background-color:#FFFFFF; border:none; }
QWidget#content { background-color:#FFFFFF; }
QLabel  { color:#3A3F45; font-size:13px; background:transparent; }
QLabel[section="title"] { color:#1B1F24; font-size:17px; font-weight:600; }
QLabel[section="group"] { color:#9AA0A6; font-size:11px; font-weight:600; }
QLabel[role="hint"]     { color:#B0B5BB; font-size:11px; }
QPushButton {
    background-color:#F1F3F6; border:1px solid #E2E6EB; border-radius:4px;
    padding:6px 16px; color:#3A3F45; font-size:13px;
}
QPushButton:hover  { background-color:#E7EBF0; }
QPushButton:pressed{ background-color:#DCE1E8; }
QPushButton[role="primary"] { background-color:#2E7DD8; border:none; color:#FFFFFF; font-weight:600; }
QPushButton[role="primary"]:hover  { background-color:#2A72C4; }
QPushButton[role="primary"]:pressed{ background-color:#2667B0; }
QPushButton[role="swatch"] {
    border:1px solid rgba(0,0,0,0.20); border-radius:4px;
    padding:5px 10px; font-size:11px; font-weight:600; text-align:center;
}
QPushButton[role="swatch"]:hover { border:1px solid rgba(0,0,0,0.45); }
QPushButton[role="preset"] {
    background-color:#F7F9FB; border:1px solid #E2E6EB; border-radius:4px;
    padding:5px 8px; color:#3A3F45; font-size:12px; text-align:left;
}
QPushButton[role="preset"]:hover  { border-color:#2E7DD8; background-color:#EFF6FF; }
QPushButton[role="preset"]:pressed{ background-color:#E0EEFC; }
QCheckBox { color:#3A3F45; font-size:13px; spacing:6px; background:transparent; }
QCheckBox::indicator {
    width:16px; height:16px; border:1px solid #C9CFD6;
    border-radius:3px; background-color:#FFFFFF;
}
QCheckBox::indicator:hover  { border-color:#2E7DD8; }
QCheckBox::indicator:checked{ background-color:#2E7DD8; border-color:#2E7DD8; }
QSlider::groove:horizontal  { height:4px; background:#E2E6EB; border-radius:2px; }
QSlider::sub-page:horizontal{ height:4px; background:#2E7DD8; border-radius:2px; }
QSlider::handle:horizontal {
    width:14px; height:14px; margin:-6px 0; border-radius:8px;
    background-color:#FFFFFF; border:2px solid #2E7DD8;
}
QFrame[role="line"] { background-color:#EEF1F4; max-height:1px; border:none; }
QSpinBox, QDoubleSpinBox, QComboBox, QLineEdit {
    background-color:#FFFFFF; border:1px solid #DDE2E8; border-radius:4px;
    padding:4px 8px; color:#3A3F45; font-size:13px;
}
/* 调节按钮：自绘扁平三角，替换 macOS 原生的细小箭头（太挤、与扁平风格不符） */
QSpinBox::up-button, QDoubleSpinBox::up-button {
    subcontrol-origin: border; subcontrol-position: top right;
    width: 18px; border: none; border-left: 1px solid #DDE2E8;
    background: transparent;
}
QSpinBox::down-button, QDoubleSpinBox::down-button {
    subcontrol-origin: border; subcontrol-position: bottom right;
    width: 18px; border: none; border-left: 1px solid #DDE2E8;
    background: transparent;
}
QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
    background-color: #EFF6FF;
}
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
    width: 0; height: 0;
    border-left: 3.5px solid transparent; border-right: 3.5px solid transparent;
    border-bottom: 4.5px solid #6B7280;
}
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
    width: 0; height: 0;
    border-left: 3.5px solid transparent; border-right: 3.5px solid transparent;
    border-top: 4.5px solid #6B7280;
}
QSpinBox:hover, QDoubleSpinBox:hover, QComboBox:hover, QLineEdit:hover { border-color:#B8C0CA; }
QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus, QLineEdit:focus { border-color:#2E7DD8; }
QComboBox QAbstractItemView {
    background-color:#FFFFFF; border:1px solid #DDE2E8;
    selection-background-color:#E0EEFC; selection-color:#1B1F24;
}
)";

// ---------------------------------------------------------------- 构造
SettingsDialog::SettingsDialog(Config *cfg, SysInfo *sysInfo, QWidget *parent)
    : QDialog(parent), cfg(cfg), sysInfo(sysInfo)
{
    setWindowTitle(tr("设置"));
    setModal(false);
    setMinimumWidth(520);
    setStyleSheet(QString::fromLatin1(kDialogStyle));

    buildUi();
    loadFromConfig();
}

// 防止窗口跑出屏幕：父窗口是屏幕边缘上的悬浮球，
// Qt 会按父窗口位置摆放子窗口，结果右半截（含"确定"按钮）被推到屏幕外。
// 在 show 时把自己钳回屏幕可用区域内。
void SettingsDialog::clampIntoScreen()
{
    if (!isVisible())
        return;
    QScreen *scr = screen() != nullptr ? screen() : QGuiApplication::primaryScreen();
    if (scr == nullptr)
        return;
    const QRect av = scr->availableGeometry();
    const QPoint pos = frameGeometry().topLeft();
    // QRect::right() = left + width - 1，所以"贴右缘"的 x 是 av.right()+1-w
    const int maxX = av.right()  + 1 - frameGeometry().width();
    const int maxY = av.bottom() + 1 - frameGeometry().height();
    const QPoint clamped(qBound(av.left(), pos.x(), qMax(av.left(), maxX)),
                         qBound(av.top(),  pos.y(), qMax(av.top(),  maxY)));
    if (clamped != pos)
        move(clamped);
}

void SettingsDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    // 不能在 showEvent 里同步 move()：此刻布局/开窗还没定型，钳完马上又会变。
    // 排队到事件循环下一轮，等首轮布局完成后钳一次。
    if (qEnvironmentVariableIsSet("PB_CLAMP_OFF"))
        return;   // 二分实验：禁用
    QTimer::singleShot(0, this, [this]() { clampIntoScreen(); });
}

void SettingsDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    // 窗口尺寸在显示后仍可能变化（首次布局、DPI 切换），
    // 每次尺寸变化后重新钳一次，保证永远在屏内。
    clampIntoScreen();
}

void SettingsDialog::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // ---- 可滚动的内容区 ----
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("content"));

    auto *root = new QVBoxLayout(content);
    root->setContentsMargins(22, 18, 22, 18);
    root->setSpacing(10);

    auto *line = new QFrame(content);
    line->setProperty("role", "line");
    line->setFrameShape(QFrame::StyledPanel);
    root->addWidget(line);

    root->addWidget(buildColorSection());
    root->addWidget(buildShowSection());
    root->addWidget(buildWindowSection());
    root->addWidget(buildAsideSection());
    root->addWidget(buildChartSection());
    root->addWidget(buildAdvancedSection());
    root->addWidget(buildMonitorSection());
    root->addStretch(1);

    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    // ---- 底部按钮（固定不滚动）----
    auto *btnBox = new QWidget(this);
    // 注意必须用 ID 选择器：写成 QWidget{...} 会匹配所有后代（包括按钮），
    // 且祖先样式表比对话框级样式表优先级更高，会把「确定」按钮的蓝色背景
    // 覆盖成容器色 —— 白底白字，看起来就像按钮不存在一样。
    btnBox->setObjectName(QStringLiteral("btnBar"));
    btnBox->setStyleSheet(QStringLiteral(
        "#btnBar { background-color:#FAFBFC; border-top:1px solid #EEF1F4; }"));
    auto *btnRow = new QHBoxLayout(btnBox);
    btnRow->setContentsMargins(22, 10, 22, 14);
    btnRow->setSpacing(8);

    btnRestore = new QPushButton(tr("恢复默认"), btnBox);
    btnCancel  = new QPushButton(tr("取消"), btnBox);
    btnApply   = new QPushButton(tr("应用"), btnBox);
    btnOk      = new QPushButton(tr("确定"), btnBox);
    btnOk->setProperty("role", "primary");
    btnOk->setDefault(true);

    btnRow->addWidget(btnRestore);
    btnRow->addStretch(1);
    btnRow->addWidget(btnCancel);
    btnRow->addWidget(btnApply);
    btnRow->addWidget(btnOk);
    outer->addWidget(btnBox);

    connect(btnRestore, &QPushButton::clicked, this, &SettingsDialog::onRestoreDefault);
    connect(btnCancel,  &QPushButton::clicked, this, &SettingsDialog::onCancel);
    connect(btnApply,   &QPushButton::clicked, this, &SettingsDialog::onApply);
    connect(btnOk,      &QPushButton::clicked, this, &SettingsDialog::onOk);
}

// ---------------------------------------------------------------- 各分区
QWidget *SettingsDialog::buildColorSection()
{
    auto *box = new QWidget(this);
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);



    buildPresetRow(v);

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(8);
    for (int i = 0; i < kColorCount; ++i) {
        ColorRow row;
        row.key   = QString::fromLatin1(kColors[i].key);
        row.label = QString::fromUtf8(kColors[i].label);
        row.btn   = new QPushButton(box);
        row.btn->setProperty("role", "swatch");
        row.btn->setCursor(Qt::PointingHandCursor);
        row.btn->setToolTip(tr("点击选择颜色"));
        connect(row.btn, &QPushButton::clicked, this, &SettingsDialog::pickColor);

        auto *cell = new QWidget(box);
        auto *h = new QHBoxLayout(cell);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        auto *lab = new QLabel(row.label, cell);
        lab->setMinimumWidth(58);
        h->addWidget(lab);
        h->addWidget(row.btn, 1);
        grid->addWidget(cell, i / 2, i % 2);

        rows.append(row);
    }
    v->addLayout(grid);

    auto *hint = new QLabel(
        tr("* 交换分区 / CPU 占用 / 阴影 这三项带透明度，取色器只改颜色、保留原透明度。"
           "「悬浮球文字」= 一次设置温度 / 频率 / 网速 / 磁盘IO 全部文字颜色"), box);
    hint->setProperty("role", "hint");
    v->addWidget(hint);

    return box;
}

QWidget *SettingsDialog::buildShowSection()
{
    auto *box = new QWidget(this);
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);

    auto *g = new QLabel(tr("显  示"), box);
    g->setProperty("section", "group");
    v->addWidget(g);

    auto *row = new QHBoxLayout();
    row->setSpacing(18);
    chkTemp = new QCheckBox(tr("CPU 温度"), box);
    chkFreq = new QCheckBox(tr("CPU 频率"), box);
    chkNet  = new QCheckBox(tr("网速"), box);
    chkDiskIo = new QCheckBox(tr("磁盘读写"), box);
    row->addWidget(chkTemp);
    row->addWidget(chkFreq);
    row->addWidget(chkNet);
    row->addWidget(chkDiskIo);
    row->addStretch(1);
    v->addLayout(row);

    // ---- 磁盘读写统计哪块盘 ----
    auto *drow = new QHBoxLayout();
    drow->setSpacing(10);
    auto *dlabel = new QLabel(tr("磁盘"), box);
    drow->addWidget(dlabel);

    radDiskAuto   = new QRadioButton(tr("IO 最高的盘"), box);
    radDiskManual = new QRadioButton(tr("指定磁盘"), box);
    comboDiskName = new QComboBox(box);
    radDiskAuto->setObjectName(QStringLiteral("diskAuto"));
    radDiskManual->setObjectName(QStringLiteral("diskManual"));
    comboDiskName->setObjectName(QStringLiteral("diskName"));
    comboDiskName->setMinimumWidth(150);
    comboDiskName->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    drow->addWidget(radDiskAuto);
    drow->addWidget(radDiskManual);
    drow->addWidget(comboDiskName);
    drow->addStretch(1);
    v->addLayout(drow);

    // 填充磁盘列表（显示友好名，userData 存设备名）
    const QStringList diskNames = (this->sysInfo != nullptr)
        ? this->sysInfo->getDiskNames() : QStringList();
    for (const QString &n : diskNames)
        comboDiskName->addItem(this->sysInfo->getDiskLabel(n), n);
    // 若配置里指定的盘当前不在列表（未挂载/未采样到），也补进去，避免回显丢失
    const QString cfgName = this->cfg->getDiskIoName();
    if (!cfgName.isEmpty() && !diskNames.contains(cfgName))
        comboDiskName->addItem(cfgName, cfgName);

    // 只有在"指定磁盘"时才允许改下拉（与其它控件一致：点应用/确定才生效）
    comboDiskName->setEnabled(radDiskManual->isChecked());
    connect(radDiskManual, &QRadioButton::toggled, this, [this](bool on) {
        comboDiskName->setEnabled(on);
    });

    return box;
}

QWidget *SettingsDialog::buildWindowSection()
{
    auto *box = new QWidget(this);
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);

    auto *g = new QLabel(tr("窗  口"), box);
    g->setProperty("section", "group");
    v->addWidget(g);

    // 不透明度
    auto *op = new QHBoxLayout();
    op->setSpacing(10);
    op->addWidget(new QLabel(tr("不透明度"), box));
    opacitySlider = new QSlider(Qt::Horizontal, box);
    opacitySlider->setRange(50, 100);
    opacitySlider->setValue(91);
    opacitySlider->setCursor(Qt::PointingHandCursor);
    opacityLabel = new QLabel(QStringLiteral("91%"), box);
    opacityLabel->setMinimumWidth(38);
    opacityLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    op->addWidget(opacitySlider, 1);
    op->addWidget(opacityLabel);
    v->addLayout(op);
    connect(opacitySlider, &QSlider::valueChanged, this, [this](int v) {
        opacityLabel->setText(QString::number(v) + QStringLiteral("%"));
    });

    // 大小
    auto *sz = new QHBoxLayout();
    sz->setSpacing(10);
    sz->addWidget(new QLabel(tr("大小(宽×高)"), box));
    spinWidth  = new QSpinBox(box);  spinWidth->setObjectName("width");  spinWidth->setRange(40, 600);  spinWidth->setSuffix(tr(" px"));
    spinHeight = new QSpinBox(box);  spinHeight->setObjectName("height"); spinHeight->setRange(40, 600); spinHeight->setSuffix(tr(" px"));
    sz->addWidget(spinWidth, 1);
    sz->addWidget(spinHeight, 1);
    v->addLayout(sz);

    // 边框宽度
    auto *bw = new QHBoxLayout();
    bw->setSpacing(10);
    bw->addWidget(new QLabel(tr("边框宽度"), box));
    spinBorderWidth = new QSpinBox(box); spinBorderWidth->setObjectName("borderWidth");
    spinBorderWidth->setRange(0, 30);
    spinBorderWidth->setSuffix(tr(" px"));
    bw->addWidget(spinBorderWidth, 1);
    v->addLayout(bw);

    // 阴影长度（0 = 无阴影）
    auto *sl = new QHBoxLayout();
    sl->setSpacing(10);
    sl->addWidget(new QLabel(tr("阴影长度"), box));
    spinShadowLen = new QSpinBox(box); spinShadowLen->setObjectName("shadowLen");
    spinShadowLen->setRange(0, 60);
    spinShadowLen->setSuffix(tr(" px"));
    sl->addWidget(spinShadowLen, 1);
    v->addLayout(sl);

    auto *slHint = new QLabel(tr("* 阴影长度设为 0 表示不显示阴影"), box);
    slHint->setProperty("role", "hint");
    v->addWidget(slHint);

    return box;
}

// 贴边竖条：拖到屏幕左右边缘后吸附成的圆角竖条，用窄柱图显示各指标
QWidget *SettingsDialog::buildAsideSection()
{
    auto *box = new QWidget(this);
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);

    auto *g = new QLabel(tr("贴边竖条"), box);
    g->setProperty("section", "group");
    v->addWidget(g);

    chkSnapEdge = new QCheckBox(tr("拖到屏幕左右边缘时吸附成竖条"), box);
    chkSnapEdge->setToolTip(tr("开启后把小球拖到屏幕左/右边缘，会吸附成一根圆角竖条，\n"
                               "条内用窄柱图显示 CPU 占用 / 内存 / 交换分区。\n"
                               "把竖条往屏幕里侧拖开、或单击竖条，即可变回小球。"));
    v->addWidget(chkSnapEdge);

    // 快捷档位：一次把"宽度 + 圆角"调到位，点完立即生效（可对着竖条看效果）
    auto *pre = new QHBoxLayout();
    pre->setSpacing(8);
    auto *preLab = new QLabel(tr("快捷宽度"), box);
    preLab->setMinimumWidth(58);
    pre->addWidget(preLab);
    struct SizePreset { const char *text; int width; int radius; };
    static const SizePreset kSizePresets[] = {
        { "窄 20",   20,  6 },
        { "标准 30", 30,  8 },
        { "宽 36",   36, 10 },
    };
    for (const SizePreset &p : kSizePresets) {
        auto *b = new QPushButton(QString::fromUtf8(p.text), box);
        b->setProperty("role", "preset");
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tr("竖条宽 %1 px、圆角 %2 px，点击立即生效").arg(p.width).arg(p.radius));
        connect(b, &QPushButton::clicked, this, [this, p]() {
            spinAsideWidth->setValue(p.width);
            spinAsideRadius->setValue(p.radius);
            applyChanges();     // 与预制配色一致：点一下直接应用，方便边看边调
        });
        pre->addWidget(b);
    }
    pre->addStretch(1);
    v->addLayout(pre);

    auto *sz = new QHBoxLayout();
    sz->setSpacing(10);
    sz->addWidget(new QLabel(tr("竖条宽×高"), box));
    spinAsideWidth  = new QSpinBox(box); spinAsideWidth->setObjectName("asideWidth");
    spinAsideWidth->setRange(14, 200);   spinAsideWidth->setSingleStep(2);
    spinAsideWidth->setSuffix(tr(" px"));
    spinAsideWidth->setToolTip(tr("贴边竖条的宽度。越窄越不挡视线，但要放得下 3 根窄柱，\n"
                                  "建议 20 ~ 40 px"));
    spinAsideHeight = new QSpinBox(box); spinAsideHeight->setObjectName("asideHeight");
    spinAsideHeight->setRange(40, 800);  spinAsideHeight->setSingleStep(10);
    spinAsideHeight->setSuffix(tr(" px"));
    spinAsideHeight->setToolTip(tr("贴边竖条的高度"));
    sz->addWidget(spinAsideWidth, 1);
    sz->addWidget(spinAsideHeight, 1);
    v->addLayout(sz);

    auto *rd = new QHBoxLayout();
    rd->setSpacing(10);
    rd->addWidget(new QLabel(tr("圆角半径"), box));
    spinAsideRadius = new QSpinBox(box); spinAsideRadius->setObjectName("asideRadius");
    spinAsideRadius->setRange(0, 40);
    spinAsideRadius->setSuffix(tr(" px"));
    spinAsideRadius->setToolTip(tr("贴边竖条的圆角大小（0 = 直角）"));
    rd->addWidget(spinAsideRadius, 1);
    v->addLayout(rd);

    auto *hint = new QLabel(tr("* 改完立即生效；圆角超过竖条宽度的一半时按一半显示"), box);
    hint->setProperty("role", "hint");
    v->addWidget(hint);

    auto *hint2 = new QLabel(tr("* 拖动竖条离开边缘（或单击竖条）即可变回小球"), box);
    hint2->setProperty("role", "hint");
    v->addWidget(hint2);

    return box;
}

QWidget *SettingsDialog::buildChartSection()
{
    auto *box = new QWidget(this);
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);

    auto *g = new QLabel(tr("图  表"), box);
    g->setProperty("section", "group");
    v->addWidget(g);

    auto *l1 = new QHBoxLayout();
    l1->setSpacing(10);
    l1->addWidget(new QLabel(tr("CPU 折线宽度"), box));
    spinCpuLine = new QDoubleSpinBox(box); spinCpuLine->setObjectName("cpuLine");
    spinCpuLine->setRange(0.5, 8.0);
    spinCpuLine->setSingleStep(0.1);
    spinCpuLine->setDecimals(1);
    l1->addWidget(spinCpuLine, 1);
    v->addLayout(l1);

    auto *l2 = new QHBoxLayout();
    l2->setSpacing(10);
    l2->addWidget(new QLabel(tr("图表行数"), box));
    spinChartsRows = new QSpinBox(box); spinChartsRows->setObjectName("chartsRows");
    spinChartsRows->setRange(4, 128);
    l2->addWidget(spinChartsRows, 1);
    v->addLayout(l2);

    auto *hint = new QLabel(tr("* 图表行数 = 曲线的采样点数，越大曲线越长越平滑"), box);
    hint->setProperty("role", "hint");
    v->addWidget(hint);

    return box;
}

QWidget *SettingsDialog::buildAdvancedSection()
{
    auto *box = new QWidget(this);
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);

    auto *g = new QLabel(tr("高  级"), box);
    g->setProperty("section", "group");
    v->addWidget(g);

    auto *l1 = new QHBoxLayout();
    l1->setSpacing(10);
    l1->addWidget(new QLabel(tr("数据刷新间隔"), box));
    spinDataInterval = new QSpinBox(box); spinDataInterval->setObjectName("dataInterval");
    spinDataInterval->setRange(100, 5000);
    spinDataInterval->setSuffix(tr(" ms"));
    l1->addWidget(spinDataInterval, 1);
    v->addLayout(l1);

    auto *l2 = new QHBoxLayout();
    l2->setSpacing(10);
    l2->addWidget(new QLabel(tr("界面刷新间隔"), box));
    spinUiInterval = new QSpinBox(box); spinUiInterval->setObjectName("uiInterval");
    spinUiInterval->setRange(100, 5000);
    spinUiInterval->setSuffix(tr(" ms"));
    l2->addWidget(spinUiInterval, 1);
    v->addLayout(l2);

    auto *l3 = new QHBoxLayout();
    l3->setSpacing(10);
    l3->addWidget(new QLabel(tr("形状蒙版"), box));
    comboShapeMask = new QComboBox(box); comboShapeMask->setObjectName("shapeMask");
    comboShapeMask->addItem(tr("自动（检测桌面混成）"));
    comboShapeMask->addItem(tr("强制开启"));
    comboShapeMask->addItem(tr("关闭"));
    comboShapeMask->setToolTip(tr("桌面没开混成时小球周围会出现矩形黑框，开启形状蒙版可解决"));
    l3->addWidget(comboShapeMask, 1);
    v->addLayout(l3);

    return box;
}

QWidget *SettingsDialog::buildMonitorSection()
{
    auto *box = new QWidget(this);
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);

    auto *g = new QLabel(tr("系统监视器"), box);
    g->setProperty("section", "group");
    v->addWidget(g);

    auto *row = new QHBoxLayout();
    row->setSpacing(8);

    editMonitorCmd = new QLineEdit(box);
    editMonitorCmd->setObjectName(QStringLiteral("monitorCmd"));
    editMonitorCmd->setPlaceholderText(tr("留空 = 自动按桌面环境检测"));
    editMonitorCmd->setClearButtonEnabled(true);
    editMonitorCmd->setToolTip(
        tr("右键菜单「系统监视器」调用的命令。\n"
           "只支持单条“程序 + 参数”，不支持 shell 运算符（; | & > < ` 等）。\n"
           "可填绝对路径命令，也支持带参数，例如：\n"
           "  C:\\Windows\\System32\\Taskmgr.exe\n"
           "  /usr/bin/gnome-system-monitor\n"
           "  open -a \"Activity Monitor\""));
    auto *btnBrowse = new QPushButton(tr("选择程序"), box);
    btnBrowse->setCursor(Qt::PointingHandCursor);
    btnBrowse->setToolTip(tr("在文件系统里挑一个程序，自动填入命令框"));

    row->addWidget(editMonitorCmd, 1);
    row->addWidget(btnBrowse);
    v->addLayout(row);

    auto *hint = new QLabel(tr("* 留空时按系统自动挑选：macOS→活动监视器，Windows→任务管理器，"
                               "Linux→按 GNOME/KDE/XFCE 等桌面自动匹配"), box);
    hint->setProperty("role", "hint");
    hint->setWordWrap(true);
    v->addWidget(hint);

    connect(btnBrowse, &QPushButton::clicked, this, [this]() {
        const QString startDir =
            QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
        const QString selected = QFileDialog::getOpenFileName(
            this,
            tr("选择系统监视器程序"),
            startDir.isEmpty() ? QString() : startDir,
            tr("程序可执行文件 (*);;所有文件 (*)"));
        if (selected.isEmpty())
            return;
        editMonitorCmd->setText(QDir::toNativeSeparators(selected));
    });

    return box;
}

// ---------------------------------------------------------------- 预制配色
void SettingsDialog::buildPresetRow(QVBoxLayout *parentLayout)
{
    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(6);
    for (int i = 0; i < kPresetCount; ++i) {
        QPushButton *b = new QPushButton(QString::fromUtf8(kPresets[i].name), this);
        b->setProperty("role", "preset");
        b->setIcon(presetIcon(kPresets[i]));
        b->setIconSize(QSize(30, 12));
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tr("应用这套配色并立即生效"));
        connect(b, &QPushButton::clicked, this, [this, i]() { applyPreset(i); });
        grid->addWidget(b, i / 3, i % 3);
    }
    parentLayout->addLayout(grid);
    parentLayout->addSpacing(4);
}

void SettingsDialog::applyPreset(int index)
{
    if (index < 0 || index >= kPresetCount)
        return;
    const PresetDef &p = kPresets[index];
    // 顺序与 kColors 一致（含阴影）；「悬浮球文字」与文字四色统一取 p.text
    const char *values[kColorCount] = {
        p.main, p.border, p.mem, p.swap, p.cpu, p.shadow,
        p.text, p.text, p.text, p.text, p.text
    };
    if (rows.size() != kColorCount)
        return;
    textColorTouched = false;   // 预设：总项与四个分项同色，无需总项联动
    for (int i = 0; i < kColorCount; ++i) {
        rows[i].color = QColor(values[i]);
        refreshRowStyle(rows[i]);
    }
    applyChanges();   // 立即写入并让挂件刷新
}

// ---------------------------------------------------------------- 配置读写
QString SettingsDialog::configColor(const QString &key) const
{
    if (key == QLatin1String("main_color"))        return cfg->getMainColor();
    if (key == QLatin1String("main_border_color")) return cfg->getMainBorderColor();
    if (key == QLatin1String("mem_color"))         return cfg->getMemColor();
    if (key == QLatin1String("swap_color"))        return cfg->getSwapColor();
    if (key == QLatin1String("cpu_usage_color"))   return cfg->getCpuUsageColor();
    if (key == QLatin1String("shadow_color"))      return cfg->getShadowColor();
    if (key == QLatin1String("text_color"))        return cfg->getCpuTempColor();  // 总项：以温度文字为代表
    if (key == QLatin1String("cpu_temp_color"))    return cfg->getCpuTempColor();
    if (key == QLatin1String("cpu_freq_color"))    return cfg->getCpuFreqColor();
    if (key == QLatin1String("net_speed_color"))   return cfg->getNetSpeedColor();
    if (key == QLatin1String("disk_io_color"))     return cfg->getDiskIoColor();
    return QStringLiteral("#FF000000");
}

void SettingsDialog::setConfigColor(const QString &key, const QString &value)
{
    if      (key == QLatin1String("main_color"))        cfg->setMainColor(value);
    else if (key == QLatin1String("main_border_color")) cfg->setMainBorderColor(value);
    else if (key == QLatin1String("mem_color"))         cfg->setMemColor(value);
    else if (key == QLatin1String("swap_color"))        cfg->setSwapColor(value);
    else if (key == QLatin1String("cpu_usage_color"))   cfg->setCpuUsageColor(value);
    else if (key == QLatin1String("shadow_color"))      cfg->setShadowColor(value);
    // 「悬浮球文字」总项：一次同步四个文字色（温度/频率/网速/磁盘IO）
    else if (key == QLatin1String("text_color")) {
        cfg->setCpuTempColor(value);
        cfg->setCpuFreqColor(value);
        cfg->setNetSpeedColor(value);
        cfg->setDiskIoColor(value);
    }
    else if (key == QLatin1String("cpu_temp_color"))    cfg->setCpuTempColor(value);
    else if (key == QLatin1String("cpu_freq_color"))    cfg->setCpuFreqColor(value);
    else if (key == QLatin1String("net_speed_color"))   cfg->setNetSpeedColor(value);
    else if (key == QLatin1String("disk_io_color"))     cfg->setDiskIoColor(value);
}

// ---------------------------------------------------------------- 载入
void SettingsDialog::loadFromConfig()
{
    textColorTouched = false;   // 重新载入后总项不自动联动，等用户改动
    for (int i = 0; i < rows.size(); ++i) {
        rows[i].color = QColor(configColor(rows[i].key));
        if (!rows[i].color.isValid())
            rows[i].color = QColor(Qt::white);
        refreshRowStyle(rows[i]);
    }
    chkTemp->setChecked(cfg->getCpuTempShow() == 1);
    chkFreq->setChecked(cfg->getCpuFreqShow() == 1);
    chkNet->setChecked(cfg->getNetSpeedShow() == 1);
    chkDiskIo->setChecked(cfg->getDiskIoShow() == 1);

    // 磁盘选择：刷新列表并回显（每次打开设置都重新取一次磁盘名）
    {
        const QString curName = cfg->getDiskIoName();
        comboDiskName->blockSignals(true);
        comboDiskName->clear();
        const QStringList names = (this->sysInfo != nullptr)
            ? this->sysInfo->getDiskNames() : QStringList();
        for (const QString &n : names)
            comboDiskName->addItem(this->sysInfo->getDiskLabel(n), n);
        if (!curName.isEmpty() && !names.contains(curName))
            comboDiskName->addItem(curName, curName);
        const int idx = comboDiskName->findData(curName);
        if (idx >= 0) comboDiskName->setCurrentIndex(idx);
        comboDiskName->blockSignals(false);

        const bool manual = (cfg->getDiskIoMode() == 1);
        radDiskManual->setChecked(manual);
        radDiskAuto->setChecked(!manual);
        comboDiskName->setEnabled(manual);
    }

    opacitySlider->setValue(int(qBound(0.5, cfg->getOpacity(), 1.0) * 100));
    spinWidth->setValue(qBound(spinWidth->minimum(),  cfg->getWidth(),  spinWidth->maximum()));
    spinHeight->setValue(qBound(spinHeight->minimum(), cfg->getHeight(), spinHeight->maximum()));
    spinBorderWidth->setValue(qBound(0, cfg->getMainBorderWidth(), spinBorderWidth->maximum()));
    spinShadowLen->setValue(qBound(0, cfg->getShadowRadius(), spinShadowLen->maximum()));
    spinCpuLine->setValue(cfg->getCpuUsageWidth());
    spinChartsRows->setValue(qBound(4, cfg->getChartsRows(), spinChartsRows->maximum()));
    spinDataInterval->setValue(qBound(100, cfg->getUpdateDataInterval(), spinDataInterval->maximum()));
    spinUiInterval->setValue(qBound(100, cfg->getUpdateUIInterval(), spinUiInterval->maximum()));
    const int mask = cfg->getShapeMask();
    comboShapeMask->setCurrentIndex((mask >= 0 && mask <= 2) ? mask : 0);

    // 贴边竖条
    chkSnapEdge->setChecked(cfg->getSnapToEdge() != 0);
    spinAsideWidth->setValue(qBound(spinAsideWidth->minimum(),  cfg->getAsideWidth(),  spinAsideWidth->maximum()));
    spinAsideHeight->setValue(qBound(spinAsideHeight->minimum(), cfg->getAsideHeight(), spinAsideHeight->maximum()));
    spinAsideRadius->setValue(qBound(0, cfg->getAsideCornerRadius(), spinAsideRadius->maximum()));

    editMonitorCmd->setText(cfg->getSystemMonitorCmd());
}

// 从内置默认值把【所有】设置项填回界面。
// 用硬编码的单一默认值来源（与 default_config.ini 完全一致），
// 不依赖 qresource 读取：否则一旦颜色段读取失败，颜色不会被重置、
// 而其它项被重置，造成“只恢复了一部分”的不一致表现。
void SettingsDialog::loadDefaults()
{
    Q_ASSERT(rows.size() == kColorCount);
    textColorTouched = false;   // 恢复默认：总项与分项一致，无需联动
    // 颜色（顺序必须与 kColors 一致）
    static const struct { const char *key; const char *val; } kColorDefs[kColorCount] = {
        { "main_color",        "#13191C" },
        { "main_border_color", "#41B0DD" },
        { "mem_color",         "#2E6FC4" },
        { "swap_color",        "#8C2A5E93" },
        { "cpu_usage_color",   "#4FB7DDFF" },
        { "shadow_color",      "#000000" },
        { "text_color",        "#fff" },
        { "cpu_temp_color",    "#fff" },
        { "cpu_freq_color",    "#fff" },
        { "net_speed_color",   "#fff" },
        { "disk_io_color",     "#fff" },
    };
    for (int i = 0; i < kColorCount; ++i) {
        rows[i].color = QColor(QString::fromLatin1(kColorDefs[i].val));
        if (!rows[i].color.isValid())
            rows[i].color = QColor(Qt::white);
        refreshRowStyle(rows[i]);
    }

    // 显示
    chkTemp->setChecked(true);
    chkFreq->setChecked(false);
    chkNet->setChecked(true);
    chkDiskIo->setChecked(false);   // 磁盘读写默认不展示，需要的人自己勾
    // 磁盘选择：默认 = IO 最高的盘
    radDiskAuto->setChecked(true);
    radDiskManual->setChecked(false);
    comboDiskName->setEnabled(false);

    // 窗口
    opacitySlider->setValue(91);          // 0.91
    spinWidth->setValue(100);
    spinHeight->setValue(100);
    spinBorderWidth->setValue(2);
    spinShadowLen->setValue(0);           // shadow_radius

    // 贴边竖条
    chkSnapEdge->setChecked(true);        // snap_to_edge
    spinAsideWidth->setValue(30);         // aside_width
    spinAsideHeight->setValue(100);       // aside_height
    spinAsideRadius->setValue(8);         // aside/corner_radius

    // 图表
    spinCpuLine->setValue(1.1);           // cpu_usage_width
    spinChartsRows->setValue(32);

    // 高级
    spinDataInterval->setValue(450);
    spinUiInterval->setValue(450);
    comboShapeMask->setCurrentIndex(0);   // 形状蒙版：自动

    // 系统监视器
    editMonitorCmd->clear();
}

// ---------------------------------------------------------------- 色块样式
// 文字颜色按背景亮度自动选黑/白，保证 hex 值可读
void SettingsDialog::refreshRowStyle(ColorRow &row)
{
    const QColor c = row.color;
    const QColor opaque(c.red(), c.green(), c.blue());
    const double lum = 0.2126 * opaque.red() + 0.7152 * opaque.green() + 0.0722 * opaque.blue();
    const char *fg = (lum < 140) ? "#FFFFFF" : "#1B1F24";
    row.btn->setStyleSheet(QStringLiteral(
        "QPushButton{background-color:%1;border:1px solid rgba(0,0,0,0.20);"
        "border-radius:4px;padding:5px 10px;font-size:11px;font-weight:600;color:%2;}")
        .arg(opaque.name(QColor::HexRgb), QString::fromLatin1(fg)));
    row.btn->setText(opaque.name(QColor::HexRgb).toUpper()
                     + (c.alpha() < 255
                        ? QStringLiteral(" %1%").arg(qRound(c.alpha() / 2.55))
                        : QString()));
    row.btn->update();
}

// ---------------------------------------------------------------- 取色
void SettingsDialog::pickColor()
{
    QPushButton *btn = qobject_cast<QPushButton *>(sender());
    if (btn == nullptr) return;

    for (int i = 0; i < rows.size(); ++i) {
        if (rows[i].btn != btn)
            continue;
        QColor rgbOnly = rows[i].color;
        rgbOnly.setAlpha(255);
        // QColorDialog 不支持透明度：只改 RGB，alpha 保持原值
        const QColor picked = QColorDialog::getColor(rgbOnly, this, tr("选择颜色 - %1").arg(rows[i].label));
        if (!picked.isValid())
            return;
        QColor c = picked;
        c.setAlpha(rows[i].color.alpha());
        rows[i].color = c;
        refreshRowStyle(rows[i]);
        // 总项联动标记：改「悬浮球文字」→ 四色同设；改任一分项 → 放弃总项同步
        if (rows[i].key == QLatin1String("text_color")) {
            textColorTouched = true;
        } else if (rows[i].key == QLatin1String("cpu_temp_color")
                   || rows[i].key == QLatin1String("cpu_freq_color")
                   || rows[i].key == QLatin1String("net_speed_color")
                   || rows[i].key == QLatin1String("disk_io_color")) {
            textColorTouched = false;
        }
        return;
    }
}

// ---------------------------------------------------------------- 应用/确定/取消
void SettingsDialog::applyChanges()
{
    // 「悬浮球文字」总项在循环中跳过：它在四个分项之前，
    // 直接按顺序写会被分项（预设白等）覆盖；是否联动由 textColorTouched 决定
    QString textColorValue;
    for (int i = 0; i < rows.size(); ++i) {
        if (rows[i].key == QLatin1String("text_color")) {
            textColorValue = rows[i].color.name(QColor::HexArgb);
            continue;
        }
        setConfigColor(rows[i].key, rows[i].color.name(QColor::HexArgb));
    }
    if (textColorTouched && !textColorValue.isEmpty())
        setConfigColor(QLatin1String("text_color"), textColorValue);

    cfg->setCpuTempShow(chkTemp->isChecked() ? 1 : 0);
    cfg->setCpuFreqShow(chkFreq->isChecked() ? 1 : 0);
    cfg->setNetSpeedShow(chkNet->isChecked() ? 1 : 0);
    cfg->setDiskIoShow(chkDiskIo->isChecked() ? 1 : 0);
    // 磁盘选择：0 = IO 最高的盘（默认）, 1 = 指定盘
    if (radDiskManual->isChecked()) {
        cfg->setDiskIoMode(1);
        cfg->setDiskIoName(comboDiskName->currentData().toString());
    } else {
        cfg->setDiskIoMode(0);
    }
    cfg->setOpacity(opacitySlider->value() / 100.0);
    cfg->setWidth(spinWidth->value());
    cfg->setHeight(spinHeight->value());
    cfg->setMainBorderWidth(spinBorderWidth->value());
    cfg->setShadowRadius(spinShadowLen->value());
    cfg->setShadowColor(rows.at(5).color.name(QColor::HexArgb));   // kColors[5] = shadow_color
    cfg->setCpuUsageWidth(spinCpuLine->value());
    cfg->setChartsRows(spinChartsRows->value());
    cfg->setUpdateDataInterval(spinDataInterval->value());
    cfg->setUpdateUIInterval(spinUiInterval->value());
    cfg->setShapeMask(comboShapeMask->currentIndex());

    // 贴边竖条（aside_edge 由挂件自己记，这里不动）
    cfg->setSnapToEdge(chkSnapEdge->isChecked() ? 1 : 0);
    cfg->setAsideWidth(spinAsideWidth->value());
    cfg->setAsideHeight(spinAsideHeight->value());
    cfg->setAsideCornerRadius(spinAsideRadius->value());

    // 系统监视器命令：只允许“单条程序+参数”。含 shell 运算符的危险串不落盘，
    // 并提示用户（其余设置照常保存）。
    const QString monitorCmd = editMonitorCmd->text();
    if (isSafeMonitorCommandLine(monitorCmd)) {
        cfg->setSystemMonitorCmd(monitorCmd);
    } else {
        QMessageBox::warning(this, tr("系统监视器命令不安全"),
            tr("「系统监视器」命令包含不允许的字符（; | & < > ` $() 或换行）。\n"
               "该项只支持单条“程序 + 参数”，不支持 shell 运算符，\n"
               "为避免执行危险指令，本次未保存该项，其余设置已生效。"));
    }

    emit settingsApplied();
}

void SettingsDialog::onApply()
{
    applyChanges();      // 生效但不关窗口
}

void SettingsDialog::onOk()
{
    applyChanges();      // 生效并关窗口
    accept();
}

void SettingsDialog::onCancel()
{
    reject();            // 不生效直接关
}

// 恢复默认需要二次确认
bool SettingsDialog::confirmRestore()
{
    const QMessageBox::StandardButton r = QMessageBox::question(
        this,
        tr("恢复默认"),
        tr("确定要把所有设置恢复为默认值吗？\n\n"
           "这只会把默认值填回本窗口，还需点「应用」或「确定」才会真正生效。"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    return r == QMessageBox::Yes;
}

void SettingsDialog::onRestoreDefault()
{
    if (!confirmRestore())
        return;
    loadDefaults();
}
