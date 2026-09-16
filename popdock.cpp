#include "popdock.h"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QCryptographicHash>
#include <QByteArray>
#include <QCursor>
#include <QContextMenuEvent>
#include <QDir>
#include <QMimeData>
#include <QMenu>
#include <QDrag>
#include <QUrl>
#include <QFileInfo>
#include <QFileIconProvider>
#include <QDesktopServices>
#include <QStandardPaths>
#include <QDateTime>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QBuffer>
#include <QProcess>
#include <QPixmap>
#include <QPainter>
#include <QPolygonF>
#include <QScreen>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QFont>
#include <QFontMetrics>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListView>
#include <QMouseEvent>
#include <QAbstractItemView>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QShowEvent>
#include <QHideEvent>
#include <QResizeEvent>
#include <QEvent>
#include <QCloseEvent>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QKeySequence>
#include <QTextDocument>
#include <QTextCursor>
#include <QSignalBlocker>

#ifdef POPBALL2_HAVE_QT_MULTIMEDIA
#include <QMediaPlayer>
#include <QVideoSink>
#include <QVideoFrame>
#endif

// 文本条目的展示名：取**第一段非空内容**，太长才截断。
// 不用"整篇压成一行再截 24 字"——那样多行文本的名字会被下一行的开头挤满，很难认。
static QString textLabelFor(const QString &text)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &ln : lines) {
        const QString t = ln.trimmed();
        if (!t.isEmpty())
            return t.size() > 28 ? t.left(28) + QStringLiteral("…") : t;
    }
    return QObject::tr("(空文本)");
}

// ---------------- 小工具：人类可读的文件大小 ----------------
static QString humanSize(qint64 bytes)
{
    const double kb = 1024.0;
    if (bytes < qint64(kb))
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < qint64(kb * kb))
        return QStringLiteral("%1 KB").arg(bytes / kb, 0, 'f', 1);
    if (bytes < qint64(kb * kb * kb))
        return QStringLiteral("%1 MB").arg(bytes / (kb * kb), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(bytes / (kb * kb * kb), 0, 'f', 2);
}

// ---------------- 小工具：把 pixmap 等比缩放后居中画进 rect ----------------
static void drawContain(QPainter *p, const QPixmap &pm, const QRect &rect)
{
    if (pm.isNull() || rect.isEmpty())
        return;
    const QPixmap sc = pm.scaled(rect.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    p->drawPixmap(rect.left() + (rect.width() - sc.width()) / 2,
                  rect.top() + (rect.height() - sc.height()) / 2, sc);
}

// ---------------- 小工具：QImage -> PNG 字节 ----------------
static QByteArray imageToPng(const QImage &img)
{
    if (img.isNull())
        return QByteArray();
    QByteArray ba;
    QBuffer buf(&ba);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return ba;
}

// 带尺寸读图片文件：只解码到需要的大小（几十兆的手机原图整张解码既慢又吃内存）。
// setAutoTransform 让 EXIF 方向生效（手机拍的照片常靠它才能摆正）。
static QImage readImageScaled(const QString &path, const QSize &box)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return QImage();
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize src = reader.size();
    if (src.isValid()) {
        const QSize target = src.scaled(box, Qt::KeepAspectRatio);
        if (target.isValid() && target != src)
            reader.setScaledSize(target);
    }
    return reader.read();
}

// ============================ TsItemDelegate ============================
// 一个委托同时承担三种布局：
//   图标网格 —— 单元格尺寸由 setIconCell() 给（**必须与 gridSize 完全一致**：
//               uniformItemSizes=true 时 Qt 的条目尺寸取自委托 sizeHint，
//               gridSize 只负责"摆放间距"；两者不一致就会出现尺寸/间距互相错位）
//   列表     —— 交给 QStyledItemDelegate 默认绘制（与系统观感一致）
//   详细     —— 自定义两行绘制：图标 + 名称 + 灰色副标题（类型/大小/路径/摘要）
// 只用一个实例、只在构造时安装一次，避免 setItemDelegate() 反复调用带来的
// 旧委托归属/释放问题。
class TsItemDelegate : public QStyledItemDelegate
{
public:
    enum Mode { IconCells, ListRows, DetailRows, PreviewCells };

    explicit TsItemDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void setMode(Mode m) { m_mode = m; }
    Mode mode() const { return m_mode; }

    void setIconCell(const QSize &cell)
    {
        if (cell.isValid() && cell != m_iconCell) {
            m_iconCell = cell;
            m_iconCellSet = true;
        }
    }

    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        switch (m_mode) {
        case IconCells:
            return m_iconCellSet ? m_iconCell : QSize(74, 88);
        case PreviewCells:
            return m_iconCellSet ? m_iconCell : QSize(148, 148);
        case DetailRows:
            return QSize(180, 48);
        case ListRows:
            break;
        }
        return QStyledItemDelegate::sizeHint(opt, idx);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        switch (m_mode) {
        case IconCells:
        case ListRows:
            QStyledItemDelegate::paint(p, opt, idx);
            return;
        case DetailRows:
            paintDetail(p, opt, idx);
            return;
        case PreviewCells:
            paintPreview(p, opt, idx);
            return;
        }
    }

private:
    // 详细：两行（加粗名称 + 灰色副标题）
    void paintDetail(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        const QRect r = opt.rect.adjusted(1, 1, -1, -1);

        // 选中 / 悬停底色
        if (opt.state & QStyle::State_Selected) {
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(58, 110, 165, 200));
            p->drawRoundedRect(r, 7, 7);
        } else if (opt.state & QStyle::State_MouseOver) {
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(255, 255, 255, 16));
            p->drawRoundedRect(r, 7, 7);
        }

        // 图标
        const QIcon ic = qvariant_cast<QIcon>(idx.data(Qt::DecorationRole));
        const QPixmap pm = ic.pixmap(QSize(32, 32));
        const QRect ir(r.left() + 6, r.top() + (r.height() - 32) / 2, 32, 32);
        if (!pm.isNull())
            p->drawPixmap(ir, pm);

        const QVariantMap d = idx.data(Qt::UserRole).toMap();
        const QString sub = d.value(QStringLiteral("subtitle")).toString();

        const int tx = ir.right() + 10;
        const int tw = qMax(24, r.right() - tx - 6);

        QFont f = opt.font;
        f.setPixelSize(13);
        f.setBold(true);
        p->setFont(f);
        p->setPen(QColor(0xe8, 0xea, 0xed));
        const QFontMetrics fm(f);
        p->drawText(QRect(tx, r.top() + 6, tw, 16), Qt::AlignLeft | Qt::AlignVCenter,
                    fm.elidedText(idx.data(Qt::DisplayRole).toString(), Qt::ElideMiddle, tw));

        QFont f2 = opt.font;
        f2.setPixelSize(11);
        p->setFont(f2);
        p->setPen(QColor(0x9a, 0xa0, 0xa6));
        const QFontMetrics fm2(f2);
        p->drawText(QRect(tx, r.top() + 24, tw, 15), Qt::AlignLeft | Qt::AlignVCenter,
                    fm2.elidedText(sub, Qt::ElideMiddle, tw));

        p->restore();
    }

    // 预览：只有图标 —— 文本渲染成"一页纸"，图片/视频给大图缩略，其它文件放大系统图标
    void paintPreview(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);

        const QRect card = opt.rect.adjusted(3, 3, -3, -3);
        QColor bg(255, 255, 255, 12);
        if (opt.state & QStyle::State_Selected)
            bg = QColor(58, 110, 165, 190);
        else if (opt.state & QStyle::State_MouseOver)
            bg = QColor(255, 255, 255, 28);
        p->setPen(Qt::NoPen);
        p->setBrush(bg);
        p->drawRoundedRect(card, 9, 9);

        const QVariantMap d = idx.data(Qt::UserRole).toMap();
        const TsType type = TsType(d.value(QStringLiteral("type")).toInt());
        const QString key  = d.value(QStringLiteral("dedup")).toString();
        const qreal dpr    = opt.widget ? opt.widget->devicePixelRatioF() : 1.0;
        const QRect inner  = card.adjusted(5, 5, -5, -5);

        switch (type) {
        case TsType::Image: {
            const QPixmap pm = imageThumb(key, d.value(QStringLiteral("path")).toString(),
                                          inner.size(), dpr);
            if (!pm.isNull())
                drawContain(p, pm, inner);
            break;
        }
        case TsType::Text: {
            const QPixmap pm = docThumb(key, d.value(QStringLiteral("text")).toString(),
                                        inner.size(), dpr);
            if (!pm.isNull())
                drawContain(p, pm, inner);
            break;
        }
        case TsType::Video:
        case TsType::File: {
            // 图片文件（.jpg/.png…）直接给**真实缩略图** —— 否则只剩一枚"JPEG 文档"
            // 图标，完全看不出内容（用户反馈的正是这个）。
            const QString path = d.value(QStringLiteral("path")).toString();
            QPixmap pm;
            if (TransferStation::isImageFile(path))
                pm = imageThumb(key, path, inner.size(), dpr);
            if (pm.isNull()) {
                // 视频条目的图标里已经带了播放按钮（见 TransferStation::videoIcon）；
                // 其它文件放大系统图标 —— 都用"取大号图标再等比缩放"保证清晰。
                const QIcon ic = qvariant_cast<QIcon>(idx.data(Qt::DecorationRole));
                const int want = qMax(64, qMin(inner.width(), inner.height()));
                pm = ic.pixmap(QSize(want * dpr, want * dpr));
                if (pm.isNull())
                    pm = ic.pixmap(want, want);
            }
            if (!pm.isNull()) {
                pm.setDevicePixelRatio(dpr);
                drawContain(p, pm, inner);
            }
            break;
        }
        }
        p->restore();
    }

    // 图片缩略图：带 scaledSize 读文件，避免把几十兆的原图整张解码
    QPixmap imageThumb(const QString &key, const QString &path,
                       const QSize &box, qreal dpr) const
    {
        if (path.isEmpty() || !QFileInfo::exists(path))
            return QPixmap();
        const QSize want(qMax(32, int(box.width() * dpr)), qMax(32, int(box.height() * dpr)));
        const QString ck = QStringLiteral("img:%1@%2x%3").arg(key).arg(want.width()).arg(want.height());
        auto it = m_cache.constFind(ck);
        if (it != m_cache.constEnd())
            return it.value();

        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QSize src = reader.size();
        if (src.isValid()) {
            QSize target = src.scaled(want, Qt::KeepAspectRatio);
            reader.setScaledSize(target);
        }
        const QImage img = reader.read();
        const QPixmap pm = img.isNull() ? QPixmap() : QPixmap::fromImage(img);
        cachePut(ck, pm);
        return pm;
    }

    // 文本缩略图：像 macOS Quick Look 那样把内容渲染成"一页纸"
    QPixmap docThumb(const QString &key, const QString &text,
                     const QSize &box, qreal dpr) const
    {
        if (text.trimmed().isEmpty())
            return QPixmap();
        const QSize want(qMax(48, int(box.width() * dpr)), qMax(48, int(box.height() * dpr)));
        const QString ck = QStringLiteral("doc:%1@%2x%3").arg(key).arg(want.width()).arg(want.height());
        auto it = m_cache.constFind(ck);
        if (it != m_cache.constEnd())
            return it.value();

        // 先按 3 倍尺寸画一页纸（真实文字），再缩放下来 —— 这样才像"文档预览"而不是几条灰线
        const int W = 360;
        const int H = qMax(W, int(W * 1.32));
        QImage page(W, H, QImage::Format_ARGB32_Premultiplied);
        page.fill(Qt::transparent);
        {
            QPainter pp(&page);
            pp.setRenderHint(QPainter::Antialiasing, true);
            pp.setPen(Qt::NoPen);
            pp.setBrush(QColor(0xf6, 0xf6, 0xf3));          // 纸
            pp.drawRoundedRect(QRectF(0, 0, W, H), 10, 10);
            pp.setBrush(QColor(0xd6, 0xdb, 0xe0));          // 标题条
            pp.drawRoundedRect(QRectF(20, 20, W * 0.52, 14), 4, 4);
            pp.setBrush(QColor(0xe4, 0xe8, 0xec));          // 副标题条
            pp.drawRoundedRect(QRectF(20, 42, W * 0.32, 9), 3, 3);

            QFont f = pp.font();
            f.setPixelSize(13);
            pp.setFont(f);
            pp.setPen(QColor(0x3a, 0x40, 0x48));
            QString body = text;
            if (body.size() > 1400)
                body = body.left(1400);
            pp.drawText(QRect(20, 66, W - 40, H - 86),
                        Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, body);
            pp.end();
        }
        const QPixmap pm = QPixmap::fromImage(
            page.scaled(want, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        cachePut(ck, pm);
        return pm;
    }

    void cachePut(const QString &key, const QPixmap &pm) const
    {
        if (m_cache.size() > 240)          // 防止面板宽度来回变时无限堆积
            m_cache.clear();
        m_cache.insert(key, pm);
    }

    Mode   m_mode       = IconCells;
    QSize  m_iconCell;
    bool   m_iconCellSet = false;
    // 缩略图缓存：paint 是 const 的，所以这里用 mutable（内容与绘制结果无关的纯缓存）
    mutable QHash<QString, QPixmap> m_cache;
};

// ============================ TransferStation ============================

TransferStation::TransferStation(QWidget *parent)
    : QListWidget(parent)
{
    m_delegate = new TsItemDelegate(this);
    setItemDelegate(m_delegate);

    setMovement(QListView::Snap);
    setResizeMode(QListView::Adjust);
    setWrapping(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setAcceptDrops(true);
    setDragEnabled(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setContextMenuPolicy(Qt::DefaultContextMenu);   // 走 contextMenuEvent

    // 悬停反馈 + 悬停预览都需要鼠标移动事件（列表本身与 viewport 都要打开）
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_Hover, true);
    viewport()->installEventFilter(this);

    m_hoverTimer = new QTimer(this);
    m_hoverTimer->setSingleShot(true);
    m_hoverTimer->setInterval(300);
    connect(m_hoverTimer, &QTimer::timeout, this, [this] {
        if (m_hoverItem != nullptr)
            emit previewRequested(dataOf(m_hoverItem), m_hoverCenter);
    });

    setStyleSheet(QStringLiteral(
        "QListWidget{background:transparent;color:#e8eaed;border:none;outline:none;}"
        "QListWidget::item{color:#e8eaed;border-radius:6px;}"
        "QListWidget::item:hover{background:rgba(255,255,255,0.08);}"
        "QListWidget::item:selected{background:rgba(58,110,165,0.85);color:#ffffff;}"
        "QScrollBar:vertical{background:transparent;width:8px;margin:0;}"
        "QScrollBar::handle:vertical{background:rgba(255,255,255,0.22);"
        "border-radius:4px;min-height:24px;}"
        "QScrollBar::handle:vertical:hover{background:rgba(255,255,255,0.34);}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}"));

    setViewStyle(IconView);
}

// 网格单元格：按当前布局决定列数（图标 4 列 / 预览 2 列）与尺寸。
// 单元格宽度按当前控件宽度算（预留滚动条），并同步给委托 sizeHint —— 两者必须一致。
void TransferStation::updateIconGrid()
{
    if (m_viewStyle != IconView && m_viewStyle != PreviewView)
        return;

    const bool preview = (m_viewStyle == PreviewView);
    const int  cols    = preview ? int(kPreviewColumns) : int(kIconColumns);

    int w = this->width();
    if (w <= 0 && parentWidget() != nullptr)
        w = parentWidget()->width() - 24;        // 还没布局时按面板宽度估
    if (w <= 0)
        w = 300;

    const int kScrollBarAllow = 10;              // 给垂直滚动条留位置，避免"有滚动条就少一列"
    const int avail = qMax(cols * 52, w - kScrollBarAllow);
    const int cellW = qMax(preview ? 96 : 52, avail / cols);
    const int iconPx = qBound(30, cellW - 26, 54);
    const int cellH = preview ? cellW : iconPx + 40;   // 预览模式用方格（缩略图铺满）

    if (cellW == m_iconCell.width() && cellH == m_iconCell.height() && iconPx == m_iconPx)
        return;

    m_iconCell = QSize(cellW, cellH);
    m_iconPx   = iconPx;
    m_delegate->setIconCell(m_iconCell);
    setIconSize(QSize(iconPx, iconPx));
    setGridSize(m_iconCell);
    doItemsLayout();
}

void TransferStation::resizeEvent(QResizeEvent *e)
{
    QListWidget::resizeEvent(e);
    updateIconGrid();                            // 面板变宽/变窄（小屏收缩）时重算列宽
}

// 切换展示布局：图标（默认）/ 列表 / 详细 / 预览
void TransferStation::setViewStyle(ViewStyle style)
{
    m_viewStyle = style;
    m_delegate->setMode(style == DetailView  ? TsItemDelegate::DetailRows
                        : style == ListView   ? TsItemDelegate::ListRows
                        : style == PreviewView ? TsItemDelegate::PreviewCells
                                              : TsItemDelegate::IconCells);

    switch (style) {
    case IconView:
    case PreviewView: {
        const bool preview = (style == PreviewView);
        setViewMode(QListView::IconMode);
        // IconMode 的自然流向就是 LeftToRight（按行铺满再换行）；
        // 设成 TopToBottom 会变成"先竖着排满一列再换下一列"，不是网格观感。
        setFlow(QListView::LeftToRight);
        setWrapping(true);
        setResizeMode(QListView::Adjust);
        // 等大单元格：uniform=true 时条目尺寸取委托 sizeHint（= updateIconGrid() 设的单元格），
        // gridSize 负责摆放间距，两者取同一组值才不会错位。
        setUniformItemSizes(true);
        setSpacing(0);
        setWordWrap(!preview);          // 预览模式不画文字，关掉换行
        setTextElideMode(Qt::ElideRight);
        updateIconGrid();
        break;
    }
    case ListView:
    case DetailView:
        setViewMode(QListView::ListMode);
        setFlow(QListView::TopToBottom);
        // 关键：ListMode 下 wrapping=true 会把条目"折"成多列，每列宽度被压到
        // sizeHint 宽度（列表里就变成 "测…g" 这种极窄省略）。列表/详细都要关掉换行。
        setWrapping(false);
        setGridSize(QSize());
        setWordWrap(false);
        setTextElideMode(Qt::ElideMiddle);
        setUniformItemSizes(true);
        setIconSize(style == ListView ? QSize(24, 24) : QSize(32, 32));
        setSpacing(2);
        break;
    }
    doItemsLayout();                       // 立刻按新布局重排，避免残留旧网格几何
    emit previewHideRequested();
}

// 详细模式第二行说明：类型 · 大小 · 路径 / 摘要
QString TransferStation::subtitleFor(const QVariantMap &data)
{
    const TsType type = TsType(data.value(QStringLiteral("type")).toInt());
    const QString path = data.value(QStringLiteral("path")).toString();

    if (type == TsType::Video || type == TsType::File) {
        QFileInfo fi(path);
        QString s = (type == TsType::Video) ? QObject::tr("视频") : QObject::tr("文件");
        if (fi.exists())
            s += QStringLiteral(" · ") + humanSize(fi.size());
        const QString dir = QDir::toNativeSeparators(fi.absolutePath());
        if (!dir.isEmpty())
            s += QStringLiteral(" · ") + dir;
        return s;
    }
    if (type == TsType::Image) {
        const QImage img(path);
        QString s = QObject::tr("图片");
        if (!img.isNull())
            s += QStringLiteral(" · %1×%2").arg(img.width()).arg(img.height());
        QFileInfo fi(path);
        if (fi.exists())
            s += QStringLiteral(" · ") + humanSize(fi.size());
        return s;
    }
    const QString t = data.value(QStringLiteral("text")).toString();
    const QString flat = t.simplified();
    return QObject::tr("文本 · %1 字").arg(t.size())
           + (flat.isEmpty() ? QString() : QStringLiteral(" · ") + flat);
}

// tooltip：类型 · 大小 · 摘要（文本再附内容预览），让"图文混合"时也能看清是什么
QString TransferStation::tooltipFor(const QVariantMap &data)
{
    const TsType type = TsType(data.value(QStringLiteral("type")).toInt());
    const QString name = data.value(QStringLiteral("name")).toString();
    QString tip = name.isEmpty() ? subtitleFor(data) : (name + QStringLiteral("\n") + subtitleFor(data));
    if (type == TsType::Text) {
        QString t = data.value(QStringLiteral("text")).toString().trimmed();
        if (t.size() > 300)
            t = t.left(300) + QStringLiteral("…");
        if (!t.isEmpty())
            tip += QStringLiteral("\n\n") + t;
    }
    return tip;
}

// ---------------- 排序：一条规则贯穿所有布局 ----------------
// 每个条目挂一个 rank（最近使用时刻，ms）。列表自上而下 **非递增**，即"最新的在最前"。
// 为什么不写成"新的一定 insertItem(0)"：
//   历史回填是一批"由新到旧"的记录，逐条插到最前会把整批顺序颠倒；而按 rank 插到
//   正确位置则**与传入顺序无关** —— 乱序传入也得到同一结果，于是图标/列表/详细/预览
//   四种布局（同一个 model）看到的顺序天然一致，重启回填后也不会变。
// 只用整数比较，不依赖文件时间/本地时间格式 ⇒ Linux / macOS / Windows 表现一致。
qint64 TransferStation::rankOf(const QListWidgetItem *it)
{
    if (it == nullptr)
        return 0;
    return it->data(Qt::UserRole).toMap().value(QStringLiteral("rank")).toLongLong();
}

void TransferStation::noteRank(qint64 rank)
{
    if (rank > m_lastRank)
        m_lastRank = rank;
}

qint64 TransferStation::nextRank()
{
    // 比"系统当前时间"和"列表里已有的最大 rank"都大 ⇒ 严格递增。
    // 这样即便系统时钟被回拨（休眠唤醒/时区切换/跨平台时间精度不同），
    // 或者同一毫秒内连着来好几条，新条目也一定排在最前。
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_lastRank = qMax(now, m_lastRank + 1);
    return m_lastRank;
}

int TransferStation::orderedRow(qint64 rank) const
{
    // 找到第一条"比它旧"的条目，插在它前面。相同 rank 时返回的位置在所有
    // 同 rank 条目之后 ⇒ 先来的仍在前（稳定），次序因此完全确定。
    int i = 0;
    for (; i < count(); ++i) {
        const QListWidgetItem *it = item(i);
        if (it != nullptr && rankOf(it) < rank)
            break;
    }
    return i;
}

void TransferStation::appendItem(const QIcon &icon, const QString &text,
                                 TsType type, const QVariantMap &data, qint64 rank)
{
    // 关键：这里不能给 item 传 parent。
    // 一旦传了 parent，item 会立刻被追加进列表（view 已绑定），此后
    // QListWidget::insertItem(row, item) 对"已经属于某个 view"的 item 是空操作，
    // 位置就永远定不下来——新条目会一直掉到末尾（用户看到的"没显示出来"）。
    auto *it = new QListWidgetItem(icon, text);
    const qint64 r = (rank >= 0) ? rank : nextRank();     // <0 = 取"当前最新"
    QVariantMap d = data;
    d[QStringLiteral("type")]     = int(type);
    d[QStringLiteral("rank")]     = r;
    d[QStringLiteral("subtitle")] = subtitleFor(d);
    it->setData(Qt::UserRole, d);
    it->setToolTip(tooltipFor(d));
    it->setFlags(it->flags() | Qt::ItemIsDragEnabled);

    const int row = orderedRow(r);
    insertItem(row, it);
    noteRank(r);

    // 只让"最新的那条"滚进可见区：否则列表已滚过时新条目落在视口外，
    // 看起来像"没加进去"。历史回填（插在中间/末尾）不打断你当前的浏览位置。
    if (row == 0)
        scrollToItem(it, QAbstractItemView::PositionAtTop);
    emit stationChanged();
}

// 按内容指纹移除已有条目；返回是否真的移除了（随后新条目会被重新插入到顶部）
bool TransferStation::removeExisting(const QString &dedupKey)
{
    bool removed = false;
    for (int i = count() - 1; i >= 0; --i) {
        QListWidgetItem *it = item(i);
        if (!it)
            continue;
        if (dataOf(it).value(QStringLiteral("dedup")).toString() == dedupKey) {
            QListWidgetItem *old = takeItem(i);
            delete old;
            removed = true;
        }
    }
    return removed;
}

bool TransferStation::hasDedup(const QString &dedupKey) const
{
    for (int i = 0; i < count(); ++i) {
        QListWidgetItem *it = item(i);
        if (it && dataOf(it).value(QStringLiteral("dedup")).toString() == dedupKey)
            return true;
    }
    return false;
}

QString TransferStation::imageHash(const QImage &img)
{
    // 归一化到小尺寸再取 MD5，避免不同封装/格式造成的不一致，同时控制开销
    const QImage small = img.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_RGB32);
    const QByteArray ba(reinterpret_cast<const char *>(small.constBits()),
                        small.sizeInBytes());
    return QString::fromLatin1(
        QCryptographicHash::hash(ba, QCryptographicHash::Md5).toHex());
}

void TransferStation::addFileItem(const QString &path)
{
    addFileItemEx(path, -1);
}

// 一批文件按给定顺序成组入列。每条都会"提到最前"，所以倒着加：
// 复制 A,B,C ⇒ 面板里就是 A,B,C（不做的话会变成 C,B,A，预览布局里看着尤其乱）。
// 库里的 used_at 也按同一顺序递增 ⇒ 重启回填后顺序仍然一致。
void TransferStation::addFileItems(const QStringList &paths)
{
    for (int i = paths.size() - 1; i >= 0; --i)
        addFileItem(paths.at(i));
}

void TransferStation::addFileItemEx(const QString &path, qint64 dbId, qint64 rank)
{
    QFileInfo fi(path);
    if (!fi.exists())
        return;

    // 视频文件单独成一类：图标带播放按钮，预览模式给封面帧
    const bool   video = isVideoFile(path);
    const TsType type  = video ? TsType::Video : TsType::File;

    QIcon icon;
    if (video) {
        icon = videoIcon(QPixmap());            // 先胶片占位，封面帧异步回来再换
    } else if (isImageFile(path)) {
        // 图片文件给**真实缩略图**，不要系统那枚"JPEG 文档"图标
        // （用户反馈：预览布局里 .jpg 应该看到图，而不是一个 JPEG 图标）
        const QPixmap pm = imageFileThumb(path);
        if (!pm.isNull())
            icon = QIcon(pm);
    }
    if (icon.isNull()) {
        QFileIconProvider prov;
        icon = prov.icon(fi);
        if (icon.isNull())
            icon = prov.icon(QFileIconProvider::File);
    }

    const QString key = QStringLiteral("f:") + QDir::cleanPath(path);
    const bool fromHistory = (dbId >= 0);
    removeExisting(key);                 // 同路径文件不重复，提升到顶部

    // 先入库拿到行 id，再插列表：条目挂着 dbId，删除时才能同步删库
    qint64 rowId = dbId;
    if (!fromHistory && m_store != nullptr)
        rowId = m_store->put(ClipStore::FileKind, key, fi.fileName(), QString(),
                             path, QByteArray(), fi.size());

    QVariantMap d;
    d[QStringLiteral("path")]  = path;
    d[QStringLiteral("name")]  = fi.fileName();
    d[QStringLiteral("dedup")] = key;
    if (rowId > 0)
        d[QStringLiteral("dbId")] = rowId;
    appendItem(icon, fi.fileName(), type, d, rank);

    if (video)
        requestVideoThumb(key, path);
}

void TransferStation::addImageItem(const QImage &image, const QString &name)
{
    addImageItemEx(image, name, -1);
}

void TransferStation::addImageItemEx(const QImage &image, const QString &name, qint64 dbId,
                                     qint64 rank)
{
    if (image.isNull())
        return;

    const QString key = QStringLiteral("i:") + imageHash(image);
    const bool fromHistory = (dbId >= 0);
    removeExisting(key);                 // 相同图片去重，提升到顶部

    const QString p = saveImageTemp(image);
    const QPixmap pm = QPixmap::fromImage(image).scaled(
        88, 88, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QString label = name.isEmpty() ? QFileInfo(p).fileName() : name;

    // 先入库拿到行 id，再插列表（图片存 PNG 二进制，不依赖临时文件）
    qint64 rowId = dbId;
    if (!fromHistory && m_store != nullptr) {
        const QByteArray png = imageToPng(image);
        if (!png.isEmpty())
            rowId = m_store->put(ClipStore::ImageKind, key, label, QString(),
                                 QString(), png, png.size());
    }

    QVariantMap d;
    d[QStringLiteral("path")]  = p;
    d[QStringLiteral("name")]  = label;
    d[QStringLiteral("dedup")] = key;
    if (rowId > 0)
        d[QStringLiteral("dbId")] = rowId;
    appendItem(QIcon(pm), label, TsType::Image, d, rank);
}

void TransferStation::addTextItem(const QString &text, const QString &label)
{
    addTextItemEx(text, label, -1);
}

void TransferStation::addTextItemEx(const QString &text, const QString &label, qint64 dbId,
                                    qint64 rank)
{
    if (text.isEmpty())
        return;
    const QString l     = label.isEmpty() ? textLabelFor(text) : label;
    const QString key   = QStringLiteral("t:") + text;
    const bool fromHistory = (dbId >= 0);
    removeExisting(key);                 // 相同文本去重，提升到顶部

    // 先入库拿到行 id，再插列表：条目挂着 dbId，删除时才能同步删库
    qint64 rowId = dbId;
    if (!fromHistory && m_store != nullptr)
        rowId = m_store->put(ClipStore::TextKind, key, l, text, QString(),
                             QByteArray(), text.toUtf8().size());

    QVariantMap d;
    d[QStringLiteral("text")]  = text;
    d[QStringLiteral("name")]  = l;
    d[QStringLiteral("dedup")] = key;
    if (rowId > 0)
        d[QStringLiteral("dbId")] = rowId;
    appendItem(QIcon(textIcon()), l, TsType::Text, d, rank);
}

// 历史回填：每条按自己的 usedAt 插到正确位置（不是无脑追加到末尾）。
// 好处：**与传入顺序无关** —— 无论库里怎么给、给了几批，最终次序都一样，
// 与库里 `recent()` 的排序（used_at DESC, id DESC）完全对应，重启前后一致。
void TransferStation::loadRecords(const QVector<ClipRecord> &records)
{
    for (const ClipRecord &r : records) {
        if (hasDedup(r.hash))
            continue;
        const qint64 rank = qMax<qint64>(1, r.usedAt);   // 老库里 usedAt=0 的历史排到最旧
        switch (r.kind) {
        case ClipStore::TextKind:
            if (!r.text.isEmpty())
                addTextItemEx(r.text, r.title, r.id, rank);
            break;
        case ClipStore::FileKind:
            if (!r.path.isEmpty() && QFileInfo::exists(r.path))
                addFileItemEx(r.path, r.id, rank);
            break;
        case ClipStore::ImageKind: {
            const QByteArray png = (m_store != nullptr) ? m_store->pngOf(r.id) : QByteArray();
            const QImage img = QImage::fromData(png, "PNG");
            if (!img.isNull())
                addImageItemEx(img, r.title, r.id, rank);
            break;
        }
        default:
            break;
        }
    }
}

void TransferStation::pasteClipboard()
{
    QClipboard *cb = QApplication::clipboard();
    const QMimeData *m = cb ? cb->mimeData() : nullptr;
    if (!m)
        return;
    if (m->hasImage()) {
        const QImage img = qvariant_cast<QImage>(m->imageData());
        if (!img.isNull())
            addImageItem(img);
    } else if (m->hasText()) {
        addTextItem(m->text());
    }
}

QString TransferStation::saveImageTemp(const QImage &img)
{
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/popball2";
    QDir().mkpath(dir);
    const QString p = dir + "/img_" +
                      QString::number(QDateTime::currentMSecsSinceEpoch()) + ".png";
    img.save(p, "PNG");
    return p;
}

// ---------------- 扩展名分类（视频要单独显示播放标识） ----------------
bool TransferStation::isVideoFile(const QString &path)
{
    static const QSet<QString> exts = {
        QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mov"),
        QStringLiteral("mkv"), QStringLiteral("webm"), QStringLiteral("avi"),
        QStringLiteral("wmv"), QStringLiteral("flv"), QStringLiteral("mpg"),
        QStringLiteral("mpeg"), QStringLiteral("3gp"), QStringLiteral("ts"),
        QStringLiteral("m2ts"), QStringLiteral("rmvb"), QStringLiteral("ogv")
    };
    return exts.contains(QFileInfo(path).suffix().toLower());
}

bool TransferStation::isImageFile(const QString &path)
{
    static const QSet<QString> exts = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("gif"), QStringLiteral("bmp"), QStringLiteral("webp"),
        QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("heic"),
        QStringLiteral("ico"), QStringLiteral("svg")
    };
    return exts.contains(QFileInfo(path).suffix().toLower());
}

QPixmap TransferStation::videoThumbFor(const QString &dedupKey) const
{
    return m_videoThumbs.value(dedupKey);
}

// 视频条目图标：封面帧（或胶片占位图）+ 居中播放按钮。
// 播放按钮直接烘进图标里，这样图标/列表/详细/预览 四种布局都能看到"这是个视频"。
QPixmap TransferStation::imageFileThumb(const QString &path)
{
    if (path.isEmpty())
        return QPixmap();
    const auto cached = m_imageFileThumbs.constFind(path);
    if (cached != m_imageFileThumbs.constEnd())
        return cached.value();

    // 图标布局最大也就 48px（高 dpi 96），128 足够清晰；预览布局不走这里，
    // 由委托按单元格实际大小现取（见 TsItemDelegate::imageThumb）。
    constexpr int kThumbPx = 128;
    const QImage img = readImageScaled(path, QSize(kThumbPx, kThumbPx));
    if (img.isNull())
        return QPixmap();          // 读不了（缺插件的 svg/heic、损坏文件）→ 调用方退回系统图标

    const QPixmap pm = QPixmap::fromImage(img);
    if (m_imageFileThumbs.size() > 200)     // 与委托里的缓存同一策略：超了整体清掉
        m_imageFileThumbs.clear();
    m_imageFileThumbs.insert(path, pm);
    return pm;
}

QIcon TransferStation::videoIcon(const QPixmap &frame)
{
    const int S = 128;
    QPixmap pm(S, S);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRect card(2, 2, S - 4, S - 4);
    p.setPen(Qt::NoPen);
    QLinearGradient g(card.topLeft(), card.bottomRight());
    g.setColorAt(0, QColor(58, 64, 74));
    g.setColorAt(1, QColor(28, 32, 38));
    p.setBrush(g);
    p.drawRoundedRect(card, 10, 10);

    if (!frame.isNull()) {
        const QPixmap sc = frame.scaled(card.size(), Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation);
        p.drawPixmap(card.center().x() - sc.width() / 2,
                     card.center().y() - sc.height() / 2, sc);
        p.setBrush(QColor(0, 0, 0, 80));            // 压暗一点，让播放按钮更醒目
        p.drawRoundedRect(card, 10, 10);
    } else {
        // 胶片感占位：几条竖带
        p.setBrush(QColor(255, 255, 255, 24));
        for (int i = 0; i < 3; ++i)
            p.drawRoundedRect(QRect(card.left() + 12 + i * 34, card.top() + 14,
                                    24, card.height() - 28), 3, 3);
    }

    // 居中播放按钮
    const QPoint c = card.center();
    p.setBrush(QColor(0, 0, 0, 130));
    p.setPen(QPen(QColor(255, 255, 255, 225), 2.4));
    p.drawEllipse(c, 24, 24);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 240));
    QPolygonF tri;
    tri << QPointF(c.x() - 8, c.y() - 12) << QPointF(c.x() - 8, c.y() + 12)
        << QPointF(c.x() + 13, c.y());
    p.drawPolygon(tri);
    return QIcon(pm);
}

// 异步生成视频封面帧。两条路：
//   1) 有 ffmpeg 就用它抽一帧（纯命令行、跨平台、可自动化验证）；
//   2) 没有 ffmpeg 时，macOS 退回 Quick Look（qlmanage -t，效果同 Finder 里的预览，
//      顺带也支持 PDF/文档，但需要 QuickLook 服务，某些受限环境下跑不了）。
// 两条都不可用就保持胶片占位图（不反复尝试）。
void TransferStation::requestVideoThumb(const QString &dedupKey, const QString &path)
{
    if (dedupKey.isEmpty() || path.isEmpty())
        return;
    if (m_videoThumbs.contains(dedupKey) || m_videoThumbPending.contains(dedupKey)
        || m_videoThumbFailed.contains(dedupKey))
        return;

    const QString outDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                           + QStringLiteral("/popball2_thumbs");
    QDir().mkpath(outDir);

    QString expected;
    QString program;
    QStringList args;

    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!ffmpeg.isEmpty()) {
        expected = outDir + QLatin1Char('/') + QString::number(qHash(dedupKey))
                   + QStringLiteral(".png");
        program = ffmpeg;
        args = { QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-y"),
                 QStringLiteral("-ss"), QStringLiteral("1"), QStringLiteral("-i"), path,
                 QStringLiteral("-frames:v"), QStringLiteral("1"),
                 QStringLiteral("-vf"), QStringLiteral("scale=512:-1"), expected };
    }
#if defined(Q_OS_MACOS)
    else if (!QStandardPaths::findExecutable(QStringLiteral("qlmanage")).isEmpty()) {
        // qlmanage 的输出名 = 原文件名 + ".png"
        expected = outDir + QLatin1Char('/') + QFileInfo(path).fileName()
                   + QStringLiteral(".png");
        program = QStringLiteral("qlmanage");
        args = { QStringLiteral("-t"), QStringLiteral("-s"), QStringLiteral("512"),
                 QStringLiteral("-o"), outDir, path };
    }
#endif
    else {
        m_videoThumbFailed.insert(dedupKey);     // 没有可用的抽帧工具
        return;
    }
    QFile::remove(expected);

    m_videoThumbPending.insert(dedupKey);
    auto *proc = new QProcess(this);
    connect(proc, &QProcess::finished, this,
            [this, proc, dedupKey, expected](int code, QProcess::ExitStatus st) {
                proc->deleteLater();
                m_videoThumbPending.remove(dedupKey);

                QPixmap thumb;
                if (code == 0 && st == QProcess::NormalExit && QFileInfo::exists(expected))
                    thumb = QPixmap(expected);
                if (thumb.isNull()) {
                    m_videoThumbFailed.insert(dedupKey);
                    return;
                }
                m_videoThumbs.insert(dedupKey, thumb);

                // 回填条目图标（同一内容只有一条，去重保证）
                for (int i = 0; i < count(); ++i) {
                    QListWidgetItem *it = item(i);
                    if (it != nullptr
                        && dataOf(it).value(QStringLiteral("dedup")).toString() == dedupKey) {
                        it->setIcon(videoIcon(thumb));
                    }
                }
                viewport()->update();
            });
    // 超时保护：卡住的进程直接杀掉（不成功就永久保持占位图）
    QTimer::singleShot(8000, proc, [proc] {
        if (proc->state() != QProcess::NotRunning)
            proc->kill();
    });
    proc->start(program, args);
}

QPixmap TransferStation::textIcon()
{
    QPixmap pm(88, 88);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor(58, 110, 165));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(8, 8, 72, 72, 10, 10);
    p.setPen(Qt::white);
    p.setFont(QFont(QString(), 36, QFont::Bold));
    p.drawText(pm.rect(), Qt::AlignCenter, QStringLiteral("T"));
    return pm;
}

QVariantMap TransferStation::dataOf(QListWidgetItem *it)
{
    if (it == nullptr)
        return QVariantMap();
    const QVariant v = it->data(Qt::UserRole);
    return v.isValid() ? v.toMap() : QVariantMap();
}

// 悬停条目变化：换条目先收起旧预览并重新计时；离开条目立即收起
void TransferStation::updateHover(QListWidgetItem *item, const QPoint &globalCenter)
{
    if (item != nullptr)
        m_hoverCenter = globalCenter;

    if (item == m_hoverItem)
        return;

    m_hoverItem = item;
    if (item == nullptr) {
        m_hoverTimer->stop();
        emit previewHideRequested();
    } else {
        emit previewHideRequested();       // 换条目：先收起，300ms 后再弹新的
        m_hoverTimer->start();
    }
}

bool TransferStation::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == viewport()) {
        switch (event->type()) {
        case QEvent::MouseMove: {
            auto *me = static_cast<QMouseEvent *>(event);
            QListWidgetItem *it = itemAt(me->position().toPoint());
            const QPoint gc = (it != nullptr)
                                  ? viewport()->mapToGlobal(visualItemRect(it).center())
                                  : QPoint();
            updateHover(it, gc);
            break;
        }
        case QEvent::Leave:
        case QEvent::Wheel:
        case QEvent::MouseButtonPress:
            updateHover(nullptr, QPoint());
            break;
        default:
            break;
        }
    }
    return QListWidget::eventFilter(watched, event);
}

void TransferStation::fillItemMenu(QMenu &menu)
{
    menu.setStyleSheet(QStringLiteral(
        "QMenu{background:#262a31;color:#e8eaed;border:1px solid #4a505c;"
        "border-radius:8px;padding:4px;}"
        "QMenu::item{padding:5px 18px;border-radius:5px;}"
        "QMenu::item:selected{background:rgba(58,110,165,0.9);color:#ffffff;}"
        "QMenu::separator{height:1px;background:rgba(255,255,255,0.12);margin:4px 6px;}"));
    menu.addAction(tr("复制"));
    menu.addAction(tr("打开"));
    menu.addSeparator();
    menu.addAction(tr("删除"));
}

void TransferStation::contextMenuEvent(QContextMenuEvent *event)
{
    QListWidgetItem *it = itemAt(event->pos());
    if (it == nullptr) {
        event->ignore();
        return;
    }
    setCurrentItem(it);

    QMenu menu(this);
    fillItemMenu(menu);
    QAction *actCopy = menu.actions().value(0);
    QAction *actOpen = menu.actions().value(1);
    QAction *actDel  = menu.actions().value(3);      // 中间有一个分隔符

    // 菜单是独立弹出窗口：它会抢走鼠标，导致面板收到 Leave。这里先加交互锁
    // （PopDock 会因此不请求收起），菜单关闭后再解锁 —— 否则右键时面板会自己跑掉。
    emit interactionBegin();
    QAction *chosen = menu.exec(event->globalPos());
    emit interactionEnd();

    if (chosen == actCopy)
        copyItemToClipboard(it);
    else if (chosen == actOpen)
        openItem(it);
    else if (chosen == actDel)
        removeItem(it);
    event->accept();
}

void TransferStation::copyItemToClipboard(QListWidgetItem *it)
{
    if (it == nullptr)
        return;
    const QVariantMap d = dataOf(it);
    const TsType type = TsType(d.value(QStringLiteral("type")).toInt());
    QClipboard *cb = QApplication::clipboard();
    if (cb == nullptr)
        return;

    if (type == TsType::Text) {
        cb->setText(d.value(QStringLiteral("text")).toString());
        return;
    }

    const QString path = d.value(QStringLiteral("path")).toString();
    if (path.isEmpty())
        return;

    // 图片：既给图像（粘进编辑器）也给文件 URL（粘进文件管理器）
    // 文件：URL + 文本路径（粘到终端/编辑器里也能用）
    auto *mime = new QMimeData;
    mime->setUrls({ QUrl::fromLocalFile(path) });
    mime->setText(QDir::toNativeSeparators(path));
    if (type == TsType::Image) {
        const QImage img(path);
        if (!img.isNull())
            mime->setImageData(img);
    }
    cb->setMimeData(mime);
}

void TransferStation::openItem(QListWidgetItem *it)
{
    if (it == nullptr)
        return;
    const QVariantMap d = dataOf(it);
    const TsType type = TsType(d.value(QStringLiteral("type")).toInt());
    if (type == TsType::Text) {
        // 文本：弹出专门的小编辑器（可看可改），而不是只把内容塞回剪贴板
        emit editTextRequested(d.value(QStringLiteral("dbId")).toLongLong(),
                               d.value(QStringLiteral("dedup")).toString(),
                               d.value(QStringLiteral("name")).toString(),
                               d.value(QStringLiteral("text")).toString());
        return;
    }
    const QString p = d.value(QStringLiteral("path")).toString();
    if (!p.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(p));
}

QListWidgetItem *TransferStation::findTextItem(qint64 dbId, const QString &originalKey) const
{
    // 条目可能在编辑器开着的时候被删掉/被"重新复制"提升替换，所以**不存裸指针**，
    // 每次按身份（库里行 id，退化时用载入时的 dedup 键）重新找。
    for (int i = 0; i < count(); ++i) {
        QListWidgetItem *it = item(i);
        if (it == nullptr)
            continue;
        const QVariantMap d = dataOf(it);
        if (int(TsType(d.value(QStringLiteral("type")).toInt())) != int(TsType::Text))
            continue;
        if (dbId > 0) {
            if (d.value(QStringLiteral("dbId")).toLongLong() == dbId)
                return it;
        } else if (!originalKey.isEmpty()
                   && d.value(QStringLiteral("dedup")).toString() == originalKey) {
            return it;
        }
    }
    return nullptr;
}

bool TransferStation::applyTextEdit(qint64 dbId, const QString &originalKey, const QString &text)
{
    QListWidgetItem *it = findTextItem(dbId, originalKey);
    if (it == nullptr)
        return false;                 // 条目已被删除：不写库、也不复活它

    const QString label = textLabelFor(text);
    const QString key   = QStringLiteral("t:") + text;

    // 改成了与另一条重复的内容：按去重语义合并（删掉那条），与"重新复制"的行为一致
    for (int i = count() - 1; i >= 0; --i) {
        QListWidgetItem *other = item(i);
        if (other == nullptr || other == it)
            continue;
        if (dataOf(other).value(QStringLiteral("dedup")).toString() == key) {
            const qint64 otherId = dataOf(other).value(QStringLiteral("dbId")).toLongLong();
            if (otherId > 0 && m_store != nullptr)
                m_store->remove(otherId);
            delete takeItem(i);
        }
    }

    QVariantMap d = dataOf(it);
    d[QStringLiteral("text")]     = text;
    d[QStringLiteral("name")]     = label;
    d[QStringLiteral("dedup")]    = key;
    d[QStringLiteral("subtitle")] = subtitleFor(d);
    it->setData(Qt::UserRole, d);
    it->setText(label);
    it->setToolTip(tooltipFor(d));

    const qint64 rowId = d.value(QStringLiteral("dbId")).toLongLong();
    if (rowId > 0 && m_store != nullptr)
        m_store->updateText(rowId, key, label, text);   // 就地改库：id / 顺序都不变

    emit stationChanged();
    viewport()->update();             // 详细/预览布局要重绘（摘要、缩略图都变了）
    return true;
}

void TransferStation::removeItem(QListWidgetItem *it)
{
    if (it == nullptr)
        return;
    const QVariantMap d = dataOf(it);
    const qint64 dbId = d.value(QStringLiteral("dbId")).toLongLong();
    if (dbId > 0 && m_store != nullptr)
        m_store->remove(dbId);           // 历史库里的也一起删掉
    delete takeItem(row(it));
    emit stationChanged();
}

void TransferStation::dragEnterEvent(QDragEnterEvent *e)
{
    const QMimeData *m = e->mimeData();
    if (m && (m->hasUrls() || m->hasImage() || m->hasText()))
        e->acceptProposedAction();
    else
        e->ignore();
}

void TransferStation::dragMoveEvent(QDragMoveEvent *e)
{
    const QMimeData *m = e->mimeData();
    if (m && (m->hasUrls() || m->hasImage() || m->hasText()))
        e->acceptProposedAction();
    else
        e->ignore();
}

void TransferStation::dropEvent(QDropEvent *e)
{
    const QMimeData *m = e->mimeData();
    if (!m) { e->ignore(); return; }
    if (m->hasUrls()) {
        QStringList files;
        for (const QUrl &u : m->urls())
            if (u.isLocalFile())
                files << u.toLocalFile();
        addFileItems(files);                  // 保持拖入时的原始顺序
        e->acceptProposedAction();
    } else if (m->hasImage()) {
        const QImage img = qvariant_cast<QImage>(m->imageData());
        if (!img.isNull())
            addImageItem(img);
        e->acceptProposedAction();
    } else if (m->hasText()) {
        addTextItem(m->text());
        e->acceptProposedAction();
    } else {
        e->ignore();
    }
}

void TransferStation::keyPressEvent(QKeyEvent *e)
{
    // 粘贴剪贴板图像 / 文本
    if ((e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) &&
        e->key() == Qt::Key_V) {
        pasteClipboard();
        return;
    }
    // 删除选中项（同时删掉历史库里的记录）
    if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && currentItem()) {
        removeItem(currentItem());
        return;
    }
    QListWidget::keyPressEvent(e);
}

void TransferStation::startDrag(Qt::DropActions /*supportedActions*/)
{
    QListWidgetItem *it = currentItem();
    if (!it)
        return;
    const QVariantMap d = dataOf(it);
    const TsType type = TsType(d[QStringLiteral("type")].toInt());
    auto *m = new QMimeData;
    if (type == TsType::File || type == TsType::Image) {
        const QString p = d[QStringLiteral("path")].toString();
        if (!p.isEmpty()) {
            m->setUrls({ QUrl::fromLocalFile(p) });
            if (type == TsType::Image) {
                const QImage img(p);
                if (!img.isNull())
                    m->setImageData(img);
            }
        }
    } else if (type == TsType::Text) {
        m->setText(d[QStringLiteral("text")].toString());
    }
    auto *drag = new QDrag(this);
    drag->setMimeData(m);
    drag->setPixmap(it->icon().pixmap(48, 48));
    // 拖出期间同样加锁：拖到面板外时光标已离开面板，别让面板在中途收起
    emit interactionBegin();
    drag->exec(Qt::CopyAction);
    emit interactionEnd();
}

// ============================ PreviewPopup ============================

PreviewPopup::PreviewPopup(QWidget *parent)
    : QWidget(parent)
{
    // ToolTip 型窗口：不抢焦点、不激活；再对鼠标透明，绝不干扰面板的进出判定
    setObjectName(QStringLiteral("popDockPreview"));
    setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 10, 10, 8);
    lay->setSpacing(6);

    m_content = new QLabel(this);
    m_content->setAlignment(Qt::AlignCenter);
    m_content->setStyleSheet(QStringLiteral("background:transparent;color:#e8eaed;"));

    m_caption = new QLabel(this);
    m_caption->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_caption->setStyleSheet(QStringLiteral("background:transparent;color:#a8aeb6;"));
    QFont cf = m_caption->font();
    cf.setPixelSize(11);
    m_caption->setFont(cf);

    lay->addWidget(m_content, 0, Qt::AlignCenter);
    lay->addWidget(m_caption, 0);
    m_videoSize = QSize(320, 180);
#ifdef POPBALL2_HAVE_QT_MULTIMEDIA
    m_player = new QMediaPlayer(this);
    m_sink   = new QVideoSink(this);
    m_player->setVideoSink(m_sink);
    m_player->setAudioOutput(nullptr);          // 悬停预览不发声
    connect(m_sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &f) {
        if (f.isValid())
            setFrame(f.toImage());
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString &msg) {
                if (m_caption != nullptr && !msg.isEmpty())
                    m_caption->setToolTip(msg);
            });
#endif
}

void PreviewPopup::showPixmap(const QPixmap &pixmap, const QString &caption)
{
    if (pixmap.isNull()) {
        hide();
        return;
    }
    m_content->setWordWrap(false);
    m_content->setText(QString());
    m_content->setPixmap(pixmap);
    m_content->setFixedSize(pixmap.size());

    const int maxW = 340;
    QFontMetrics fm(m_caption->font());
    m_caption->setText(fm.elidedText(caption, Qt::ElideMiddle, qMax(maxW, pixmap.width())));
    m_caption->setVisible(!caption.isEmpty());

    ensurePolished();
    adjustSize();
}

void PreviewPopup::showText(const QString &text, const QString &caption)
{
    const int maxW = 340;      // 上限：再长就换行
    const int minW = 130;      // 下限：太窄气泡不成形
    const int maxH = 260;      // 上限：超过就截断

    QFont f = m_content->font();
    f.setPixelSize(12);
    m_content->setFont(f);

    bool truncated = false;
    QString t = text;
    if (t.size() > 600) {
        t = t.left(600);
        truncated = true;
    }

    m_content->setPixmap(QPixmap());
    m_content->setWordWrap(true);

    // 短文本（单行放得下）就贴合成一行宽度，不要让气泡白白撑到 340 宽
    const QFontMetrics fmc(f);
    const int oneLineW = fmc.horizontalAdvance(t.simplified()) + 6;
    const int w = qBound(minW, qMin(maxW, oneLineW), maxW);
    m_content->setFixedWidth(w);
    m_content->setText(t);

    int h = m_content->heightForWidth(w);
    if (h <= 0)
        h = 60;
    if (h > maxH) {
        h = maxH;
        truncated = true;
    }
    m_content->setFixedSize(w, h);

    QFontMetrics fm(m_caption->font());
    QString cap = caption;
    if (truncated)
        cap += QStringLiteral(" · 内容过长，已省略");
    m_caption->setText(fm.elidedText(cap, Qt::ElideMiddle, qMax(w, maxW)));
    m_caption->setVisible(!cap.isEmpty());

    ensurePolished();
    adjustSize();
}

// 视频：能播就静音循环播放（画面逐帧画进内容标签），不能播就退化成静态封面。
void PreviewPopup::showVideo(const QString &path, const QString &caption,
                             const QPixmap &fallbackStill)
{
#ifdef POPBALL2_HAVE_QT_MULTIMEDIA
    if (m_player == nullptr || m_sink == nullptr) {   // 理论上不会，兜底用静态图
        if (!fallbackStill.isNull())
            showPixmap(fallbackStill, caption);
        else
            showText(tr("（无法预览此视频）"), caption);
        return;
    }

    m_content->setWordWrap(false);
    m_content->setText(QString());
    const QPixmap first = fallbackStill.isNull()
                              ? QPixmap()
                              : fallbackStill.scaled(m_videoSize, Qt::KeepAspectRatio,
                                                     Qt::SmoothTransformation);
    m_content->setPixmap(first);                      // 先显示封面帧，解码出画面就切过去
    m_content->setFixedSize(m_videoSize);

    QFontMetrics fm(m_caption->font());
    m_caption->setText(fm.elidedText(caption + QStringLiteral(" · 悬停预览（静音）"),
                                     Qt::ElideMiddle, m_videoSize.width() + 40));
    m_caption->setVisible(true);

    m_player->setSource(QUrl::fromLocalFile(path));
    m_player->setLoops(QMediaPlayer::Infinite);
    m_player->play();

    ensurePolished();
    adjustSize();
#else
    Q_UNUSED(path);
    if (!fallbackStill.isNull())
        showPixmap(fallbackStill, caption + QStringLiteral(" · 双击用系统播放器打开"));
    else
        showText(tr("（本机未启用视频预览：双击用系统播放器打开）"), caption);
#endif
}

void PreviewPopup::stopVideo()
{
#ifdef POPBALL2_HAVE_QT_MULTIMEDIA
    if (m_player != nullptr) {
        m_player->stop();
        m_player->setSource(QUrl());
    }
#endif
}

bool PreviewPopup::isPlayingVideo() const
{
#ifdef POPBALL2_HAVE_QT_MULTIMEDIA
    return m_player != nullptr && m_player->playbackState() == QMediaPlayer::PlayingState;
#else
    return false;
#endif
}

void PreviewPopup::setFrame(const QImage &frame)
{
    if (frame.isNull() || m_content == nullptr)
        return;
    const QImage scaled = frame.scaled(m_videoSize, Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation);
    m_content->setPixmap(QPixmap::fromImage(scaled));
}

void PreviewPopup::paintEvent(QPaintEvent *e)
{
    Q_UNUSED(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(QColor(86, 92, 104), 1));
    p.setBrush(QColor(24, 26, 31, 246));
    p.drawRoundedRect(r, 10, 10);
}

// ============================ TextEditorWindow ============================
// 文本条目的"小编辑器"：双击 / 右键「打开」时弹出，直接看全文，也能改。
// 几点取舍：
//   * 独立顶层窗口（有系统标题栏，可拖动/缩放），深色主题与面板保持一致。
//   * 关闭时若有未保存的改动 → **自动保存**。不弹"要保存吗"的模态框：既免得把用户卡住，
//     也让无头自检不必去点对话框（模态框会把 harness 挂死）。
//   * **不缓存 QListWidgetItem 指针**：条目随时可能被删除、或被"重新复制"提升替换掉。
//     每次保存都按 dbId（优先）或载入时的 dedup 键重新定位；找不到就拒绝写入，不复活已删条目。
TextEditorWindow::TextEditorWindow(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("popDockTextEditor"));
    setWindowTitle(tr("文本预览"));
    setWindowFlags(windowFlags() | Qt::Window);        // 顶层窗口
    // QWidget 的子类不会自动画样式表里的背景，得显式声明"我要样式表背景"
    setAttribute(Qt::WA_StyledBackground, true);
    resize(520, 380);
    setMinimumSize(340, 220);
    setStyleSheet(QStringLiteral(
        "#popDockTextEditor{background:#1e2126;}"
        "QLabel{background:transparent;color:#d7dbe0;}"
        "QPlainTextEdit{background:#171a1f;color:#e8eaed;border:1px solid #3a4048;"
        "border-radius:2px;padding:6px;selection-background-color:rgba(58,110,165,0.85);}"
        "QPlainTextEdit:focus{border:1px solid rgba(96,150,205,0.95);}"
        "QToolButton{border:1px solid transparent;border-radius:3px;color:#c8cdd4;"
        "background:rgba(255,255,255,0.07);padding:2px 10px;}"
        "QToolButton:hover{background:rgba(255,255,255,0.15);color:#ffffff;}"
        "QToolButton:disabled{color:#6b7178;background:transparent;}"));

    // 不要标题行：条目名就是正文的第一行，再单独显示一遍纯属占地方（窗口标题栏已写着"文本预览"）。
    // 字数/行数挪到底栏左侧，跟状态挤一行。
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 10);
    root->setSpacing(8);

    // 正文
    m_edit = new QPlainTextEdit(this);
    m_edit->setObjectName(QStringLiteral("textEditorEdit"));
    m_edit->setPlaceholderText(tr("（空文本）"));
    m_edit->setTabChangesFocus(false);
    {
        QFont f = m_edit->font();
        f.setPixelSize(13);
        m_edit->setFont(f);
    }
    root->addWidget(m_edit, 1);

    // 底部：左状态，右 复制 / 保存 / 关闭
    auto *foot = new QHBoxLayout;
    foot->setSpacing(8);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("textEditorStatus"));
    m_status->setStyleSheet(QStringLiteral("background:transparent;color:#8b9199;"));
    {
        QFont f = m_status->font();
        f.setPixelSize(11);
        m_status->setFont(f);
    }
    foot->addWidget(m_status, 1);

    auto makeBtn = [this](const QString &name, const QString &text) {
        auto *b = new QToolButton(this);
        b->setObjectName(name);
        b->setText(text);
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedHeight(26);
        b->setMinimumWidth(56);
        QFont f = b->font();
        f.setPixelSize(11);
        b->setFont(f);
        return b;
    };
    auto *copyBtn  = makeBtn(QStringLiteral("textEditorCopy"),  tr("复制"));
    m_saveBtn      = makeBtn(QStringLiteral("textEditorSave"),  tr("保存"));
    auto *closeBtn = makeBtn(QStringLiteral("textEditorClose"), tr("关闭"));
    foot->addWidget(copyBtn, 0);
    foot->addWidget(m_saveBtn, 0);
    foot->addWidget(closeBtn, 0);
    root->addLayout(foot);

    connect(copyBtn,  &QToolButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_edit->toPlainText());
        m_status->setText(tr("已复制到剪贴板"));
    });
    connect(m_saveBtn, &QToolButton::clicked, this, [this] { save(); });
    connect(closeBtn,  &QToolButton::clicked, this, &TextEditorWindow::close);
    connect(m_edit, &QPlainTextEdit::textChanged, this, [this] { refreshState(); });

    auto *escSc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escSc, &QShortcut::activated, this, &TextEditorWindow::close);
    auto *saveSc = new QShortcut(QKeySequence::Save, this);        // ⌘S / Ctrl+S
    connect(saveSc, &QShortcut::activated, this, [this] { save(); });

    m_idleText = tr("未修改");
    refreshState();
}

void TextEditorWindow::showFor(TransferStation *station, qint64 dbId,
                               const QString &originalKey, const QString &title,
                               const QString &text)
{
    m_station = station;
    m_dbId    = dbId;
    m_key     = originalKey;
    // title 有意不用了：文本条目的名字就是正文第一行，再在窗口里单独显示一行纯属占地方
    // （窗口标题栏已经写着"文本预览"）。留着这个参数只为不改信号签名。
    Q_UNUSED(title);

    {
        const QSignalBlocker block(m_edit);        // 载入内容不算"修改"
        m_edit->setPlainText(text);
        m_edit->document()->setModified(false);
        m_edit->moveCursor(QTextCursor::Start);
    }
    m_baseline = text;                             // 改动的判定基线
    m_idleText = tr("未修改");
    refreshState();
    show();
    raise();
    activateWindow();
    m_edit->setFocus();
}

bool TextEditorWindow::modified() const
{
    // 与"已保存的正文"比对，而不是用 QTextDocument::isModified()：
    // setPlainText()/undo 等操作会把这个标志位重置，语义不稳（实测有"改了却报未修改"）。
    return m_edit != nullptr && m_edit->toPlainText() != m_baseline;
}

void TextEditorWindow::refreshState()
{
    if (m_edit == nullptr)
        return;
    const QString t = m_edit->toPlainText();
    const bool dirty = modified();
    if (m_saveBtn != nullptr)
        m_saveBtn->setEnabled(dirty);
    if (m_status != nullptr) {
        // 状态 + 字数/行数挤在底栏一行里（原来单独占一行标题，纯属浪费纵向空间）
        const QString state = dirty ? tr("已修改（关闭时会自动保存）") : m_idleText;
        m_status->setText(tr("%1 · %2 字 · %3 行")
                              .arg(state)
                              .arg(t.size())
                              .arg(m_edit->document()->blockCount()));
    }
}

void TextEditorWindow::save()
{
    if (m_station == nullptr || m_edit == nullptr)
        return;
    const QString text = m_edit->toPlainText();
    if (m_station->applyTextEdit(m_dbId, m_key, text)) {
        m_key      = QStringLiteral("t:") + text;    // 内容变了，去重键随之变化
        m_baseline = text;                           // 这版已是"库里的版本"
        m_idleText = tr("已保存");
        m_edit->document()->setModified(false);
    } else {
        m_idleText = tr("条目已被删除，未保存");
    }
    refreshState();
}

void TextEditorWindow::closeEvent(QCloseEvent *event)
{
    if (modified())
        save();                    // 没点保存就关：自动落盘，别让用户白改
    QWidget::closeEvent(event);
}

void TextEditorWindow::keyPressEvent(QKeyEvent *event)
{
    // 快捷键兜底（正文控件吃掉按键时会走到这里）
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    if (event->key() == Qt::Key_S
        && (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
        save();
        return;
    }
    QWidget::keyPressEvent(event);
}

// ============================ PopDock ============================

// 右下角"新增记事"按钮图标：铅笔 + 右上角"+"角标（表示"新增/书写"）
QIcon PopDock::noteAddIcon()
{
    const int S = 64;
    QPixmap pm(S, S);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    // 圆形按钮底色
    p.setBrush(QColor(58, 110, 165));
    p.setPen(Qt::NoPen);
    p.drawEllipse(4, 4, S - 8, S - 8);

    // 铅笔（白色，旋转 45° 呈对角线）
    p.save();
    p.translate(S / 2, S / 2);
    p.rotate(45);
    p.setBrush(Qt::white);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(-3.5, -16, 7, 26, 3, 3);          // 笔身
    QPolygonF nib;                                        // 笔尖
    nib << QPointF(-3.5, 10) << QPointF(3.5, 10) << QPointF(0, 19);
    p.drawPolygon(nib);
    p.setBrush(QColor(225, 225, 225));                   // 橡皮头
    p.drawRoundedRect(-3.5, -20, 7, 6, 2, 2);
    p.restore();

    // 右上角白色小圆 + 蓝色"+"角标，表示"新增"
    const QPoint c(47, 47);
    p.setBrush(QColor(245, 245, 245));
    p.setPen(Qt::NoPen);
    p.drawEllipse(c.x() - 9, c.y() - 9, 18, 18);
    p.setPen(QPen(QColor(58, 110, 165), 2.5, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(c.x(), c.y() - 6, c.x(), c.y() + 6);
    p.drawLine(c.x() - 6, c.y(), c.x() + 6, c.y());

    return QIcon(pm);
}

// 记事输入框右侧的"确认"按钮：回车在中文输入法下常被用于确认候选词，
// 不一定触发 returnPressed，所以必须给一个不依赖回车的保存入口。
QIcon PopDock::noteOkIcon()
{
    const int S = 48;
    QPixmap pm(S, S);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor(58, 110, 165));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(4, 4, S - 8, S - 8, 10, 10);
    p.setPen(QPen(Qt::white, 3.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    QPolygonF tick;
    tick << QPointF(14, 25) << QPointF(21, 32) << QPointF(34, 17);
    p.drawPolyline(tick);
    return QIcon(pm);
}

// ---------------- 划出 / 划入动画 ----------------

// 时长与位移幅度：170ms 够看出"划出来"，又不会拖沓；
// 位移沿"球 → 面板"方向 14px，形成"从球里长出来"的观感。
static constexpr int kDockAnimMs  = 170;
static constexpr int kDockSlidePx = 14;

void PopDock::ensureAnimations()
{
    if (m_anim != nullptr)
        return;

    m_slide = new QPropertyAnimation(this, "pos", this);
    m_slide->setDuration(kDockAnimMs);
    m_fade = new QPropertyAnimation(this, "windowOpacity", this);
    m_fade->setDuration(kDockAnimMs);

    m_anim = new QParallelAnimationGroup(this);
    m_anim->addAnimation(m_slide);
    m_anim->addAnimation(m_fade);

    // 收起动画播完才真正隐藏（中间被 showAnimated 打断时 m_hiding 已被清掉）
    connect(m_anim, &QParallelAnimationGroup::finished, this, [this] {
        if (!m_hiding)
            return;
        m_hiding = false;
        QWidget::hide();
        setWindowOpacity(1.0);      // 复原，供下次淡入
    });
}

void PopDock::showAnimated(const QPoint &targetPos, const QRect &ballRect)
{
    ensureAnimations();
    m_anim->stop();

    // 起点：从球那一侧斜着偏出来一点点（球在面板左边就往左偏）
    const QPoint panelCenter(targetPos.x() + width() / 2, targetPos.y() + height() / 2);
    const QPoint d = ballRect.center() - panelCenter;
    QPoint offset(0, 0);
    if (qAbs(d.x()) >= qAbs(d.y()))
        offset.setX(d.x() > 0 ? kDockSlidePx : (d.x() < 0 ? -kDockSlidePx : 0));
    else
        offset.setY(d.y() > 0 ? kDockSlidePx : (d.y() < 0 ? -kDockSlidePx : 0));
    m_slideFrom    = targetPos + offset;
    m_targetPos    = targetPos;
    m_hasSlideFrom = true;
    m_hiding       = false;

    m_slide->setStartValue(m_slideFrom);
    m_slide->setEndValue(targetPos);
    m_slide->setEasingCurve(QEasingCurve::OutCubic);
    m_fade->setStartValue(0.0);
    m_fade->setEndValue(1.0);
    m_fade->setEasingCurve(QEasingCurve::OutCubic);

    // 先挪到起点、透明、再 show：避免"先闪一下终点的实心面板再滑进来"
    move(m_slideFrom);
    setWindowOpacity(0.0);
    QWidget::show();
    m_anim->start();
}

void PopDock::hideAnimated()
{
    if (this->isHidden())
        return;
    ensureAnimations();
    m_anim->stop();
    hidePreview();                          // 预览气泡不跟着一起滑

    m_hiding = true;
    if (!m_hasSlideFrom)
        m_slideFrom = pos();                // 没划过（例如启动即显示）就原地淡出

    m_slide->setStartValue(pos());
    m_slide->setEndValue(m_slideFrom);      // 沿原路滑回球的方向
    m_slide->setEasingCurve(QEasingCurve::InCubic);
    m_fade->setStartValue(windowOpacity());
    m_fade->setEndValue(0.0);
    m_fade->setEasingCurve(QEasingCurve::InCubic);
    m_anim->start();
}

void PopDock::cancelHideAnimation()
{
    if (m_anim == nullptr || !m_hiding)
        return;
    m_anim->stop();                         // 停在当前位置
    m_hiding = false;
    move(m_targetPos);                      // 回到落点
    setWindowOpacity(1.0);
}

PopDock::PopDock(QWidget *parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(kPreferredWidth, kPreferredHeight);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 12);        // 统一的窗口内边距
    root->setSpacing(8);

    // ---------- 标题区（小）：数据中转站 + 条目数 ----------
    {
        auto *header = new QWidget(this);
        header->setObjectName(QStringLiteral("popDockHeader"));
        header->setStyleSheet(QStringLiteral("background:transparent;"));
        header->setFixedHeight(22);
        auto *hl = new QHBoxLayout(header);
        hl->setContentsMargins(2, 0, 2, 0);
        hl->setSpacing(6);

        m_titleLabel = new QLabel(tr("数据中转站"), header);
        m_titleLabel->setObjectName(QStringLiteral("popDockTitle"));
        {
            QFont f = m_titleLabel->font();
            f.setPixelSize(13);
            f.setBold(true);
            m_titleLabel->setFont(f);
        }
        m_titleLabel->setStyleSheet(QStringLiteral("background:transparent;color:#d7dbe0;"));

        m_countLabel = new QLabel(header);
        m_countLabel->setObjectName(QStringLiteral("popDockCount"));
        {
            QFont f = m_countLabel->font();
            f.setPixelSize(11);
            m_countLabel->setFont(f);
        }
        m_countLabel->setStyleSheet(QStringLiteral("background:transparent;color:#8b9199;"));

        hl->addWidget(m_titleLabel);
        hl->addStretch(1);
        hl->addWidget(m_countLabel);
        root->addWidget(header, 0);
    }

    // ---------- 中转站（文件 / 剪贴板 / 记事）占满中部 ----------
    m_station = new TransferStation(this);
    m_station->setObjectName(QStringLiteral("popDockStation"));
    root->addWidget(m_station, 1);

    // ---------- 记事编辑行：输入框 + 确认（显示时占布局一行，隐藏时不占空间） ----------
    m_noteRow = new QWidget(this);
    m_noteRow->setObjectName(QStringLiteral("popDockNoteRow"));
    m_noteRow->setStyleSheet(QStringLiteral("background:transparent;"));
    {
        auto *nr = new QHBoxLayout(m_noteRow);
        nr->setContentsMargins(0, 0, 0, 0);
        nr->setSpacing(6);

        m_noteEdit = new QLineEdit(m_noteRow);
        m_noteEdit->setObjectName(QStringLiteral("popDockNoteEdit"));
        m_noteEdit->setPlaceholderText(tr("输入记事，回车或点 ✓ 保存…"));
        {
            QFont ef = m_noteEdit->font();
            ef.setPixelSize(12);
            m_noteEdit->setFont(ef);
        }
        m_noteEdit->setStyleSheet(QStringLiteral(
            "QLineEdit{background:rgba(255,255,255,0.10);color:#e8eaed;"
            "border:1px solid rgba(255,255,255,0.18);border-radius:6px;padding:5px 8px;}"
            "QLineEdit:focus{border:1px solid rgba(96,150,205,0.95);}"));
        nr->addWidget(m_noteEdit, 1);

        m_noteOkBtn = new QToolButton(m_noteRow);
        m_noteOkBtn->setObjectName(QStringLiteral("popDockNoteOk"));
        m_noteOkBtn->setIcon(noteOkIcon());
        m_noteOkBtn->setIconSize(QSize(22, 22));
        m_noteOkBtn->setFixedSize(26, 26);
        m_noteOkBtn->setCursor(Qt::PointingHandCursor);
        m_noteOkBtn->setFocusPolicy(Qt::NoFocus);
        m_noteOkBtn->setToolTip(tr("保存记事"));
        m_noteOkBtn->setStyleSheet(QStringLiteral(
            "QToolButton{border:none;background:transparent;border-radius:6px;}"
            "QToolButton:hover{background:rgba(255,255,255,0.12);}"));
        nr->addWidget(m_noteOkBtn, 0, Qt::AlignVCenter);
    }
    m_noteRow->hide();
    root->addWidget(m_noteRow, 0);

    // ---------- 底部工具栏：左侧布局切换（默认图标），右侧新增记事 ----------
    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(0, 0, 0, 0);
    footer->setSpacing(4);

    auto *viewGroup = new QButtonGroup(this);
    viewGroup->setExclusive(true);
    const QString viewNames[4] = { tr("图标"), tr("列表"), tr("详细"), tr("预览") };
    for (int i = 0; i < 4; ++i) {
        auto *b = new QToolButton(this);
        b->setObjectName(QStringLiteral("popDockView%1").arg(i));
        b->setText(viewNames[i]);
        b->setCheckable(true);
        b->setChecked(i == 0);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedHeight(26);
        b->setMinimumWidth(44);
        b->setToolTip(i == 3 ? tr("预览：只显示缩略图") : tr("切换展示布局"));
        {
            QFont bf = b->font();
            bf.setPixelSize(11);
            b->setFont(bf);
        }
        b->setStyleSheet(QStringLiteral(
            "QToolButton{border:1px solid transparent;border-radius:6px;color:#9aa0a6;"
            "background:transparent;padding:1px 6px;}"
            "QToolButton:hover{background:rgba(255,255,255,0.10);color:#e8eaed;}"
            "QToolButton:checked{background:rgba(58,110,165,0.90);color:#ffffff;}"));
        viewGroup->addButton(b, i);
        footer->addWidget(b);
        m_viewBtns[i] = b;
    }
    footer->addStretch(1);

    m_addNoteBtn = new QToolButton(this);
    m_addNoteBtn->setObjectName(QStringLiteral("popDockAddNote"));
    m_addNoteBtn->setIcon(noteAddIcon());
    m_addNoteBtn->setIconSize(QSize(26, 26));
    m_addNoteBtn->setFixedSize(28, 28);
    m_addNoteBtn->setCursor(Qt::PointingHandCursor);
    m_addNoteBtn->setFocusPolicy(Qt::NoFocus);
    m_addNoteBtn->setToolTip(tr("新增记事"));
    m_addNoteBtn->setStyleSheet(QStringLiteral(
        "QToolButton{border:none;background:transparent;border-radius:14px;}"
        "QToolButton:hover{background:rgba(255,255,255,0.12);}"));
    footer->addWidget(m_addNoteBtn, 0, Qt::AlignRight | Qt::AlignVCenter);
    root->addLayout(footer, 0);

    connect(viewGroup, &QButtonGroup::idClicked, this, [this](int id) {
        m_station->setViewStyle(TransferStation::ViewStyle(id));
        hidePreview();
        emit viewStyleChanged(id);          // Widget 负责写回配置
    });
    connect(m_addNoteBtn, &QToolButton::clicked, this, &PopDock::onNoteHover);
    connect(m_noteOkBtn, &QToolButton::clicked, this, &PopDock::onNoteReturn);
    connect(m_noteEdit, &QLineEdit::returnPressed, this, &PopDock::onNoteReturn);
    connect(m_station, &QListWidget::itemDoubleClicked, this, &PopDock::onItemDoubleClicked);
    connect(m_station, &TransferStation::previewRequested, this, &PopDock::onPreviewRequested);
    connect(m_station, &TransferStation::previewHideRequested, this,
            &PopDock::onPreviewHideRequested);
    connect(m_station, &TransferStation::stationChanged, this, &PopDock::refreshCount);
    // 文本条目"打开" → 弹小编辑器
    connect(m_station, &TransferStation::editTextRequested, this, &PopDock::openTextEditor);
    // 右键菜单 / 拖出条目期间加交互锁：用户在操作，面板不能自动收起
    connect(m_station, &TransferStation::interactionBegin, this, &PopDock::beginInteraction);
    connect(m_station, &TransferStation::interactionEnd,   this, &PopDock::endInteraction);

    // ---------- 剪贴板历史（SQLite）+ 剪贴板监听 ----------
    openHistoryStore();
    m_clipTimer = new QTimer(this);
    m_clipTimer->setSingleShot(true);
    m_clipTimer->setInterval(250);          // 防抖：一次复制可能连发几次 dataChanged
    connect(m_clipTimer, &QTimer::timeout, this, &PopDock::captureClipboard);
    if (QClipboard *cb = QApplication::clipboard())
        connect(cb, &QClipboard::dataChanged, this, &PopDock::onClipboardChanged);

    refreshCount();
    installDockTracking(this);
}

PopDock::~PopDock()
{
    if (m_textEditor != nullptr) {
        m_textEditor->close();     // 关闭会触发"未保存改动自动保存"
        delete m_textEditor;       // 顶层窗口、无 parent：由面板统一持有与释放
        m_textEditor = nullptr;
    }
}

QWidget *PopDock::textEditor() const
{
    return m_textEditor;
}

// 弹出（或复用）文本小编辑器。顶层窗口不设 parent：设了就会被当成面板里的子控件裁掉了。
void PopDock::openTextEditor(qint64 dbId, const QString &key, const QString &title,
                             const QString &text)
{
    if (m_textEditor == nullptr)
        m_textEditor = new TextEditorWindow();
    m_textEditor->showFor(m_station, dbId, key, title, text);
}

void PopDock::openHistoryStore()
{
    m_store = new ClipStore(this);
    // 默认放在家目录（与 ~/.popball2_config.ini 一致的约定），
    // 自检/多实例可以用 POPBALL2_DB 指到临时文件，避免污染真实历史。
    QString dbPath = qEnvironmentVariable("POPBALL2_DB");
    if (dbPath.isEmpty())
        dbPath = QDir(QDir::homePath()).absoluteFilePath(".popball2_clipboard.db");
    if (!m_store->open(dbPath)) {
        qInfo() << "[PopDock] 剪贴板历史不可用（仅本会话内存保存）:" << m_store->lastError();
        return;
    }
    m_station->setStore(m_store);
    m_store->prune();
}

void PopDock::ensureHistoryLoaded()
{
    if (m_historyLoaded)
        return;
    m_historyLoaded = true;
    if (m_store == nullptr || !m_store->isReady())
        return;
    m_station->loadRecords(m_store->recent(60));   // 追加到末尾，新的仍在最前
    refreshCount();
}

void PopDock::refreshCount()
{
    if (m_countLabel == nullptr || m_station == nullptr)
        return;
    m_countLabel->setText(tr("%1 项").arg(m_station->count()));
}

void PopDock::setViewStyle(int style)
{
    const int s = qBound(0, style, 3);          // 0=图标 1=列表 2=详细 3=预览
    if (m_station != nullptr)
        m_station->setViewStyle(TransferStation::ViewStyle(s));
    if (m_viewBtns[s] != nullptr)
        m_viewBtns[s]->setChecked(true);
    hidePreview();
}

int PopDock::viewStyle() const
{
    return m_station != nullptr ? int(m_station->viewStyle()) : 0;
}

// 剪贴板变了：防抖后再抓（一次复制常连发多个 dataChanged）
void PopDock::onClipboardChanged()
{
    if (m_clipTimer != nullptr && !m_clipTimer->isActive())
        m_clipTimer->start();
}

// 抓当前剪贴板内容。类型优先级：本地文件 URL > 图片 > 文本。
//   复制文件时系统往往也附带图标/文本，而网页里复制图片常带 HTML/URL，
//   所以"本地真实文件"最优先，其次图片，最后才是纯文本。
void PopDock::captureClipboard()
{
    QClipboard *cb = QApplication::clipboard();
    const QMimeData *m = cb ? cb->mimeData() : nullptr;
    if (m == nullptr)
        return;

    ensureHistoryLoaded();

    QStringList files;
    if (m->hasUrls()) {
        for (const QUrl &u : m->urls())
            if (u.isLocalFile() && QFileInfo::exists(u.toLocalFile()))
                files << u.toLocalFile();
    }
    if (!files.isEmpty()) {
        m_station->addFileItems(files);       // 保持剪贴板里的原始顺序
        return;
    }

    if (m->hasImage()) {
        const QImage img = qvariant_cast<QImage>(m->imageData());
        if (!img.isNull()) {
            m_station->addImageItem(img);
            return;
        }
    }

    if (m->hasText()) {
        const QString t = m->text();
        if (!t.trimmed().isEmpty())
            m_station->addTextItem(t);
    }
}

void PopDock::installDockTracking(QObject *obj)
{
    obj->installEventFilter(this);
    for (QObject *c : obj->children())
        if (c->isWidgetType())
            installDockTracking(c);
}

bool PopDock::eventFilter(QObject *watched, QEvent *e)
{
    if (e->type() == QEvent::Enter) {
        emit mouseEntered();
        return false;
    }
    if (e->type() == QEvent::Leave) {
        // 正在菜单/拖拽里操作：这次的 Leave 多半只是"光标移到弹出菜单上"，
        // 不能当成离开面板（否则面板会在右键时自己消失）。
        if (isInteractionLocked())
            return false;
        QWidget *w = QApplication::widgetAt(QCursor::pos());
        if (w == this || this->isAncestorOf(w))
            return false;           // 仍在面板内（含子控件），不算离开
        emit mouseLeft();
        return false;
    }
    return QWidget::eventFilter(watched, e);
}

void PopDock::beginInteraction()
{
    ++m_interactionLock;
    if (m_interactionLock == 1)
        emit interactionStarted();      // 让 Widget 把已排队的收起取消掉
}

void PopDock::endInteraction()
{
    if (m_interactionLock > 0)
        --m_interactionLock;
}

void PopDock::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    hidePreview();
    ensureHistoryLoaded();          // 先把历史补进来
    captureClipboard();             // 再记录一次当前剪贴板（保证刚复制的内容在最前）
    refreshCount();
    m_station->setFocus();          // 拿到焦点，Ctrl/Cmd+V 粘贴才生效
}

void PopDock::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    hidePreview();                  // 面板收起时预览气泡一并收起
}

void PopDock::addFiles(const QStringList &paths)
{
    m_station->addFileItems(paths);           // 保持调用方给的顺序
    m_station->setFocus();
}

void PopDock::addImage(const QImage &image)
{
    m_station->addImageItem(image);
    m_station->setFocus();
}

void PopDock::addText(const QString &text)
{
    m_station->addTextItem(text);
    m_station->setFocus();
}

void PopDock::onNoteHover()
{
    m_noteEdit->setText(QString());
    m_noteRow->show();
    m_noteEdit->setFocus();
}

void PopDock::onNoteReturn()
{
    const QString t = m_noteEdit->text().trimmed();
    if (!t.isEmpty())
        m_station->addTextItem(t);      // 直接用内容当标题：图标/列表里都能一眼看到
    m_noteEdit->clear();
    m_noteRow->hide();
    m_station->setFocus();
}

void PopDock::onItemDoubleClicked(QListWidgetItem *item)
{
    m_station->openItem(item);
}

void PopDock::hidePreview()
{
    if (m_preview == nullptr)
        return;
    m_preview->stopVideo();                 // 视频预览要停掉，别让它在后台继续解码
    if (m_preview->isVisible())
        m_preview->hide();
}

void PopDock::onPreviewHideRequested()
{
    hidePreview();
}

void PopDock::onPreviewRequested(const QVariantMap &data, const QPoint &globalCenter)
{
    if (this->isHidden())
        return;

    const TsType type = TsType(data.value(QStringLiteral("type")).toInt());
    const QString name = data.value(QStringLiteral("name")).toString();

    if (m_preview == nullptr)
        m_preview = new PreviewPopup(this);

    if (type == TsType::Text) {
        const QString t = data.value(QStringLiteral("text")).toString();
        m_preview->showText(t, tr("文本 · %1 字").arg(t.size()));
    } else if (type == TsType::Image) {
        const QString p = data.value(QStringLiteral("path")).toString();
        const QImage img(p);
        if (img.isNull()) {
            hidePreview();
            return;
        }
        const QImage scaled = img.scaled(320, 240, Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation);
        m_preview->showPixmap(QPixmap::fromImage(scaled),
                              QStringLiteral("%1 · %2×%3")
                                  .arg(name).arg(img.width()).arg(img.height()));
    } else if (type == TsType::Video) {
        // 视频：悬停即静音循环播放（没有 QtMultimedia 时退化成静态封面 + 提示）
        const QString p = data.value(QStringLiteral("path")).toString();
        QFileInfo fi(p);
        m_preview->showVideo(p,
                             QStringLiteral("%1 · %2")
                                 .arg(fi.fileName()).arg(humanSize(fi.size())),
                             m_station->videoThumbFor(
                                 data.value(QStringLiteral("dedup")).toString()));
    } else {
        // 文件：图片文件直接给大图预览；其它给放大图标 + 文件名 · 所在目录
        const QString p = data.value(QStringLiteral("path")).toString();
        QFileInfo fi(p);
        // 带尺寸读，别整张解码（手机原图动辄几千万像素）；尺寸说明取文件真实宽高
        const QImage img = TransferStation::isImageFile(p)
                               ? readImageScaled(p, QSize(320, 240))
                               : QImage();
        if (!img.isNull()) {
            const QSize real = QImageReader(p).size();      // 只读文件头，拿原始宽高
            m_preview->showPixmap(QPixmap::fromImage(img),
                                  QStringLiteral("%1 · %2×%3 · %4")
                                      .arg(fi.fileName())
                                      .arg(real.isValid() ? real.width() : img.width())
                                      .arg(real.isValid() ? real.height() : img.height())
                                      .arg(humanSize(fi.size())));
        } else {
            QFileIconProvider prov;
            QIcon ic = prov.icon(fi);
            if (ic.isNull())
                ic = prov.icon(QFileIconProvider::File);
            m_preview->showPixmap(ic.pixmap(96, 96),
                                  fi.fileName() + QStringLiteral(" · ")
                                      + QDir::toNativeSeparators(fi.absolutePath()));
        }
    }

    // 摆在面板"朝屏幕内侧"的一侧，纵向对齐所悬停的条目；越界则翻到另一侧
    QScreen *scr = this->screen();
    if (scr == nullptr)
        scr = QGuiApplication::primaryScreen();
    const QRect sr = scr->availableGeometry();
    const QRect me = this->geometry();
    const int pw = m_preview->width();
    const int ph = m_preview->height();

    const bool preferRight = (me.center().x() <= sr.center().x());
    int x = preferRight ? (me.right() + 8) : (me.left() - 8 - pw);
    if (x < sr.left() || x + pw > sr.right())
        x = preferRight ? (me.left() - 8 - pw) : (me.right() + 8);
    x = qBound(sr.left(), x, qMax(sr.left(), sr.right() - pw));

    const int y = qBound(sr.top(), globalCenter.y() - ph / 2,
                         qMax(sr.top(), sr.bottom() - ph));

    m_preview->move(x, y);
    m_preview->show();
    m_preview->raise();
}

void PopDock::paintEvent(QPaintEvent *e)
{
    Q_UNUSED(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(QColor(74, 80, 92), 1));
    p.setBrush(QColor(30, 33, 39, 244));
    p.drawRoundedRect(r, 14, 14);
}
