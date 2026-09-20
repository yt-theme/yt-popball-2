#include "config.h"

Config::Config()
{
    // 允许用环境变量指定配置文件：多开实例、自动化测试时可以不碰用户的配置。
    // 例：POPBALL2_CONFIG=/tmp/test.ini ./popball2
    const QString envPath = qEnvironmentVariable("POPBALL2_CONFIG");
    if (!envPath.isEmpty())
        this->configFilePath = envPath;

    // check
    const bool checkOrCreateRet = this->checkOrCreateConfig();
    // 无论配置文件是否创建成功，都创建 QSettings 对象（QSettings 打开不存在的
    // 文件也安全）：checkOrCreateConfig 失败时 settingsObj 仍有效，readConfig()
    // 不会解引用野指针；读不到的值落到默认参数。
    this->settingsObj = new QSettings(this->configFilePath, QSettings::IniFormat);
    if (!checkOrCreateRet)
        qWarning() << "Config: 配置文件创建/写入失败，将使用默认值:" << this->configFilePath;

    // read config
    this->readConfig();


}

Config::~Config()
{
    delete this->settingsObj;
}

// check config file isExists and create
bool Config::checkOrCreateConfig()
{
    QFile cfgFile(this->configFilePath);
    if (cfgFile.exists() == false)
    {
        // 只打开一次（WriteOnly 本身就负责创建文件），并检查打开结果：
        // 目录不存在 / 只读 / 磁盘满时 open 会失败，返回 false 由上层兜底。
        if (!cfgFile.open(QIODevice::WriteOnly))
        {
            qWarning() << "CheckOrCreateConfig: cannot create" << this->configFilePath;
            return false;
        }
        QFile defaultConfFile(this->defaultConfigRes);
        bool copied = false;
        if (defaultConfFile.open(QIODevice::ReadOnly))
        {
            const qint64 ret = cfgFile.write(defaultConfFile.readAll());
            copied = (ret > 0);
        }
        cfgFile.close();
        if (!copied)
        {
            qDebug() << "CheckOrCreateConfig: copy default configure file err" << "\n";
            return false;
        }
    }
    return true;
}

// read configure file content
void Config::readConfig()
{
    // 防御：构造函数已保证 settingsObj 非空；此分支仅防未来改动破坏该保证
    if (this->settingsObj == nullptr)
        return;

    this->x                     = this->settingsObj->value("/position/x", 300).toInt();
    this->y                     = this->settingsObj->value("/position/y", 300).toInt();
    this->width                 = this->settingsObj->value("/appearance/width", 100).toInt();
    this->height                = this->settingsObj->value("/appearance/height", 100).toInt();
    this->aside_width           = this->settingsObj->value("/appearance/aside_width", 30).toInt();
    this->aside_height          = this->settingsObj->value("/appearance/aside_height", 100).toInt();
    this->opacity               = this->settingsObj->value("/appearance/opacity", 0.91).toDouble();
    this->shadow_radius         = this->settingsObj->value("/appearance/shadow_radius", 0).toInt();
    // 新增键：老配置文件里可能没有，必须给默认值，否则读出来是空/0
    this->shadow_color          = this->settingsObj->value("/appearance/shadow_color", "#000000").toString();
    this->shape                 = this->settingsObj->value("/appearance/shape", 0).toInt();
    this->main_color            = this->settingsObj->value("/appearance/main_color", "#13191C").toString();
    this->main_border_color     = this->settingsObj->value("/appearance/main_border_color", "#41B0DD").toString();
    this->main_border_width     = this->settingsObj->value("/appearance/main_border_width", 2).toInt();
    this->mem_color             = this->settingsObj->value("/appearance/mem_color", "#2E6FC4").toString();
    this->swap_color            = this->settingsObj->value("/appearance/swap_color", "#8C2A5E93").toString();
    this->cpu_usage_color       = this->settingsObj->value("/appearance/cpu_usage_color", "#4FB7DDFF").toString();
    this->cpu_usage_width       = this->settingsObj->value("/appearance/cpu_usage_width", 1.1).toDouble();
    this->cpu_freq_color        = this->settingsObj->value("/appearance/cpu_freq_color", "#fff").toString();
    this->cpu_temp_color        = this->settingsObj->value("/appearance/cpu_temp_color", "#fff").toString();
    this->net_speed_color       = this->settingsObj->value("/appearance/net_speed_color", "#fff").toString();
    this->disk_io_color         = this->settingsObj->value("/appearance/disk_io_color", "#fff").toString();
    this->charts_rows           = this->settingsObj->value("/appearance/charts_rows", 32).toInt();
    this->update_data_interval  = this->settingsObj->value("/timer/update_data_interval", 450).toInt();
    this->update_ui_interval    = this->settingsObj->value("/timer/update_ui_interval", 450).toInt();

    // [aside] 贴边竖条
    // 这几个键是后加的，老配置文件里没有，必须给默认值（0 = 关闭会把功能关掉）
    this->snap_to_edge          = this->settingsObj->value("/aside/snap_to_edge", 1).toInt();
    this->aside_edge            = this->settingsObj->value("/aside/aside_edge", ASIDE_RIGHT).toInt();
    this->aside_corner_radius   = this->settingsObj->value("/aside/corner_radius", 8).toInt();

    // [components_show]
    this->cpu_temp_show         = this->settingsObj->value("/components_show/cpu_temp_show", 1).toInt();
    this->cpu_freq_show         = this->settingsObj->value("/components_show/cpu_freq_show", 0).toInt();
    this->net_speed_show        = this->settingsObj->value("/components_show/net_speed_show", 1).toInt();
    // 磁盘读写默认不展示（用户可在设置里勾选「磁盘读写」打开）
    this->disk_io_show          = this->settingsObj->value("/components_show/disk_io_show", 0).toInt();

    // [disk]
    this->disk_io_mode          = this->settingsObj->value("/disk/disk_io_mode", 0).toInt();
    this->disk_io_name          = this->settingsObj->value("/disk/disk_io_name", "").toString();

    // [ui] 数据中转站面板的展示布局（0=图标 1=列表 2=详细）
    this->dock_view_style       = this->settingsObj->value("/ui/dock_view_style", 0).toInt();
    // 悬浮球形态：0=球形（默认）1=圆角矩形 2=直角方形 3=长条形
    this->ball_style            = this->settingsObj->value("/ui/ball_style", 0).toInt();
    // 弹窗：尺寸 / 位置 / 密度（后加的键，老配置没有必须给默认值）
    this->dock_width            = this->settingsObj->value("/ui/dock_width", 330).toInt();
    this->dock_height           = this->settingsObj->value("/ui/dock_height", 452).toInt();
    this->dock_position         = this->settingsObj->value("/ui/dock_position", 0).toInt();
    this->dock_density          = this->settingsObj->value("/ui/dock_density", 1).toInt();
    this->dock_opacity          = this->settingsObj->value("/ui/dock_opacity", 871).toInt();
    // 磨砂玻璃背景：默认开启（悬浮球与弹窗共用）
    this->dock_remember_scroll  = this->settingsObj->value("/ui/dock_remember_scroll", 0).toInt();

    // [window]
    this->shape_mask            = this->settingsObj->value("/window/shape_mask", 0).toInt();

    // [system_monitor]
    this->system_monitor_cmd    = this->settingsObj->value("/system_monitor/cmd", "").toString().trimmed();

    // ---------------- 老配置的一次性迁移 ----------------
    // 磁盘读写速度在早期版本里默认是"显示"的，现在改成默认不显示（要的人自己去设置里勾）。
    // 已存在的配置里存着旧默认值 1，若不迁移用户会以为"改了默认值却没生效"。
    // 用 [meta] config_version 只迁一次，之后不再覆盖用户的主动选择。
    const int cfgVersion = this->settingsObj->value("/meta/config_version", 1).toInt();
    if (cfgVersion < 2)
    {
        this->settingsObj->setValue("/components_show/disk_io_show", 0);
        this->settingsObj->setValue("/meta/config_version", 2);
        this->settingsObj->sync();
        this->disk_io_show = 0;
    }
}


/** *********************************************************
                           set
********************************************************* */
void Config::setX(qint32 val)
{
    this->settingsObj->setValue("/position/x", val);
    this->x = val;
}

void Config::setY(qint32 val)
{
    this->settingsObj->setValue("/position/y", val);
    this->y = val;
}

void Config::setWidth(qint32 val)
{
    this->settingsObj->setValue("/appearance/width", val);
    this->width = val;
}

void Config::setHeight(qint32 val)
{
    this->settingsObj->setValue("/appearance/height", val);
    this->height = val;
}

void Config::setAsideWidth(qint32 val)
{
    this->settingsObj->setValue("/appearance/aside_width", val);
    this->aside_width = val;
}

void Config::setAsideHeight(qint32 val)
{
    this->settingsObj->setValue("/appearance/aside_height", val);
    this->aside_height = val;
}

void Config::setOpacity(double val)
{
    this->settingsObj->setValue("/appearance/opacity", val);
    this->opacity = val;
}

void Config::setShadowRadius(qint32 val)
{
    this->settingsObj->setValue("/appearance/shadow_radius", val);
    this->shadow_radius = val;
}

void Config::setShadowColor(QString val)
{
    this->settingsObj->setValue("/appearance/shadow_color", val);
    this->shadow_color = val;
}

void Config::setShape(qint32 val)
{
    this->settingsObj->setValue("/appearance/shape", val);
    this->shape = val;
}

void Config::setMainColor(QString val)
{
    this->settingsObj->setValue("/appearance/main_color", val);
    this->main_color = val;
}
void Config::setMainBorderColor(QString val)
{
    this->settingsObj->setValue("/appearance/main_border_color", val);
    this->main_border_color = val;
}

void Config::setMainBorderWidth(qint32 val)
{
    this->settingsObj->setValue("/appearance/main_border_width", val);
    this->main_border_width = val;
}

void Config::setMemColor(QString val)
{
    this->settingsObj->setValue("/appearance/mem_color", val);
    this->mem_color = val;

}
void Config::setSwapColor(QString val)
{
    this->settingsObj->setValue("/appearance/swap_color", val);
    this->swap_color = val;
}
void Config::setCpuUsageColor(QString val)
{
    this->settingsObj->setValue("/appearance/cpu_usage_color", val);
    this->cpu_usage_color = val;
}

void Config::setCpuUsageWidth(double val)
{
    this->settingsObj->setValue("/appearance/cpu_usage_width", val);
    this->cpu_usage_width = val;
}

void Config::setCpuFreqColor(QString val)
{
    this->settingsObj->setValue("/appearance/cpu_freq_color", val);
    this->cpu_freq_color = val;
}
void Config::setCpuTempColor(QString val)
{
    this->settingsObj->setValue("/appearance/cpu_temp_color", val);
    this->cpu_temp_color = val;
}
void Config::setNetSpeedColor(QString val)
{
    this->settingsObj->setValue("/appearance/net_speed_color", val);
    this->net_speed_color = val;
}
void Config::setDiskIoColor(QString val)
{
    this->settingsObj->setValue("/appearance/disk_io_color", val);
    this->disk_io_color = val;
}

void Config::setChartsRows(qint32 val)
{
    this->settingsObj->setValue("/appearance/charts_rows", val);
    this->charts_rows = val;
}

void Config::setUpdateDataInterval(qint32 val)
{
    this->settingsObj->setValue("/timer/update_data_interval", val);
    this->update_data_interval = val;
}

void Config::setUpdateUIInterval(qint32 val)
{
    this->settingsObj->setValue("/timer/update_ui_interval", val);
    this->update_ui_interval = val;
}

// [aside] 贴边竖条
void Config::setSnapToEdge(qint32 val)
{
    this->settingsObj->setValue("/aside/snap_to_edge", val);
    this->snap_to_edge = val;
}

void Config::setAsideEdge(qint32 val)
{
    this->settingsObj->setValue("/aside/aside_edge", val);
    this->aside_edge = val;
}

void Config::setAsideCornerRadius(qint32 val)
{
    this->settingsObj->setValue("/aside/corner_radius", val);
    this->aside_corner_radius = val;
}

// [components_show]
void Config::setCpuTempShow(qint8 val)
{
    this->settingsObj->setValue("/components_show/cpu_temp_show", val);
    this->cpu_temp_show = val;
}
void Config::setCpuFreqShow(qint8 val)
{
    this->settingsObj->setValue("/components_show/cpu_freq_show", val);
    this->cpu_freq_show = val;
}
void Config::setNetSpeedShow(qint8 val)
{
    this->settingsObj->setValue("/components_show/net_speed_show", val);
    this->net_speed_show = val;
}
void Config::setDiskIoShow(qint8 val)
{
    this->settingsObj->setValue("/components_show/disk_io_show", val);
    this->disk_io_show = val;
}

// [disk]
void Config::setDiskIoMode(qint32 val)
{
    this->settingsObj->setValue("/disk/disk_io_mode", val);
    this->disk_io_mode = val;
}
void Config::setDiskIoName(QString val)
{
    this->settingsObj->setValue("/disk/disk_io_name", val);
    this->disk_io_name = val;
}

// [ui]
void Config::setDockViewStyle(qint32 val)
{
    this->settingsObj->setValue("/ui/dock_view_style", val);
    this->dock_view_style = val;
}

void Config::setBallStyle(qint32 val)
{
    this->settingsObj->setValue("/ui/ball_style", val);
    this->ball_style = qBound<qint32>(0, val, 3);
}

void Config::setDockWidth(qint32 val)
{
    this->settingsObj->setValue("/ui/dock_width", val);
    this->dock_width = val;
}

void Config::setDockHeight(qint32 val)
{
    this->settingsObj->setValue("/ui/dock_height", val);
    this->dock_height = val;
}

void Config::setDockPosition(qint32 val)
{
    this->settingsObj->setValue("/ui/dock_position", val);
    this->dock_position = val;
}

void Config::setDockDensity(qint32 val)
{
    this->settingsObj->setValue("/ui/dock_density", val);
    this->dock_density = val;
}

void Config::setDockOpacity(qint32 val)
{
    this->settingsObj->setValue("/ui/dock_opacity", val);
    this->dock_opacity = val;
}

void Config::setRememberScroll(bool on)
{
    this->settingsObj->setValue("/ui/dock_remember_scroll", on ? 1 : 0);
    this->dock_remember_scroll = on ? 1 : 0;
}

// [window]
void Config::setShapeMask(qint32 val)
{
    this->settingsObj->setValue("/window/shape_mask", val);
    this->shape_mask = val;
}

// [system_monitor]
void Config::setSystemMonitorCmd(QString val)
{
    val = val.trimmed();   // 前后空白去掉，避免误判成非空命令
    this->settingsObj->setValue("/system_monitor/cmd", val);
    this->system_monitor_cmd = val;
}

/** *********************************************************
                           get
********************************************************* */
qint32 Config::getX()
{
    return this->x;
}

qint32 Config::getY()
{
    return this->y;
}

qint32 Config::getWidth()
{
    return this->width;
}

qint32 Config::getHeight()
{
    return this->height;
}

qint32 Config::getAsideWidth()
{
    return this->aside_width;
}

qint32 Config::getAsideHeight()
{
    return this->aside_height;
}

double Config::getOpacity()
{
    return this->opacity;
}

qint32 Config::getShadowRadius()
{
    return this->shadow_radius;
}

QString Config::getShadowColor()
{
    return this->shadow_color;
}

qint32 Config::getShape()
{
    return this->shape;
}

QString Config::getMainColor()
{
    return this->main_color;
}
QString Config::getMainBorderColor()
{
    return this->main_border_color;
}
qint32 Config::getMainBorderWidth()
{
    return this->main_border_width;
}
QString Config::getMemColor()
{
    return this->mem_color;
}
QString Config::getSwapColor()
{
    return this->swap_color;
}
QString Config::getCpuUsageColor()
{
    return this->cpu_usage_color;
}
double Config::getCpuUsageWidth()
{
    return this->cpu_usage_width;
}
QString Config::getCpuFreqColor()
{
    return this->cpu_freq_color;
}
QString Config::getCpuTempColor()
{
    return this->cpu_temp_color;
}
QString Config::getNetSpeedColor()
{
    return this->net_speed_color;
}
QString Config::getDiskIoColor()
{
    return this->disk_io_color;
}

qint32 Config::getChartsRows()
{
    return this->charts_rows;
}

qint32 Config::getUpdateDataInterval()
{
    return this->update_data_interval;
}

qint32 Config::getUpdateUIInterval()
{
    return this->update_ui_interval;
}

// [aside] 贴边竖条
qint32 Config::getSnapToEdge()
{
    return this->snap_to_edge;
}

qint32 Config::getAsideEdge()
{
    return this->aside_edge;
}

qint32 Config::getAsideCornerRadius()
{
    return this->aside_corner_radius;
}

// [components_show]
qint8 Config::getCpuTempShow()
{
    return this->cpu_temp_show;
}
qint8 Config::getCpuFreqShow()
{
    return this->cpu_freq_show;
}
qint8 Config::getNetSpeedShow()
{
    return this->net_speed_show;
}
qint8 Config::getDiskIoShow()
{
    return this->disk_io_show;
}

// [disk]
qint32 Config::getDiskIoMode()
{
    return this->disk_io_mode;
}
QString Config::getDiskIoName()
{
    return this->disk_io_name;
}

// [ui]
qint32 Config::getDockViewStyle()
{
    return this->dock_view_style;
}

qint32 Config::getBallStyle()
{
    return this->ball_style;
}

qint32 Config::getDockWidth()
{
    return this->dock_width;
}

qint32 Config::getDockHeight()
{
    return this->dock_height;
}

qint32 Config::getDockPosition()
{
    return this->dock_position;
}

qint32 Config::getDockDensity()
{
    return this->dock_density;
}

qint32 Config::getDockOpacity()
{
    return this->dock_opacity;
}

bool Config::getRememberScroll()
{
    return this->dock_remember_scroll != 0;
}

// [window]
qint32 Config::getShapeMask()
{
    return this->shape_mask;
}

// [system_monitor]
QString Config::getSystemMonitorCmd()
{
    return this->system_monitor_cmd;
}


