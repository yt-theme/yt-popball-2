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
#include <QBuffer>
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
    enum Mode { IconCells, ListRows, DetailRows };

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
        case IconCells:  return m_iconCellSet ? m_iconCell : QSize(74, 88);
        case DetailRows: return QSize(180, 48);
        case ListRows:   break;
        }
        return QStyledItemDelegate::sizeHint(opt, idx);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        if (m_mode != DetailRows) {
            QStyledItemDelegate::paint(p, opt, idx);
            return;
        }

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

private:
    Mode   m_mode       = IconCells;
    QSize  m_iconCell;
    bool   m_iconCellSet = false;
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

// 图标网格：目标是一行 kIconColumns 列（默认 4）。
// 单元格宽度按当前控件宽度算（预留滚动条），并同步给委托 sizeHint —— 两者必须一致。
void TransferStation::updateIconGrid()
{
    if (m_viewStyle != IconView)
        return;

    int w = this->width();
    if (w <= 0 && parentWidget() != nullptr)
        w = parentWidget()->width() - 24;        // 还没布局时按面板宽度估
    if (w <= 0)
        w = 300;

    const int kScrollBarAllow = 10;              // 给垂直滚动条留位置，避免"有滚动条就少一列"
    const int avail = qMax(kIconColumns * 52, w - kScrollBarAllow);
    const int cellW = qMax(52, avail / kIconColumns);
    const int iconPx = qBound(30, cellW - 26, 54);
    const int cellH = iconPx + 40;               // 图标 + 两行文字

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

// 切换展示布局：图标（默认）/ 列表 / 详细
void TransferStation::setViewStyle(ViewStyle style)
{
    m_viewStyle = style;
    m_delegate->setMode(style == DetailView ? TsItemDelegate::DetailRows
                                            : (style == ListView ? TsItemDelegate::ListRows
                                                                 : TsItemDelegate::IconCells));

    switch (style) {
    case IconView:
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
        setWordWrap(true);
        setTextElideMode(Qt::ElideRight);
        updateIconGrid();
        break;
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

    if (type == TsType::File) {
        QFileInfo fi(path);
        QString s = QObject::tr("文件");
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

void TransferStation::appendItem(const QIcon &icon, const QString &text,
                                 TsType type, const QVariantMap &data, bool toTop)
{
    // 关键：这里不能给 item 传 parent。
    // 一旦传了 parent，item 会立刻被追加进列表（view 已绑定），此后
    // QListWidget::insertItem(row, item) 对"已经属于某个 view"的 item 是空操作，
    // toTop 就永远不会生效——新条目会一直掉到末尾（用户看到的"没显示出来"）。
    auto *it = new QListWidgetItem(icon, text);
    QVariantMap d = data;
    d[QStringLiteral("type")]     = int(type);
    d[QStringLiteral("subtitle")] = subtitleFor(d);
    it->setData(Qt::UserRole, d);
    it->setToolTip(tooltipFor(d));
    it->setFlags(it->flags() | Qt::ItemIsDragEnabled);

    if (toTop)
        insertItem(0, it);
    else
        addItem(it);

    // 新条目滚入可见区：否则列表已滚过时新条目会落在视口外，看起来"没加进去"
    if (toTop)
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

void TransferStation::addFileItemEx(const QString &path, qint64 dbId)
{
    QFileInfo fi(path);
    if (!fi.exists())
        return;
    QFileIconProvider prov;
    QIcon icon = prov.icon(fi);
    if (icon.isNull())
        icon = prov.icon(QFileIconProvider::File);

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
    appendItem(icon, fi.fileName(), TsType::File, d, /*toTop=*/!fromHistory);
}

void TransferStation::addImageItem(const QImage &image, const QString &name)
{
    addImageItemEx(image, name, -1);
}

void TransferStation::addImageItemEx(const QImage &image, const QString &name, qint64 dbId)
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
    appendItem(QIcon(pm), label, TsType::Image, d, /*toTop=*/!fromHistory);
}

void TransferStation::addTextItem(const QString &text, const QString &label)
{
    addTextItemEx(text, label, -1);
}

void TransferStation::addTextItemEx(const QString &text, const QString &label, qint64 dbId)
{
    if (text.isEmpty())
        return;
    const QString flat  = text.simplified();
    const QString l     = label.isEmpty() ? flat.left(24) : label;
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
    appendItem(QIcon(textIcon()), l, TsType::Text, d, /*toTop=*/!fromHistory);
}

// 历史回填：records 已按"最近使用时间倒序"，这里按同样顺序追加到列表末尾
// （调用时列表里已有的条目都比库里的新，追加不会打乱"新的在前"）。
void TransferStation::loadRecords(const QVector<ClipRecord> &records)
{
    for (const ClipRecord &r : records) {
        if (hasDedup(r.hash))
            continue;
        switch (r.kind) {
        case ClipStore::TextKind:
            if (!r.text.isEmpty())
                addTextItemEx(r.text, r.title, r.id);
            break;
        case ClipStore::FileKind:
            if (!r.path.isEmpty() && QFileInfo::exists(r.path))
                addFileItemEx(r.path, r.id);
            break;
        case ClipStore::ImageKind: {
            const QByteArray png = (m_store != nullptr) ? m_store->pngOf(r.id) : QByteArray();
            const QImage img = QImage::fromData(png, "PNG");
            if (!img.isNull())
                addImageItemEx(img, r.title, r.id);
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
        QApplication::clipboard()->setText(d.value(QStringLiteral("text")).toString());
        return;
    }
    const QString p = d.value(QStringLiteral("path")).toString();
    if (!p.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(p));
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
        for (const QUrl &u : m->urls())
            if (u.isLocalFile())
                addFileItem(u.toLocalFile());
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
    const QString viewNames[3] = { tr("图标"), tr("列表"), tr("详细") };
    for (int i = 0; i < 3; ++i) {
        auto *b = new QToolButton(this);
        b->setObjectName(QStringLiteral("popDockView%1").arg(i));
        b->setText(viewNames[i]);
        b->setCheckable(true);
        b->setChecked(i == 0);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedHeight(26);
        b->setMinimumWidth(48);
        b->setToolTip(tr("切换展示布局"));
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

PopDock::~PopDock() = default;

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
    const int s = qBound(0, style, 2);
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
        for (const QString &p : files)
            m_station->addFileItem(p);
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
    for (const QString &p : paths)
        m_station->addFileItem(p);
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
    if (m_preview != nullptr && m_preview->isVisible())
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
    } else {
        // 文件：放大图标 + 文件名 · 所在目录
        QFileInfo fi(data.value(QStringLiteral("path")).toString());
        QFileIconProvider prov;
        QIcon ic = prov.icon(fi);
        if (ic.isNull())
            ic = prov.icon(QFileIconProvider::File);
        m_preview->showPixmap(ic.pixmap(96, 96),
                              fi.fileName() + QStringLiteral(" · ")
                                  + QDir::toNativeSeparators(fi.absolutePath()));
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
