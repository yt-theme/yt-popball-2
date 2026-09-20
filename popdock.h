#ifndef POPDOCK_H
#define POPDOCK_H

#include <QWidget>
#include <QListWidget>
#include <QToolButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QLineEdit>
#include <QVector>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QEnterEvent>
#include <QShowEvent>
#include <QVariantMap>
#include <QColor>

#include "clipstore.h"

class TransferStation;
class TsItemDelegate;
class QPropertyAnimation;
class QParallelAnimationGroup;
class QVariantAnimation;
class QMenu;
class QPlainTextEdit;
class QCloseEvent;
class QWheelEvent;
#ifdef POPBALL2_HAVE_QT_MULTIMEDIA
class QMediaPlayer;
class QVideoSink;
#endif

// 中转站条目类型（Video = 按扩展名识别的视频文件）
enum class TsType { File, Image, Text, Video };

// 图标网格 / 列表 / 详细 / 预览 四种展示形式下的中转站。
// 支持：拖入文件、粘贴剪贴板图像(Ctrl/Cmd+V)、双击打开、拖出、右键（复制/打开/删除）、
//       Delete 删除；所有新增/删除都会同步到剪贴板历史库（若可用）。
class TransferStation : public QListWidget
{
    Q_OBJECT
public:
    // 展示布局：图标网格（默认）/ 列表 / 详细 / 预览（只有图标）
    enum ViewStyle { IconView = 0, ListView = 1, DetailView = 2, PreviewView = 3 };
    enum { kIconColumns = 4, kPreviewColumns = 2 };   // 每种布局每行的列数

    explicit TransferStation(QWidget *parent = nullptr);

    void addFileItem(const QString &path, int source = 1);
    // 一批文件按**给定顺序**成组入列（复制/拖入 A,B,C 后，面板里也是 A,B,C）。
    // source：1=中转站（拖入，默认） 0=剪贴板。
    void addFileItems(const QStringList &paths, int source = 1);
    void addImageItem(const QImage &image, const QString &name = QString(), int source = 0);
    void addTextItem(const QString &text, const QString &label = QString(), int source = 0);
    // 从剪贴板粘贴（图像或文本）到中转站
    void pasteClipboard();

    // 切换展示布局（图标 / 列表 / 详细 / 预览）
    void setViewStyle(ViewStyle style);
    ViewStyle viewStyle() const { return m_viewStyle; }

    // 内容密度：0=紧凑 1=标准（默认） 2=宽松。
    // 影响图标/预览网格的列数与缩略图尺寸、列表/详细的行高与图标大小。
    void setDensity(int density);
    int  density() const { return m_density; }
    // 图标网格按密度取的列数（紧凑 5 / 标准 4 / 宽松 3）
    int  densityIconColumns() const;

    // 类型筛选（标题区下面那条 tab）：
    //   AllItems   全部
    //   DocItems    文档 = 文本条目 + 文档类文件（isDocFile 的白名单）
    //   ImageItems  图片 = 剪贴板图片 + 图片文件
    //   VideoItems  视频 = 视频文件（type==Video / isVideoFile）
    //   AppItems    安装包 = apk / ipa / exe / dmg / deb 等
    //   ArchiveItems 压缩包 = zip / rar / 7z / tar 等
    //   AudioItems  音频 = mp3 / wav / flac 等
    //   ExecItems   可执行 = 无后缀可执行 / .app / .command / .elf / .so / .dylib 等
    //   FontItems   字体 = ttf / otf / woff / woff2 等
    //   DataItems   数据库/数据 = db / sqlite / mdb / dat 等
    //   DesignItems 设计/CAD = psd / ai / sketch / dwg / stl / blend 等
    // 以上分类互斥（每条目只属一类，见 categoryOf）；无专属分类的内容只在「全部」里出现。
    // 用 setHidden 隐藏而不是删除条目：顺序、去重、库里的记录都不受影响。
    enum TypeFilter { AllItems = 0, DocItems = 1, ImageItems = 2,
                      VideoItems = 3, AppItems = 4, ArchiveItems = 5, AudioItems = 6,
                      ExecItems = 7, FontItems = 8, DataItems = 9, DesignItems = 10 };

    void setTypeFilter(TypeFilter f);
    TypeFilter typeFilter() const { return m_filter; }
    // 来源筛选：0=全部（默认） 1=仅剪贴板 2=仅中转站（拖入文件/新增记事）。
    // 与类型筛选正交叠加：条目须同时通过"类型"与"来源"两个条件才可见。
    void setSourceFilter(int f);
    int  sourceFilter() const { return m_sourceFilter; }
    // 某条目是否通过来源筛选（静态，便于自检直接验证判定规则）。
    // 旧条目 / 旧库记录没有 source 键时按 0（剪贴板）处理。
    static bool itemMatchesSource(const QVariantMap &data, int filter);
    // 当前筛选下可见的条目数
    int visibleCount() const;
    // 某条目是否属于该筛选（静态，便于自检直接验证判定规则）
    static bool itemMatchesFilter(const QVariantMap &data, TypeFilter f);
    // 条目归属的大分类（互斥，返回 TypeFilter 或 -1 = 无专属分类、只在「全部」里）。
    // 判定规则集中在这里，自检可以直接验证，不必去拼界面。
    static int categoryOf(const QVariantMap &data);
    // 文档类文件判定（按扩展名：文本/代码/Office/PDF 等）
    static bool isDocFile(const QString &path);
    // 视频 / 图片 / 安装包 / 压缩包 / 音频 文件判定（按扩展名）
    static bool isVideoFile(const QString &path);
    static bool isImageFile(const QString &path);
    static bool isAppFile(const QString &path);
    static bool isArchiveFile(const QString &path);
    static bool isAudioFile(const QString &path);
    // 可执行 / 字体 / 数据库 / 设计-CAD 文件判定（按扩展名）
    static bool isExecFile(const QString &path);
    static bool isFontFile(const QString &path);
    static bool isDataFile(const QString &path);
    static bool isDesignFile(const QString &path);

    // 剪贴板历史（可为空 = 不持久化，功能照旧）
    void setStore(ClipStore *store) { m_store = store; }
    // 用历史记录回填。**顺序由每条记录自带的 usedAt 决定，与传入顺序无关**：
    // 同一条历史无论先来后到，插进去的位置都一样 ⇒ 图标/列表/详细/预览 四种布局
    // 看到的顺序也必然一致（用户要求的"统一，最新的在前"）。
    void loadRecords(const QVector<ClipRecord> &records);

    // 按当前布局/面板宽度重算网格单元格
    void updateIconGrid();

    // 网格位移动画：条目增删 / 切换筛选后，剩余可见格子从旧位置平滑滑到新位置（向前对齐）。
    // oldPos 由调用方在数据变化前快照（snapshotVisibleRects），动画只影响绘制、不耽误交互。
    void startGridShiftAnimation(const QHash<qint64, QRect> &oldPos);
    // 快照当前所有可见条目（rank → 视口内位置），供位移动画在增删/筛选前记录旧位置。
    QHash<qint64, QRect> snapshotVisibleRects() const;

    // 滚动位置记忆：保存当前滚动值 / 恢复上次保存值 / 滚回顶部。
    // 由 PopDock 在弹窗显隐时按配置调用（默认不记住 → 每次打开都从顶部开始）。
    void saveScrollPosition();
    void restoreScrollPosition();
    void scrollToTop();

    // 视频封面帧（异步生成后缓存；没有就返回空）
    QPixmap videoThumbFor(const QString &dedupKey) const;

    // 读取条目上挂的自定义数据
    static QVariantMap dataOf(QListWidgetItem *it);
    // 条目的"排序键"（最近使用时刻，ms）。列表自上而下非递增 —— 越靠前越新。
    // 用纯整数比较，不依赖文件时间/本地时间格式，因此跨 Linux/macOS/Windows 表现一致。
    static qint64 rankOf(const QListWidgetItem *it);
    // 详细模式第二行的说明文字（类型 / 大小 / 路径 / 文本摘要）
    static QString subtitleFor(const QVariantMap &data);

    // 条目操作（右键菜单与快捷键共用；也便于自检直接调用）
    void copyItemToClipboard(QListWidgetItem *it);
    void openItem(QListWidgetItem *it);      // 文本 → 弹小编辑器；其它 → 系统默认程序打开
    void removeItem(QListWidgetItem *it);
    // ---- 文本条目的小编辑器（双击 / 右键「打开」）----
    // 用 dbId（优先，最可靠）或"载入时的 dedup 键"定位文本条目；找不到返回 nullptr
    QListWidgetItem *findTextItem(qint64 dbId, const QString &originalKey) const;
    // 把编辑器里的新内容写回条目与历史库。条目已被删除 → 返回 false（不写库、不复活它）
    bool applyTextEdit(qint64 dbId, const QString &originalKey, const QString &text);
    // 往给定菜单里填"复制 / 打开 / 删除"（抽出来是为了让自检能直接检查/出图，
    // 不必走模态的 menu.exec()）
    void fillItemMenu(QMenu &menu);

    // 强调色（主题色）：列表选中条目 / 右键菜单高亮底色跟随主题，
    // 由 PopDock 在主题（main_border_color）变化时调用刷新
    void setAccentColor(const QColor &color);
    QColor accentColor() const { return m_accentColor; }

signals:
    void stationChanged();
    // 文本条目要"打开"：请求弹出小编辑器（PopDock 拥有窗口；dbId/key 用于回写时定位条目）
    void editTextRequested(qint64 dbId, const QString &key, const QString &title, const QString &text);
    // 鼠标在某个条目上悬停达阈值：请求显示预览（globalCenter 为该条目图标的全局中心）
    void previewRequested(const QVariantMap &data, const QPoint &globalCenter);
    // 光标离开条目 / 离开列表 / 滚动 / 按下：请求收起预览
    void previewHideRequested();
    // 即将进入/结束"嵌套模态交互"（右键菜单、拖出条目）。
    // 期间 PopDock 会加交互锁：光标离开面板也不收起 —— 用户明明在操作，面板不能跑掉。
    void interactionBegin();
    void interactionEnd();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void startDrag(Qt::DropActions supportedActions) override;
    void resizeEvent(QResizeEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    // 鼠标滚轮滚动列表：累积目标 + 每帧持续趋近的连续平滑滚动（Firefox 式，触控板像素滚动走原生）。
    // 连续滚动只更新目标值、动画不停顿不跳格；接近目标后自动停止，不耽误交互。
    void wheelEvent(QWheelEvent *event) override;
    void scrollTick();          // 每帧把滚动条往目标值推进一步（指数趋近）
    void abortScrollAnim();     // 外部直接定位滚动（恢复/回顶/筛选跳转）时放弃平滑动画

private:
    // 插入一条条目。rank < 0 = 自动取"当前最新"（严格大于列表里所有条目 ⇒ 排到最前）；
    // rank >= 0 时（历史回填）按这个时间戳插到正确位置。
    // dbId >= 0 表示这条来自历史库（已在库里，不再重复写）
    void appendItem(const QIcon &icon, const QString &text, TsType type,
                    const QVariantMap &data, qint64 rank = -1);
    // 插入位置：列表按 rank 非递增排列，返回第一条比它旧的条目下标（相同 rank 时
    // 保持"先来的在前"，因此同一批历史无论以什么顺序传入，最终次序都相同）
    int  orderedRow(qint64 rank) const;
    // 新条目的 rank：max(当前毫秒, 列表最大 rank + 1)，严格递增 —— 即使系统时钟
    // 被回拨、或同一毫秒内连着来好几条，新条目也一定排在最前。
    qint64 nextRank();
    void noteRank(qint64 rank);
    void addTextItemEx(const QString &text, const QString &label, qint64 dbId, qint64 rank = -1, int source = 0);
    void addImageItemEx(const QImage &image, const QString &name, qint64 dbId, qint64 rank = -1, int source = 0);
    void addFileItemEx(const QString &path, qint64 dbId, qint64 rank = -1, int source = 0);
    QString saveImageTemp(const QImage &img);
    static QPixmap textIcon(const QColor &accent);
    // 视频条目图标：封面帧（或胶片占位图）+ 居中播放按钮
    static QIcon videoIcon(const QPixmap &frame);
    // 图片文件（.jpg/.png…）当条目时的**真实缩略图**（带尺寸读 + 按路径缓存）。
    // 读不了（缺插件的 svg/heic、损坏文件）返回空 map，调用方退回系统图标。
    QPixmap imageFileThumb(const QString &path);
    // 异步生成视频封面帧（qlmanage / ffmpeg），完成后回填条目图标
    void requestVideoThumb(const QString &dedupKey, const QString &path);
    // 按内容去重：移除已有相同内容的条目（使其随后可被提升/重新插入到顶部）
    bool removeExisting(const QString &dedupKey);
    bool hasDedup(const QString &dedupKey) const;
    // 同一张图片（无论来源）生成稳定指纹，用于去重
    static QString imageHash(const QImage &img);
    // 按当前筛选隐藏/显示所有条目（用 setHidden，不动顺序）
    void applyFilter();
    // 悬停条目变化 → 重启/停止预览计时
    void updateHover(QListWidgetItem *item, const QPoint &globalCenter);
    // 依当前强调色重建列表样式（选中条目底色跟随主题）
    void rebuildStyleSheet();
    // 主题色变化后，把已有文本条目的图标重新着色（文本图标是自绘的，需跟随主题）
    void recolorTextIcons();

    ViewStyle       m_viewStyle  = IconView;
    TypeFilter      m_filter     = AllItems;  // 当前类型筛选
    int             m_sourceFilter = 0;       // 来源筛选：0=全部 1=仅剪贴板 2=仅中转站
    int             m_density    = 1;        // 0=紧凑 1=标准 2=宽松
    int             m_savedScroll = 0;       // 记住的滚动位置（弹窗关闭时保存，下次打开恢复）
    QColor          m_accentColor = QColor::fromRgb(0x41, 0xB0, 0xDD); // 主题强调色（默认=主题蓝）
    TsItemDelegate *m_delegate   = nullptr;   // 图标/列表/详细/预览 四种绘制模式
    QTimer         *m_hoverTimer = nullptr;
    QListWidgetItem *m_hoverItem = nullptr;
    QPoint           m_hoverCenter;           // 当前悬停条目图标的全局中心
    ClipStore       *m_store     = nullptr;   // 不持有所有权（由 PopDock 持有）
    QSize            m_iconCell  = QSize(76, 110);  // 网格单元格（随布局/宽度重算）
    int              m_iconPx    = 66;              // 缩略图方块边长（图标格）/ 单元格边长（预览）
    QHash<QString, QPixmap> m_videoThumbs;          // dedup 键 → 视频封面帧
    QHash<QString, QPixmap> m_imageFileThumbs;      // 文件路径 → 图片文件缩略图
    QSet<QString>           m_videoThumbPending;    // 正在提取中的（避免重复起进程）
    QSet<QString>           m_videoThumbFailed;     // 提取失败的（不再重试）
    qint64                  m_lastRank = 0;         // 已用过的最大排序键（保证严格递增）
    QVariantAnimation      *m_shiftAnim = nullptr;  // 网格位移动画（增删/筛选后剩余格子向前对齐）
    QTimer                 *m_scrollTimer = nullptr; // 滚轮连续平滑滚动驱动（约 16ms/帧）
    int                     m_scrollTarget = -1;  // 滚轮累积的滚动目标值；-1 = 无进行中动画
};

// 悬停预览气泡：图片 / 文件图标 / 文本内容 / 视频（有 QtMultimedia 时静音播放）。
// 用 ToolTip 型窗口（不抢焦点）且对鼠标透明（不干扰面板的进出判定）。
class PreviewPopup : public QWidget
{
public:
    explicit PreviewPopup(QWidget *parent = nullptr);

    void showPixmap(const QPixmap &pixmap, const QString &caption);
    void showText(const QString &text, const QString &caption);
    // 视频：能播就静音循环播放（画面逐帧画进内容标签），不能播就退化成静态封面
    void showVideo(const QString &path, const QString &caption,
                   const QPixmap &fallbackStill);
    // 下沿信息栏：显示条目详情（类型 / 尺寸 / 大小 / 完整路径），
    // 替代旧版"悬停 item 时弹出的 tooltip 气泡"
    void setInfo(const QString &text);
    // 停止播放（气泡收起 / 换条目时都要调）
    void stopVideo();
    bool isPlayingVideo() const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void setFrame(const QImage &frame);

    QLabel *m_content = nullptr;
    QLabel *m_caption = nullptr;
    QLabel *m_info    = nullptr;   // 下沿详情栏（类型/尺寸/大小/路径）
#ifdef POPBALL2_HAVE_QT_MULTIMEDIA
    QMediaPlayer *m_player = nullptr;
    QVideoSink   *m_sink   = nullptr;
#endif
    QSize   m_videoSize;         // 播放画面的固定尺寸（16:9 内）
};

// 文本条目的小编辑器：双击 / 右键「打开」时弹出，预览并能直接改。
// 独立顶层窗口（系统标题栏，可拖动缩放），深色主题与面板一致。
// 关闭时若有未保存的改动会**自动保存**（不弹模态框 —— 免得把用户卡住，也便于无头自检）。
class TextEditorWindow : public QWidget
{
public:
    explicit TextEditorWindow(QWidget *parent = nullptr);

    // 主题强调色：文本选区底色 / 聚焦边框跟随主题（由 PopDock 同步）
    void setAccentColor(const QColor &color);
    QColor accentColor() const { return m_accentColor; }

    // 载入一条文本条目（dbId / originalKey 用于回写时定位）
    void showFor(TransferStation *station, qint64 dbId, const QString &originalKey,
                 const QString &title, const QString &text);

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void save();                       // 写回条目 + 历史库
    void refreshState();               // 字数 / 已修改 / 保存按钮可用性
    bool modified() const;

    TransferStation *m_station = nullptr;
    qint64           m_dbId    = -1;   // 条目在历史库里的行 id（无库时为 -1）
    QString          m_key;            // 载入时的 dedup 键（无 dbId 时用它定位条目）
    QString          m_baseline;       // "已保存"的正文：与它比对来判断有没有改动
    QString          m_idleText;       // 未修改时状态栏显示的字样（未修改/已保存/条目已删除…）
    QColor           m_accentColor = QColor::fromRgb(0x41, 0xB0, 0xDD); // 主题强调色（默认=主题蓝）
    QLabel          *m_status  = nullptr;      // 底栏左：状态 · 字数 · 行数
    QPlainTextEdit  *m_edit    = nullptr;      // 正文
    QToolButton     *m_saveBtn = nullptr;      // 保存（未改动时禁用）
};

// 悬浮球 hover / 拖拽 300ms 后弹出的"数据中转站"面板。
// - 顶部一条小标题区（左：数据中转站，右：条目数 —— 有筛选时是"可见 / 总数 项"）
// - 标题区下面一条类型 tab（全部 / 文档 / 图片），只筛"看什么"，不动数据与顺序
// - 打开即自动记录当前剪贴板内容，并把剪贴板变化持续收进中转站（含历史持久化）
// - 底部一条紧凑工具栏：左侧切换展示布局（图标 / 列表 / 详细 / 预览），右侧"铅笔+加号"新增记事
// - 条目悬停可预览图片 / 文本 / 文件 / 视频；右键可复制 / 打开 / 删除
class PopDock : public QWidget
{
    Q_OBJECT
public:
    // 面板首选尺寸。屏幕装不下时（小屏/投影）Widget 会调 setFixedSize 收缩，
    // 所以这里用常量暴露出来，避免在别处硬编码。
    // 高度里含 24px 的类型 tab 行（标题 22 + tab 24 + 底栏 26 + 两条 8px 间距）。
    static constexpr int kPreferredWidth  = 330;
    static constexpr int kPreferredHeight = 452;

    explicit PopDock(QWidget *parent = nullptr);
    ~PopDock() override;

    // 供悬浮球调用：把拖入 / 粘贴的内容塞进中转站
    void addFiles(const QStringList &paths);
    void addImage(const QImage &image);
    void addText(const QString &text);

    // 展示布局（供 Widget 按配置回填；用户点按钮切换时发 viewStyleChanged）
    void setViewStyle(int style);
    int  viewStyle() const;

    // 类型 tab（全部 / 文档 / 图片）。与布局不同：筛选**不落盘** ——
    // 它只是"看"的方式，不是用户偏好，重启后回到「全部」。
    void setTypeFilter(int filter);
    int  typeFilter() const;

    // 剪贴板历史库（默认 ~/.popball2_clipboard.db，可用 POPBALL2_DB 覆盖 —— 自检隔离用）
    ClipStore *store() const { return m_store; }

    // 强调色（主题色）：面板内所有"激活/选中"样式（选中条目、类型 tab、视图按钮、
    // 条目右键菜单）跟随主题（main_border_color），由 Widget 在构造与设置保存后调用
    void setAccentColor(const QColor &color);
    QColor accentColor() const { return m_accentColor; }

    // 应用中转站弹窗设置：尺寸（宽/高，屏幕放不下时仍会由 Widget 收缩）与内容密度。
    // 由 Widget 在构造与设置保存后调用；位置由 Widget 计算，不在这里。
    void applyDockSettings(int width, int height, int density);
    void resizeEvent(QResizeEvent *event) override;
    // 弹窗背景不透明度（千分比 0~1000，默认 871 = 87.1%）
    void setDockOpacity(int permille);
    int dockOpacity() const { return m_dockOpacity; }
    // 弹窗是否记住上次滚动位置：true=关闭再打开回到上次滚动处；false=每次打开从顶部开始（默认）
    void setRememberScroll(bool on);
    bool rememberScroll() const { return m_rememberScroll; }
    // 当前首选尺寸（默认 = 静态常量；applyDockSettings 后跟随配置）
    int preferredWidth()  const { return m_prefWidth; }
    int preferredHeight() const { return m_prefHeight; }

    // 文本小编辑器窗口（懒创建；空闲时为 nullptr）。返回 QWidget* 便于无头自检定位子控件。
    QWidget *textEditor() const;

    // ---------- 划出 / 划入动画 ----------
    void showAnimated(const QPoint &targetPos, const QRect &ballRect);
    void hideAnimated();
    void cancelHideAnimation();
    bool isHiding() const { return m_hiding; }
    QRect targetRect() const { return QRect(m_targetPos, size()); }

    // 面板里正在发生"嵌套模态交互"（右键菜单 / 拖出条目）时为真：
    // 这期间即使光标离开面板（例如移到弹出的菜单上）也不应自动收起。
    bool isInteractionLocked() const { return m_interactionLock > 0; }

signals:
    void mouseEntered();            // 光标进入面板（含子控件）
    void mouseLeft();               // 光标真正离开面板
    void viewStyleChanged(int style);   // 用户切换了展示布局（Widget 负责落盘）
    void interactionStarted();      // 进入交互锁（Widget 据此取消已排队的收起）
    // 右上角"操作"菜单（设置 / 系统监视器 / 退出）——原悬浮球右键菜单的动作，
    // 由 Widget 连接执行
    void settingsRequested();
    void systemMonitorRequested();
    void quitRequested();
    // 用户拖拽弹窗边缘调整大小后发出（Widget 负责写入配置持久化；本对象同步首选尺寸）
    void sizeEdited(int width, int height);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    // ---- 鼠标边缘调整大小（拖弹窗边框改尺寸；放开后发 sizeEdited 落盘） ----
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private slots:
    void onNoteHover();             // 新增记事：浮出输入框
    void onNoteReturn();            // 记事：回车保存
    void onItemDoubleClicked(QListWidgetItem *item);
    void onPreviewRequested(const QVariantMap &data, const QPoint &globalCenter);
    void onPreviewHideRequested();
    void onClipboardChanged();      // 剪贴板变了（防抖后真正抓取）
    void captureClipboard();        // 把当前剪贴板内容收进中转站 + 入库

private:
    void installDockTracking(QObject *obj);   // 递归安装事件过滤器，稳定捕获进出
    void ensureHistoryLoaded();               // 首次用面板时回填历史
    void openHistoryStore();                  // 打开剪贴板历史库
    void refreshCount();                      // 刷新标题区右侧的条目数
    void refreshTypeTabs();                   // 按站内实际内容增删/显示隐藏类型 tab
    void hidePreview();                       // 收起悬停预览
    void openTextEditor(qint64 dbId, const QString &key,
                        const QString &title, const QString &text);   // 弹文本小编辑器
    void ensureAnimations();                  // 懒创建划出/划入动画
    void beginInteraction();                  // 交互锁 +1（右键菜单 / 拖拽开始时）
    void endInteraction();                    // 交互锁 -1
    static QIcon noteAddIcon(const QColor &accent);   // 右下角"铅笔+加号"图标
    static QIcon noteOkIcon(const QColor &accent);    // 记事输入框右侧的"确认"图标
    static QIcon settingsIcon(const QColor &accent);  // 右上角"设置"：用户 SVG（齿轮）形状 + 主题色
    static QIcon monitorIcon(const QColor &accent);   // 右上角"系统监视器"：用户 SVG（性能统计）+ 主题色
    static QIcon powerIcon(const QColor &accent);     // 右上角"退出"：用户 SVG（电源开关）+ 主题色
    // 读内嵌 SVG 资源并把原图 #515151 灰替换为主题强调色后渲染；缺 QtSvg 返回空 QIcon。
    static QIcon svgThemeIcon(const QString &resPath, const QColor &accent);

    TransferStation *m_station    = nullptr;
    QLabel          *m_titleLabel = nullptr;  // 标题区：数据中转站
    QLabel          *m_countLabel = nullptr;  // 标题区右侧：条目数
    QToolButton     *m_settingsBtn = nullptr; // 标题区右："设置"（齿轮）按钮
    QToolButton     *m_monitorBtn  = nullptr; // 标题区右："系统监视器"（柱状图）按钮
    QToolButton     *m_powerBtn    = nullptr; // 标题区最右："退出"（电源）按钮
    QToolButton     *m_addNoteBtn = nullptr;  // 底栏右：新建记事
    QWidget         *m_noteRow    = nullptr;  // 记事编辑行（输入框 + 确认按钮）
    QLineEdit       *m_noteEdit   = nullptr;  // 新建记事时的输入框
    QToolButton     *m_noteOkBtn  = nullptr;  // 确认保存（中文输入法下回车常被用于确认候选词）
    PreviewPopup    *m_preview    = nullptr;  // 悬停预览气泡（懒创建）
    TextEditorWindow *m_textEditor = nullptr; // 文本小编辑器（懒创建，顶层窗口）
    QToolButton     *m_viewBtns[4] = { nullptr, nullptr, nullptr, nullptr };   // 底栏左：图标/列表/详细/预览
    QToolButton     *m_tabBtns[11] = { nullptr, nullptr, nullptr, nullptr,
                                       nullptr, nullptr, nullptr, nullptr,
                                       nullptr, nullptr, nullptr };       // 类型 tab：全部/文档/图片/视频/安装包/压缩包/音频/可执行/字体/数据库/设计
    QButtonGroup    *m_tabGroup    = nullptr;                            // 类型 tab 互斥组
    QWidget         *m_tabsWidget  = nullptr;                            // 类型 tab 容器（按行数自适应高度）
    QHBoxLayout     *m_tabRow1     = nullptr;                            // tab 第一行
    QHBoxLayout     *m_tabRow2     = nullptr;                            // tab 第二行（放不下时换行）
    QCheckBox       *m_clipOnlyBtn   = nullptr;   // 来源开关：仅剪贴板
    QCheckBox       *m_transitOnlyBtn = nullptr;  // 来源开关：仅中转数据
    QColor           m_accentColor = QColor::fromRgb(0x41, 0xB0, 0xDD);         // 主题强调色（默认=主题蓝）
    int              m_prefWidth  = kPreferredWidth;    // 弹窗首选尺寸（跟随配置）
    // 鼠标边缘调整大小
    enum { kResizeNone = 0, kResizeL = 1, kResizeR = 2, kResizeT = 4, kResizeB = 8 };
    static constexpr int kMinDockWidth  = 240;    // 与设置窗 spin 范围 / applyDockSettings 一致
    static constexpr int kMinDockHeight = 260;
    static constexpr int kResizeHandle  = 6;      // 边缘热区宽度（px）
    int    m_resizeEdges   = kResizeNone;         // 当前正在拖拽的边（位标志）
    QRect  m_resizeStart;                         // 按下时窗口几何（屏幕坐标）
    QPoint m_resizePressPos;                      // 按下时鼠标全局位置
    QPoint m_resizePressLocal;                    // Wayland 下无全局坐标，用局部按下点算增量
    bool   m_mouseResizing = false;
    int    resizeEdgeAt(const QPoint &localPos) const;   // 光标位置 → 边缘位标志
    static Qt::CursorShape cursorForEdges(int edges);    // 边缘 → 光标形状
    int              m_prefHeight = kPreferredHeight;
    int              m_dockOpacity = 871;               // 弹窗背景不透明度（千分比，默认 87.1%）
    bool             m_rememberScroll = false;          // 记住上次滚动位置（默认不记住）
    ClipStore       *m_store      = nullptr;  // 剪贴板历史（可为未就绪）
    bool             m_historyLoaded = false; // 历史是否已回填
    QTimer          *m_clipTimer  = nullptr;  // 剪贴板变化防抖
    int              m_interactionLock = 0;   // >0 = 正在菜单/拖拽里操作，别自动收起

    // 划出/划入动画
    QPropertyAnimation      *m_slide = nullptr;
    QPropertyAnimation      *m_fade  = nullptr;
    QParallelAnimationGroup *m_anim  = nullptr;
    QPoint                   m_slideFrom;      // 划出起点：收起时沿原路滑回这里
    QPoint                   m_targetPos;      // 落点（动画中途也按它判定命中）
    bool                     m_hasSlideFrom = false;
    bool                     m_hiding = false; // 正在播放收起动画
};

#endif // POPDOCK_H
