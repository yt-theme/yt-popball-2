#ifndef CONFIG_H
#define CONFIG_H

#include <QSettings>
#include <QString>
#include <QFile>
#include <QDebug>
#include <QDir>
#include "macro_def.h"   // ASIDE_LEFT / ASIDE_RIGHT（贴边竖条记录吸附在哪一侧）

class Config {
private:
    QSettings *settingsObj = nullptr;   // 构造函数保证非空；显式初始化防止 checkOrCreateConfig 失败时 readConfig 解引用野指针
    QString configFilePath = QDir( QDir::homePath()).absoluteFilePath(".popball2_config.ini");
    QString defaultConfigRes = ":/config/default_config.ini";

private:
    // configure items
    qint32 x;
    qint32 y;
    qint32 width;
    qint32 height;
    qint32 aside_width;
    qint32 aside_height;
    double opacity;
    qint32 shadow_radius;      // 阴影长度（模糊半径），0 = 无阴影
    QString shadow_color;      // 阴影颜色
    qint16 shape;

    QString main_color;
    QString main_border_color;
    qint32  main_border_width;
    QString mem_color;
    QString swap_color;
    QString cpu_usage_color;
    double  cpu_usage_width;
    QString cpu_freq_color;
    QString cpu_temp_color;
    QString net_speed_color;
    QString disk_io_color;          // 磁盘读写速度文字颜色

    qint32 charts_rows;
    qint32 update_data_interval;
    qint32 update_ui_interval;

    // [aside] 贴边竖条
    // 把小球拖到屏幕左/右边缘时，吸附成一根圆角的竖条，用窄柱图显示各指标。
    qint32 snap_to_edge;          // 1 = 启用贴边变竖条，0 = 关闭（始终是圆球）
    qint32 aside_edge;            // 当前吸附在哪一侧：ASIDE_LEFT / ASIDE_RIGHT
    qint32 aside_corner_radius;   // 竖条圆角半径（px）
    // 注：竖条的宽/高沿用 [appearance] 里已有的 aside_width / aside_height

    // [components_show]
    qint8 cpu_temp_show;
    qint8 cpu_freq_show;
    qint8 net_speed_show;
    qint8 disk_io_show;            // 1=显示磁盘读写速度, 0=隐藏

    // [disk] 磁盘读写速度统计哪块盘
    qint32  disk_io_mode;          // 0=IO 最高的盘（默认）, 1=指定盘
    QString disk_io_name;          // mode=1 时指定的磁盘设备名

    // [ui] 界面偏好
    // 数据中转站面板的展示布局：0=图标网格（默认）1=列表 2=详细
    qint32  dock_view_style;
    // 悬浮球形态（非贴边竖条时）：0=球形（默认） 1=圆角矩形 2=直角方形 3=长条形
    qint32  ball_style;
    // 数据中转站弹窗：尺寸 / 优先展示位置 / 内容密度
    qint32  dock_width;      // 弹窗宽度（px，默认 330）
    qint32  dock_height;     // 弹窗高度（px，默认 452）
    qint32  dock_position;   // 0=自动（空间大的一侧） 1=悬浮球右侧 2=悬浮球左侧 3=屏幕居中
    qint32  dock_density;    // 0=紧凑 1=标准（默认） 2=宽松
    qint32  dock_opacity;    // 弹窗背景不透明度（千分比 0~1000，默认 871 = 87.1%）
    qint32  dock_remember_scroll; // 弹窗记住上次滚动位置：1=记住 0=不记住（默认，每次打开从顶部）

    // [window]
    // 形状蒙版：桌面没开混成(compositing)时，半透明窗口会露出黑色矩形底，
    // 这时需要用圆形蒙版把窗口裁成圆的。
    //   0 = 自动（X11 下自动检测有没有混成管理器）
    //   1 = 强制开启（自动检测不准时手动打开）
    //   2 = 关闭
    qint32 shape_mask;

    // [system_monitor]
    // 右键菜单「系统监视器」使用的命令；留空则按桌面环境自动检测。
    QString system_monitor_cmd;

public:
    Config();
    ~Config();

    // ########### check config file ###########
    bool checkOrCreateConfig();
    // get arg value
    void readConfig();

    // ########### write to config #############
    void setX(qint32 val);
    void setY(qint32 val);
    void setWidth(qint32 val);
    void setHeight(qint32 val);
    void setAsideWidth(qint32 val);
    void setAsideHeight(qint32 val);
    void setOpacity(double val);
    void setShadowRadius(qint32 val);
    void setShadowColor(QString val);
    void setShape(qint32 val);

    void setMainColor(QString val);
    void setMainBorderColor(QString val);
    void setMainBorderWidth(qint32 val);
    void setMemColor(QString val);
    void setSwapColor(QString val);
    void setCpuUsageColor(QString val);
    void setCpuUsageWidth(double val);
    void setCpuFreqColor(QString val);
    void setCpuTempColor(QString val);
    void setNetSpeedColor(QString val);
    void setDiskIoColor(QString val);

    void setChartsRows(qint32 val);
    void setUpdateDataInterval(qint32 val);
    void setUpdateUIInterval(qint32 val);

    // [aside] 贴边竖条
    void setSnapToEdge(qint32 val);
    void setAsideEdge(qint32 val);
    void setAsideCornerRadius(qint32 val);

    // [components_show]
    void setCpuTempShow(qint8 val);
    void setCpuFreqShow(qint8 val);
    void setNetSpeedShow(qint8 val);
    void setDiskIoShow(qint8 val);
    void setDiskIoMode(qint32 val);
    void setDiskIoName(QString val);

    // [ui]
    void setDockViewStyle(qint32 val);
    void setBallStyle(qint32 val);   // 悬浮球形态 0=球 1=圆角矩形 2=直角方形 3=长条形
    void setDockWidth(qint32 val);
    void setDockHeight(qint32 val);
    void setDockPosition(qint32 val);
    void setDockDensity(qint32 val);
    void setDockOpacity(qint32 val);
    void setRememberScroll(bool on);

    // [window]
    void setShapeMask(qint32 val);

    // [system_monitor]
    void setSystemMonitorCmd(QString val);

    // ########### get config item values ###########
    qint32  getX();
    qint32  getY();
    qint32  getWidth();
    qint32  getHeight();
    qint32  getAsideWidth();
    qint32  getAsideHeight();
    double  getOpacity();
    qint32  getShadowRadius();
    QString getShadowColor();
    qint32  getShape();

    QString getMainColor();
    QString getMainBorderColor();
    qint32  getMainBorderWidth();
    QString getMemColor();
    QString getSwapColor();
    QString getCpuUsageColor();
    double  getCpuUsageWidth();
    QString getCpuFreqColor();
    QString getCpuTempColor();
    QString getNetSpeedColor();
    QString getDiskIoColor();

    qint32  getChartsRows();
    qint32  getUpdateDataInterval();
    qint32  getUpdateUIInterval();

    // [aside] 贴边竖条
    qint32  getSnapToEdge();
    qint32  getAsideEdge();
    qint32  getAsideCornerRadius();

    // [components_show]
    qint8 getCpuTempShow();
    qint8 getCpuFreqShow();
    qint8 getNetSpeedShow();
    qint8 getDiskIoShow();
    qint32  getDiskIoMode();
    QString getDiskIoName();

    // [ui]
    qint32 getDockViewStyle();
    qint32 getBallStyle();
    qint32 getDockWidth();
    qint32 getDockHeight();
    qint32 getDockPosition();
    qint32 getDockDensity();
    qint32 getDockOpacity();
    bool getRememberScroll();

    // [window]
    qint32 getShapeMask();

    // [system_monitor]
    QString getSystemMonitorCmd();
};


// “系统监视器”命令只允许“单条程序 + 参数”，绝不经过 shell 执行。
// 故命令串里出现下面这些“只在 shell 里才有意义”的字符/序列时，
// 一律视为危险或误填，拒绝保存与执行：
//   ; | & < >  `  $(  以及换行/回车
// （QProcess 本来就按 程序+参数 直接 exec，没有 shell；这里再主动拦截，
//   是防御性检查：防止用户误粘贴 shell 一行式，也防配置被篡改后注入。）
inline bool isSafeMonitorCommandLine(const QString &cmdline)
{
    const QString danger = QStringLiteral(";|&<>`\n\r");
    for (const QChar &c : cmdline)
        if (danger.indexOf(c) >= 0)
            return false;
    if (cmdline.contains(QLatin1String("$(")))
        return false;
    return true;
}


#endif // CONFIG_H
