#ifndef POPDOCK_H
#define POPDOCK_H

#include <QWidget>
#include <QListWidget>
#include <QToolButton>
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

#include "clipstore.h"

class TransferStation;
class TsItemDelegate;
class QPropertyAnimation;
class QParallelAnimationGroup;
class QMenu;

// 中转站条目类型
enum class TsType { File, Image, Text };

// 图标网格 / 列表 / 详细 三种展示形式下的中转站。
// 支持：拖入文件、粘贴剪贴板图像(Ctrl/Cmd+V)、双击打开、拖出、右键（复制/打开/删除）、
//       Delete 删除；所有新增/删除都会同步到剪贴板历史库（若可用）。
class TransferStation : public QListWidget
{
    Q_OBJECT
public:
    // 展示布局：图标网格（默认）/ 列表 / 详细
    enum ViewStyle { IconView = 0, ListView = 1, DetailView = 2 };
    enum { kIconColumns = 4 };      // 图标网格每行固定 4 列

    explicit TransferStation(QWidget *parent = nullptr);

    void addFileItem(const QString &path);
    void addImageItem(const QImage &image, const QString &name = QString());
    void addTextItem(const QString &text, const QString &label = QString());
    // 从剪贴板粘贴（图像或文本）到中转站
    void pasteClipboard();

    // 切换展示布局（图标 / 列表 / 详细）
    void setViewStyle(ViewStyle style);
    ViewStyle viewStyle() const { return m_viewStyle; }

    // 剪贴板历史（可为空 = 不持久化，功能照旧）
    void setStore(ClipStore *store) { m_store = store; }
    // 用历史记录回填（追加到列表末尾：调用时列表里已有的都是更新的内容）
    void loadRecords(const QVector<ClipRecord> &records);

    // 按屏幕/面板宽度重算图标网格单元格（4 列）
    void updateIconGrid();

    // 读取条目上挂的自定义数据
    static QVariantMap dataOf(QListWidgetItem *it);
    // 详细模式第二行的说明文字（类型 / 大小 / 路径 / 文本摘要）
    static QString subtitleFor(const QVariantMap &data);
    // 单行 tooltip（类型 · 大小 · 摘要 / 内容预览）
    static QString tooltipFor(const QVariantMap &data);

    // 条目操作（右键菜单与快捷键共用；也便于自检直接调用）
    void copyItemToClipboard(QListWidgetItem *it);
    void openItem(QListWidgetItem *it);
    void removeItem(QListWidgetItem *it);
    // 往给定菜单里填"复制 / 打开 / 删除"（抽出来是为了让自检能直接检查/出图，
    // 不必走模态的 menu.exec()）
    void fillItemMenu(QMenu &menu);

signals:
    void stationChanged();
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

private:
    // toTop=true 时插入到最前（新条目永远排在最前：越新越靠前）
    // dbId >= 0 表示这条来自历史库（已在库里，不再重复写）
    void appendItem(const QIcon &icon, const QString &text, TsType type,
                    const QVariantMap &data, bool toTop = false);
    void addTextItemEx(const QString &text, const QString &label, qint64 dbId);
    void addImageItemEx(const QImage &image, const QString &name, qint64 dbId);
    void addFileItemEx(const QString &path, qint64 dbId);
    QString saveImageTemp(const QImage &img);
    static QPixmap textIcon();
    // 按内容去重：移除已有相同内容的条目（使其随后可被提升/重新插入到顶部）
    bool removeExisting(const QString &dedupKey);
    bool hasDedup(const QString &dedupKey) const;
    // 同一张图片（无论来源）生成稳定指纹，用于去重
    static QString imageHash(const QImage &img);
    // 悬停条目变化 → 重启/停止预览计时
    void updateHover(QListWidgetItem *item, const QPoint &globalCenter);

    ViewStyle       m_viewStyle  = IconView;
    TsItemDelegate *m_delegate   = nullptr;   // 图标/列表走默认绘制，详细走两行自定义绘制
    QTimer         *m_hoverTimer = nullptr;
    QListWidgetItem *m_hoverItem = nullptr;
    QPoint           m_hoverCenter;           // 当前悬停条目图标的全局中心
    ClipStore       *m_store     = nullptr;   // 不持有所有权（由 PopDock 持有）
    QSize            m_iconCell  = QSize(74, 88);   // 图标网格单元格（随宽度重算）
    int              m_iconPx    = 48;              // 图标边长
};

// 悬停预览气泡：图片 / 文件图标 / 文本内容。
// 用 ToolTip 型窗口（不抢焦点）且对鼠标透明（不干扰面板的进出判定）。
class PreviewPopup : public QWidget
{
public:
    explicit PreviewPopup(QWidget *parent = nullptr);

    void showPixmap(const QPixmap &pixmap, const QString &caption);
    void showText(const QString &text, const QString &caption);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QLabel *m_content = nullptr;
    QLabel *m_caption = nullptr;
};

// 悬浮球 hover / 拖拽 300ms 后弹出的"数据中转站"面板。
// - 顶部一条小标题区（左：数据中转站，右：条目数）
// - 打开即自动记录当前剪贴板内容，并把剪贴板变化持续收进中转站（含历史持久化）
// - 底部一条紧凑工具栏：左侧切换展示布局（图标 / 列表 / 详细），右侧"铅笔+加号"新增记事
// - 条目悬停可预览图片 / 文本 / 文件；右键可复制 / 打开 / 删除
class PopDock : public QWidget
{
    Q_OBJECT
public:
    // 面板首选尺寸。屏幕装不下时（小屏/投影）Widget 会调 setFixedSize 收缩，
    // 所以这里用常量暴露出来，避免在别处硬编码。
    static constexpr int kPreferredWidth  = 330;
    static constexpr int kPreferredHeight = 420;

    explicit PopDock(QWidget *parent = nullptr);
    ~PopDock() override;

    // 供悬浮球调用：把拖入 / 粘贴的内容塞进中转站
    void addFiles(const QStringList &paths);
    void addImage(const QImage &image);
    void addText(const QString &text);

    // 展示布局（供 Widget 按配置回填；用户点按钮切换时发 viewStyleChanged）
    void setViewStyle(int style);
    int  viewStyle() const;

    // 剪贴板历史库（默认 ~/.popball2_clipboard.db，可用 POPBALL2_DB 覆盖 —— 自检隔离用）
    ClipStore *store() const { return m_store; }

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

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

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
    void hidePreview();                       // 收起悬停预览
    void ensureAnimations();                  // 懒创建划出/划入动画
    void beginInteraction();                  // 交互锁 +1（右键菜单 / 拖拽开始时）
    void endInteraction();                    // 交互锁 -1
    static QIcon noteAddIcon();               // 右下角"铅笔+加号"图标
    static QIcon noteOkIcon();                // 记事输入框右侧的"确认"图标

    TransferStation *m_station    = nullptr;
    QLabel          *m_titleLabel = nullptr;  // 标题区：数据中转站
    QLabel          *m_countLabel = nullptr;  // 标题区右侧：条目数
    QToolButton     *m_addNoteBtn = nullptr;  // 底栏右：新建记事
    QWidget         *m_noteRow    = nullptr;  // 记事编辑行（输入框 + 确认按钮）
    QLineEdit       *m_noteEdit   = nullptr;  // 新建记事时的输入框
    QToolButton     *m_noteOkBtn  = nullptr;  // 确认保存（中文输入法下回车常被用于确认候选词）
    PreviewPopup    *m_preview    = nullptr;  // 悬停预览气泡（懒创建）
    QToolButton     *m_viewBtns[3] = { nullptr, nullptr, nullptr };   // 底栏左：图标/列表/详细

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
