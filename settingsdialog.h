#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QShowEvent>
#include <QPushButton>
#include <QCheckBox>
#include <QSlider>
#include <QLabel>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QColor>
#include <QList>
#include "config.h"

class QVBoxLayout;
class QRadioButton;
class SysInfo;

/*
 * 扁平化风格的设置窗口（可滚动）
 *
 *   预制配色   [经典蓝][活力蓝][霓虹夜]
 *              [薄荷][落日橙][极光紫]
 *   颜色       主球背景/球体边框/内存图/交换分区/CPU占用/阴影/
 *              悬浮球文字/温度文字/频率文字/网速文字/磁盘IO文字（点色块取色）
 *              「悬浮球文字」= 一键同步全部文字颜色
 *   显示       [x]温度 [ ]频率 [x]网速 [x]磁盘读写
 *              磁盘 (•)IO最高的盘 ( )指定磁盘 [下拉]
 *   窗口       不透明度 / 大小(宽x高) / 边框宽度
 *              阴影长度(0=无阴影) / 阴影颜色
 *   贴边竖条   是否启用 / 快捷宽度(窄/标准/宽) / 竖条宽×高 / 圆角半径
 *   图表       CPU 折线宽度 / 图表行数
 *   高级       数据刷新间隔 / 界面刷新间隔 / 形状蒙版
 *
 *   [恢复默认]              [取消] [应用] [确定]
 *
 *   应用 = 生效但不关窗口；确定 = 生效并关窗口；取消 = 不生效直接关
 *   恢复默认 = 二次确认后把默认值填回界面（仍需点应用/确定才生效）
 */
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(Config *cfg, SysInfo *sysInfo = nullptr, QWidget *parent = nullptr);

    // 从当前配置重新载入（显示前调用，保证内容是最新的）
    void loadFromConfig();

signals:
    void settingsApplied();   // 保存成功后发出，让挂件刷新界面

private slots:
    void pickColor();
    void onApply();
    void onOk();
    void onCancel();
    void onRestoreDefault();

private:
    struct ColorRow {
        QString     key;     // 对应 Config 里的配置键名
        QString     label;
        QPushButton *btn = nullptr;
        QColor      color;
    };

    void buildUi();

protected:
    // 把窗口钳进屏幕可用区域（父窗口是屏幕边缘上的悬浮球，默认摆放会跑出屏幕）
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void clampIntoScreen();
    void buildPresetRow(QVBoxLayout *parentLayout);
    QWidget *buildColorSection();
    QWidget *buildShowSection();
    QWidget *buildWindowSection();
    QWidget *buildAsideSection();
    QWidget *buildChartSection();
    QWidget *buildAdvancedSection();
    QWidget *buildMonitorSection();
    QWidget *buildDockSection();

    void loadDefaults();
    void applyPreset(int index);
    void refreshRowStyle(ColorRow &row);
    bool confirmRestore();

    QString configColor(const QString &key) const;
    void setConfigColor(const QString &key, const QString &value);
    void applyChanges();   // 把界面上的值写回 Config 并通知挂件刷新

    Config  *cfg     = nullptr;
    SysInfo *sysInfo = nullptr;   // 用于取磁盘名列表（可能为空）
    QList<ColorRow> rows;
    // 「悬浮球文字」总项是否被用户本次改动过：仅在改动时才四色同设，
    // 避免 applyChanges 循环里被四个分项（预设白等）覆盖。
    bool textColorTouched = false;

    QCheckBox *chkTemp = nullptr;
    QCheckBox *chkFreq = nullptr;
    QCheckBox *chkNet  = nullptr;
    QCheckBox *chkDiskIo = nullptr;

    // 磁盘读写统计哪块盘
    QRadioButton *radDiskAuto    = nullptr;   // IO 最高的盘（默认）
    QRadioButton *radDiskManual  = nullptr;   // 指定盘
    QComboBox    *comboDiskName  = nullptr;   // 磁盘名下拉

    QSlider *opacitySlider = nullptr;
    QLabel  *opacityLabel  = nullptr;
    QComboBox *comboBallStyle = nullptr;   // 悬浮球形态：0球 1圆角矩形 2直角方形 3长条形
    QComboBox *comboLanguage  = nullptr;   // 界面语言：0跟随系统 1简体中文 2English
    int       m_langApplied   = -1;   // 已应用的语言（用于“切换后重启生效”提示）

    QSpinBox *spinWidth        = nullptr;
    QSpinBox *spinHeight       = nullptr;
    QSpinBox *spinBorderWidth  = nullptr;
    QSpinBox *spinShadowLen    = nullptr;
    QDoubleSpinBox *spinCpuLine = nullptr;
    QSpinBox *spinChartsRows   = nullptr;
    QSpinBox *spinDataInterval = nullptr;
    QSpinBox *spinUiInterval   = nullptr;
    QComboBox *comboShapeMask  = nullptr;

    // 贴边竖条
    QCheckBox *chkSnapEdge      = nullptr;
    QSpinBox  *spinAsideWidth   = nullptr;
    QSpinBox  *spinAsideHeight  = nullptr;
    QSpinBox  *spinAsideRadius  = nullptr;

    QLineEdit *editMonitorCmd  = nullptr;   // 右键菜单「系统监视器」自定义命令

    // 数据中转站弹窗
    QSpinBox  *spinDockWidth    = nullptr;  // 弹窗宽度
    QSpinBox  *spinDockHeight   = nullptr;  // 弹窗高度
    QComboBox *comboDockPosition = nullptr; // 优先展示位置：0自动 1右侧 2左侧 3屏幕居中
    QComboBox *comboDockDensity  = nullptr; // 内容密度：0紧凑 1标准 2宽松
    QSlider   *sliderDockOpacity = nullptr; // 弹窗背景不透明度（千分比 0~1000）
    QLabel    *lblDockOpacityVal = nullptr; // 当前百分比显示（如 87.1%）
    QCheckBox *checkRememberScroll = nullptr; // 弹窗记住上次滚动位置（默认不记住）

    QPushButton *btnRestore = nullptr;
    QPushButton *btnCancel  = nullptr;
    QPushButton *btnApply   = nullptr;
    QPushButton *btnOk      = nullptr;
};

#endif // SETTINGSDIALOG_H
