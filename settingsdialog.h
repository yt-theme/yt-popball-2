#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QPushButton>
#include <QCheckBox>
#include <QSlider>
#include <QLabel>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QColor>
#include <QList>
#include "config.h"

class QVBoxLayout;

/*
 * 扁平化风格的设置窗口（可滚动）
 *
 *   预制配色   [经典蓝][活力蓝][霓虹夜]
 *              [薄荷][落日橙][极光紫]
 *   颜色       主球背景/球体边框/内存图/交换分区/CPU占用/
 *              阴影/温度文字/频率文字/网速文字  （点色块取色）
 *   显示       [x]温度 [ ]频率 [x]网速
 *   窗口       不透明度 / 大小(宽x高) / 边框宽度
 *              阴影长度(0=无阴影) / 阴影颜色
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
    explicit SettingsDialog(Config *cfg, QWidget *parent = nullptr);

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
    void buildPresetRow(QVBoxLayout *parentLayout);
    QWidget *buildColorSection();
    QWidget *buildShowSection();
    QWidget *buildWindowSection();
    QWidget *buildChartSection();
    QWidget *buildAdvancedSection();

    void loadDefaults();
    void applyPreset(int index);
    void refreshRowStyle(ColorRow &row);
    bool confirmRestore();

    QString configColor(const QString &key) const;
    void setConfigColor(const QString &key, const QString &value);
    void applyChanges();   // 把界面上的值写回 Config 并通知挂件刷新

    Config *cfg = nullptr;
    QList<ColorRow> rows;

    QCheckBox *chkTemp = nullptr;
    QCheckBox *chkFreq = nullptr;
    QCheckBox *chkNet  = nullptr;

    QSlider *opacitySlider = nullptr;
    QLabel  *opacityLabel  = nullptr;

    QSpinBox *spinWidth        = nullptr;
    QSpinBox *spinHeight       = nullptr;
    QSpinBox *spinBorderWidth  = nullptr;
    QSpinBox *spinShadowLen    = nullptr;
    QDoubleSpinBox *spinCpuLine = nullptr;
    QSpinBox *spinChartsRows   = nullptr;
    QSpinBox *spinDataInterval = nullptr;
    QSpinBox *spinUiInterval   = nullptr;
    QComboBox *comboShapeMask  = nullptr;

    QPushButton *btnRestore = nullptr;
    QPushButton *btnCancel  = nullptr;
    QPushButton *btnApply   = nullptr;
    QPushButton *btnOk      = nullptr;
};

#endif // SETTINGSDIALOG_H
