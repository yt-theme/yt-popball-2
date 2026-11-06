#include "aipanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QLabel>
#include <QScrollBar>
#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QTimer>
#include <QResizeEvent>
#include <QVariant>
#include <QApplication>

namespace {

QString rgbaOf(const QColor &c, int alpha)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
}

// 聊天气泡样式：用户靠右用主题色实底，AI 靠左用浅灰半透明底，
// 错误用淡红底；圆角与内边距都按即时通讯软件的习惯来。
QString bubbleStyle(const QColor &accent, bool mine, bool error)
{
    if (error) {
        return QStringLiteral(
            "background:rgba(255,110,100,0.16);color:#ff9a92;"
            "border:1px solid rgba(255,110,100,0.28);"
            "border-radius:12px;padding:7px 11px;font-size:12px;");
    }
    if (mine) {
        return QStringLiteral("background:%1;color:#ffffff;"
                              "border-radius:12px;padding:7px 11px;font-size:12px;")
            .arg(rgbaOf(accent, 225));
    }
    return QStringLiteral(
        "background:rgba(255,255,255,0.085);color:#e8eaed;"
        "border:1px solid rgba(255,255,255,0.07);"
        "border-radius:12px;padding:7px 11px;font-size:12px;");
}

// 气泡旁的小圆头像：AI 用主题色，自己用中性灰
QString avatarStyle(const QColor &accent, bool mine)
{
    if (mine)
        return QStringLiteral("background:rgba(255,255,255,0.12);color:#cfd4da;"
                              "border-radius:11px;");
    return QStringLiteral("background:%1;color:%2;border-radius:11px;")
        .arg(rgbaOf(accent, 46), rgbaOf(accent, 235));
}

} // namespace

AiPanel::AiPanel(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aiPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    // 弹窗本身是半透明的（dock_opacity），这里给 AI 区域叠一层更实的深色底，
    // 避免聊天文字和背后的窗口内容互相干扰（与输入框的轻底色一致）。
    setStyleSheet(QStringLiteral(
        "#aiPanel{background:rgba(22,25,31,0.90);border:1px solid rgba(255,255,255,0.08);"
        "border-radius:10px;}"));
    m_chat = new AiChat(this);
    buildUi();

    connect(m_chat, &AiChat::delta, this, &AiPanel::appendStreamText);
    connect(m_chat, &AiChat::toolCalled, this, [this](const QString &name) {
        m_statusLbl->setText(tr("正在使用工具：%1").arg(name));
    });
    connect(m_chat, &AiChat::finished, this, [this]() {
        if (!m_gotDelta)
            appendHint(tr("（没有返回内容）"));
        setBusyUi(false);
        refreshStatus();
    });
    connect(m_chat, &AiChat::failed, this, [this](const QString &message) {
        setBusyUi(false);
        appendError(message);
        refreshStatus();
    });

    setSettings(AiSettings());
    appendHint(tr("配置模型后即可对话。我能读取中转站里的条目，"
                  "也能把结果写回剪贴板或存成记事。"));
}

void AiPanel::buildUi()
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(10, 6, 4, 4);
    v->setSpacing(6);

    // ---------- 顶部：标题 + 模型 + 配置 / 关闭 ----------
    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(6);

    auto *title = new QLabel(tr("AI 助手"), this);
    {
        QFont f = title->font();
        f.setPixelSize(13);
        f.setBold(true);
        title->setFont(f);
    }
    title->setStyleSheet(QStringLiteral("background:transparent;color:#d7dbe0;"));
    header->addWidget(title);

    m_modelLbl = new QLabel(this);
    m_modelLbl->setObjectName(QStringLiteral("aiModel"));
    m_modelLbl->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    {
        QFont f = m_modelLbl->font();
        f.setPixelSize(10);
        m_modelLbl->setFont(f);
    }
    m_modelLbl->setStyleSheet(QStringLiteral("background:transparent;color:#8b9199;"));
    header->addWidget(m_modelLbl, 1);

    m_clearBtn = new QToolButton(this);
    m_clearBtn->setObjectName(QStringLiteral("aiClearBtn"));
    m_clearBtn->setText(tr("清空"));
    m_clearBtn->setCursor(Qt::PointingHandCursor);
    m_clearBtn->setFocusPolicy(Qt::NoFocus);
    m_clearBtn->setToolTip(tr("清空当前对话（气泡与上下文都会清掉）"));
    m_clearBtn->setStyleSheet(QStringLiteral(
        "QToolButton{background:transparent;border:none;border-radius:5px;color:#9aa0a6;"
        "padding:1px 6px;font-size:11px;}"
        "QToolButton:hover{background:rgba(255,255,255,0.12);color:#e8eaed;}"));
    header->addWidget(m_clearBtn);

    m_cfgBtn = new QToolButton(this);
    m_cfgBtn->setObjectName(QStringLiteral("aiCfgBtn"));
    m_cfgBtn->setText(tr("配置"));
    m_cfgBtn->setCursor(Qt::PointingHandCursor);
    m_cfgBtn->setFocusPolicy(Qt::NoFocus);
    m_cfgBtn->setToolTip(tr("配置协议 / 服务地址 / API Key / 模型"));
    m_cfgBtn->setStyleSheet(QStringLiteral(
        "QToolButton{background:transparent;border:none;border-radius:5px;color:#9aa0a6;"
        "padding:1px 6px;font-size:11px;}"
        "QToolButton:hover{background:rgba(255,255,255,0.12);color:#e8eaed;}"));
    header->addWidget(m_cfgBtn);

    m_closeBtn = new QToolButton(this);
    m_closeBtn->setObjectName(QStringLiteral("aiCloseBtn"));
    m_closeBtn->setText(QStringLiteral("×"));
    m_closeBtn->setCursor(Qt::PointingHandCursor);
    m_closeBtn->setFocusPolicy(Qt::NoFocus);
    m_closeBtn->setToolTip(tr("关闭 AI 区域"));
    m_closeBtn->setFixedSize(24, 24);
    {
        QFont f = m_closeBtn->font();
        f.setPixelSize(15);
        m_closeBtn->setFont(f);
    }
    m_closeBtn->setStyleSheet(QStringLiteral(
        "QToolButton{background:transparent;border:none;border-radius:5px;color:#9aa0a6;}"
        "QToolButton:hover{background:rgba(255,255,255,0.18);color:#e8eaed;}"));
    header->addWidget(m_closeBtn);

    v->addLayout(header);

    // ---------- 聊天记录（即时通讯式气泡，可滚动） ----------
    m_chatScroll = new QScrollArea(this);
    m_chatScroll->setObjectName(QStringLiteral("aiChatScroll"));
    m_chatScroll->setWidgetResizable(true);
    m_chatScroll->setFrameShape(QFrame::NoFrame);
    m_chatScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_chatScroll->setStyleSheet(QStringLiteral(
        "QScrollArea{background:transparent;border:none;}"
        "QScrollBar:vertical{background:transparent;width:6px;margin:0;}"
        "QScrollBar::handle:vertical{background:rgba(255,255,255,0.22);"
        "border-radius:3px;min-height:20px;}"
        "QScrollBar::handle:vertical:hover{background:rgba(255,255,255,0.34);}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}"));

    m_chatBody = new QWidget(m_chatScroll);
    m_chatBody->setObjectName(QStringLiteral("aiChatBody"));
    m_chatBody->setStyleSheet(QStringLiteral("#aiChatBody{background:transparent;}"));
    m_chatLayout = new QVBoxLayout(m_chatBody);
    m_chatLayout->setContentsMargins(2, 4, 6, 4);
    m_chatLayout->setSpacing(9);
    // 末尾留一根弹簧：消息从顶部开始排，短对话不会被垂直居中撑开
    m_chatLayout->addStretch(1);
    m_chatScroll->setWidget(m_chatBody);

    // 跟随输出：流式文本会让气泡持续变高，滚动条范围是在下一轮布局后才更新的，
    // 所以不能只在追加时算一次位置 —— 监听范围变化，只要之前停在底部就继续贴底。
    QScrollBar *sb = m_chatScroll->verticalScrollBar();
    connect(sb, &QScrollBar::rangeChanged, this, [this, sb](int, int max) {
        if (!m_stickBottom)
            return;
        m_autoScrolling = true;
        sb->setValue(max);
        m_autoScrolling = false;
    });
    // 用户往上翻（离开底部阈值）就停止跟随；翻回底部重新恢复
    connect(sb, &QScrollBar::valueChanged, this, [this, sb](int v) {
        if (m_autoScrolling)
            return;
        m_stickBottom = v >= sb->maximum() - 8;
    });
    v->addWidget(m_chatScroll, 1);

    // ---------- 输入区 ----------
    auto *composer = new QHBoxLayout;
    composer->setContentsMargins(0, 0, 0, 0);
    composer->setSpacing(6);

    m_input = new QPlainTextEdit(this);
    m_input->setObjectName(QStringLiteral("aiInput"));
    m_input->setPlaceholderText(tr("问我任何问题… Enter 发送，Shift+Enter 换行"));
    m_input->setFixedHeight(64);
    m_input->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    {
        QFont f = m_input->font();
        f.setPixelSize(12);
        m_input->setFont(f);
    }
    m_input->setStyleSheet(QStringLiteral(
        "QPlainTextEdit{background:rgba(255,255,255,0.07);color:#e8eaed;"
        "border:1px solid rgba(255,255,255,0.14);border-radius:6px;padding:5px;}"
        "QPlainTextEdit:focus{border-color:%1;}"
        "QScrollBar:vertical{background:transparent;width:6px;margin:0;}"
        "QScrollBar::handle:vertical{background:rgba(255,255,255,0.22);border-radius:3px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}")
        .arg(rgbaOf(m_accent, 240)));
    m_input->installEventFilter(this);
    composer->addWidget(m_input, 1);

    m_sendBtn = new QToolButton(this);
    m_sendBtn->setObjectName(QStringLiteral("aiSendBtn"));
    m_sendBtn->setText(tr("发送"));
    m_sendBtn->setCursor(Qt::PointingHandCursor);
    m_sendBtn->setFocusPolicy(Qt::NoFocus);
    m_sendBtn->setFixedSize(48, 30);
    m_sendBtn->setStyleSheet(QStringLiteral(
        "QToolButton{background:%1;color:#ffffff;border:none;border-radius:6px;font-size:12px;}"
        "QToolButton:hover{background:%2;}"
        "QToolButton:disabled{background:rgba(255,255,255,0.10);color:#7c828a;}")
        .arg(rgbaOf(m_accent, 235))
        .arg(rgbaOf(m_accent, 255)));
    composer->addWidget(m_sendBtn, 0, Qt::AlignBottom);
    v->addLayout(composer);

    m_statusLbl = new QLabel(this);
    m_statusLbl->setObjectName(QStringLiteral("aiStatus"));
    {
        QFont f = m_statusLbl->font();
        f.setPixelSize(10);
        m_statusLbl->setFont(f);
    }
    m_statusLbl->setStyleSheet(QStringLiteral("background:transparent;color:#8b9199;"));
    m_statusLbl->setWordWrap(true);
    v->addWidget(m_statusLbl);

    connect(m_closeBtn, &QToolButton::clicked, this, &AiPanel::closeRequested);
    connect(m_cfgBtn, &QToolButton::clicked, this, &AiPanel::requestSettings);
    connect(m_clearBtn, &QToolButton::clicked, this, &AiPanel::clearChat);
    connect(m_sendBtn, &QToolButton::clicked, this, [this] {
        if (m_chat->busy()) {
            m_chat->cancel();
            m_statusLbl->setVisible(true);
            m_statusLbl->setText(tr("已停止"));
            return;
        }
        submit();
    });
}

void AiPanel::setAccentColor(const QColor &c)
{
    if (!c.isValid())
        return;
    m_accent = c;

    if (m_sendBtn != nullptr) {
        m_sendBtn->setStyleSheet(QStringLiteral(
            "QToolButton{background:%1;color:#ffffff;border:none;border-radius:6px;font-size:12px;}"
            "QToolButton:hover{background:%2;}"
            "QToolButton:disabled{background:rgba(255,255,255,0.10);color:#7c828a;}")
            .arg(rgbaOf(m_accent, 235))
            .arg(rgbaOf(m_accent, 255)));
    }
    if (m_input != nullptr) {
        m_input->setStyleSheet(QStringLiteral(
            "QPlainTextEdit{background:rgba(255,255,255,0.07);color:#e8eaed;"
            "border:1px solid rgba(255,255,255,0.14);border-radius:6px;padding:5px;}"
            "QPlainTextEdit:focus{border-color:%1;}"
            "QScrollBar:vertical{background:transparent;width:6px;margin:0;}"
            "QScrollBar::handle:vertical{background:rgba(255,255,255,0.22);border-radius:3px;}"
            "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}")
            .arg(rgbaOf(m_accent, 240)));
    }
    // 主题色变了，已经画出来的气泡与头像也要跟着换色
    if (m_chatBody != nullptr) {
        const QList<QLabel *> labels = m_chatBody->findChildren<QLabel *>();
        for (QLabel *b : labels) {
            const QVariant role = b->property("aiRole");
            if (role.isValid()) {
                const QString r = role.toString();
                b->setStyleSheet(bubbleStyle(m_accent, r == QLatin1String("mine"),
                                             r == QLatin1String("error")));
                continue;
            }
            const QVariant av = b->property("aiAvatar");
            if (av.isValid())
                b->setStyleSheet(avatarStyle(m_accent, av.toBool()));
        }
    }
    update();
}

void AiPanel::setSettings(const AiSettings &s)
{
    m_settings = s;
    if (m_settings.width < 280) m_settings.width = 280;
    if (m_settings.width > 620) m_settings.width = 620;
    if (m_settings.temperature < 0.0) m_settings.temperature = 0.0;

    m_chat->setSettings(m_settings);
    setFixedWidth(m_settings.width);

    if (m_modelLbl != nullptr) {
        const QString model = m_settings.model.trimmed();
        const QString shown = model.isEmpty() ? tr("未配置") : model;
        QFontMetrics fm(m_modelLbl->font());
        m_modelLbl->setText(fm.elidedText(shown, Qt::ElideRight, 130));
        m_modelLbl->setToolTip(shown + QStringLiteral(" · ") + aiProtocolName(m_settings.protocol));
    }
    refreshStatus();
}

void AiPanel::focusInput()
{
    if (m_input != nullptr) {
        m_input->setFocus(Qt::OtherFocusReason);
        notifyActive();
    }
}

void AiPanel::refreshStatus()
{
    if (m_statusLbl == nullptr)
        return;
    if (m_busy) {
        m_statusLbl->setVisible(true);
        m_statusLbl->setText(tr("正在生成…"));
        return;
    }
    if (!hasApiKey()) {
        // 未配置时不显示任何提示小字（界面初始提示已经在对话区里）
        m_statusLbl->clear();
        m_statusLbl->setVisible(false);
        return;
    }
    m_statusLbl->setVisible(true);
    m_statusLbl->setText(tr("Enter 发送 · Shift+Enter 换行"));
}

void AiPanel::notifyActive()
{
    emit panelActiveChanged(m_busy || m_inputFocused);
}

void AiPanel::setBusyUi(bool busy)
{
    m_busy = busy;
    if (m_sendBtn != nullptr)
        m_sendBtn->setText(busy ? tr("停止") : tr("发送"));
    notifyActive();
}

bool AiPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_input) {
        if (event->type() == QEvent::FocusIn) {
            m_inputFocused = true;
            notifyActive();
        } else if (event->type() == QEvent::FocusOut) {
            m_inputFocused = false;
            notifyActive();
        }
    }
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        const bool enter = (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter);
        const bool shift = ke->modifiers().testFlag(Qt::ShiftModifier);
        const bool cmdCtrl = ke->modifiers().testFlag(Qt::ControlModifier)
                             || ke->modifiers().testFlag(Qt::MetaModifier);
        if (enter && (!shift || cmdCtrl)) {
            submit();
            return true;
        }
        // Esc：停止生成
        if (ke->key() == Qt::Key_Escape && m_chat->busy()) {
            m_chat->cancel();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AiPanel::submit()
{
    if (m_chat->busy())
        return;
    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty())
        return;
    if (!hasApiKey()) {
        m_statusLbl->setVisible(true);
        m_statusLbl->setText(tr("先配置 API Key 再发送"));
        requestSettings();
        return;
    }

    m_input->clear();
    m_gotDelta = false;
    m_streamLbl = nullptr;
    appendUser(text);
    setBusyUi(true);
    m_statusLbl->setVisible(true);
    m_statusLbl->setText(tr("正在生成…"));
    m_chat->send(text);
}

QLabel *AiPanel::addBubble(const QString &text, bool mine, bool error)
{
    QWidget *row = new QWidget(m_chatBody);
    row->setObjectName(QStringLiteral("aiChatRow"));
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);

    // 小圆头像：AI 在左、自己在右，和气泡同一侧
    auto *avatar = new QLabel(mine ? tr("我") : QStringLiteral("AI"), row);
    avatar->setFixedSize(22, 22);
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setProperty("aiAvatar", mine);
    {
        QFont f = avatar->font();
        f.setPixelSize(mine ? 11 : 9);
        f.setBold(true);
        avatar->setFont(f);
    }
    avatar->setStyleSheet(avatarStyle(m_accent, mine));

    auto *bubble = new QLabel(text, row);
    bubble->setObjectName(QStringLiteral("aiBubble"));
    bubble->setProperty("aiRole", error ? QStringLiteral("error")
                                        : mine ? QStringLiteral("mine")
                                               : QStringLiteral("ai"));
    bubble->setWordWrap(true);
    bubble->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bubble->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    bubble->setMaximumWidth(bubbleMaxWidth());
    bubble->setStyleSheet(bubbleStyle(m_accent, mine, error));

    if (mine) {
        h->addStretch(1);
        h->addWidget(bubble, 0, Qt::AlignTop);
        h->addWidget(avatar, 0, Qt::AlignTop);
    } else {
        h->addWidget(avatar, 0, Qt::AlignTop);
        h->addWidget(bubble, 0, Qt::AlignTop);
        h->addStretch(1);
    }

    // 弹簧始终留在最后，新消息插在它前面
    m_chatLayout->insertWidget(m_chatLayout->count() - 1, row);
    return bubble;
}

int AiPanel::bubbleMaxWidth() const
{
    if (m_chatScroll == nullptr)
        return 240;
    // 聊天区宽 - 头像 22 - 头像间距 6 - 左右留白 8 - 滚动条 6
    return qMax(120, m_chatScroll->viewport()->width() - 44);
}

void AiPanel::updateBubbleWidths()
{
    if (m_chatBody == nullptr)
        return;
    const int mw = bubbleMaxWidth();
    const QList<QLabel *> labels = m_chatBody->findChildren<QLabel *>();
    for (QLabel *b : labels) {
        if (b->property("aiRole").isValid())
            b->setMaximumWidth(mw);
    }
}

void AiPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateBubbleWidths();
}

void AiPanel::appendUser(const QString &text)
{
    // 自己发消息一定回到底部并恢复跟随
    m_streamLbl = nullptr;
    addBubble(text, true);
    scrollToEnd(true);
}

void AiPanel::appendStreamText(const QString &text)
{
    m_gotDelta = true;
    if (m_streamLbl == nullptr)
        m_streamLbl = addBubble(text, false);
    else
        m_streamLbl->setText(m_streamLbl->text() + text);
    // 只要没被用户上翻打断，就继续跟着输出往下滚
    scrollToEnd(m_stickBottom);
}

void AiPanel::appendHint(const QString &text)
{
    auto *lbl = new QLabel(text, m_chatBody);
    lbl->setObjectName(QStringLiteral("aiHint"));
    lbl->setAlignment(Qt::AlignHCenter);
    lbl->setWordWrap(true);
    lbl->setStyleSheet(QStringLiteral(
        "background:transparent;color:#8b9199;font-size:11px;padding:2px 10px;"));
    m_chatLayout->insertWidget(m_chatLayout->count() - 1, lbl);
    scrollToEnd(true);
}

void AiPanel::appendError(const QString &text)
{
    m_streamLbl = nullptr;
    addBubble(text, false, true);
    scrollToEnd(true);
}

void AiPanel::clearChat()
{
    if (m_chatLayout == nullptr)
        return;

    // 先停掉正在进行的请求（m_gotDelta 置真，避免 finished 回调再补一条「没有返回内容」），
    // 然后清上下文和所有气泡，只留下末尾的弹簧。
    if (m_chat->busy()) {
        m_gotDelta = true;
        m_chat->cancel();
    }
    m_chat->clearHistory();
    m_streamLbl = nullptr;
    m_gotDelta = true;

    while (m_chatLayout->count() > 1) {
        QLayoutItem *item = m_chatLayout->takeAt(0);
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    appendHint(tr("对话已清空，可以开始新的提问。"));
    m_stickBottom = true;
    scrollToEnd(true);
}

void AiPanel::scrollToEnd(bool force)
{
    if (m_chatScroll == nullptr)
        return;
    m_stickBottom = force;
    if (!force)
        return;
    // 气泡高度要等下一轮布局才确定，先滚一次；布局完成后的范围变化由 rangeChanged 继续贴底
    QTimer::singleShot(0, this, [this] {
        QScrollBar *sb = m_chatScroll->verticalScrollBar();
        m_autoScrolling = true;
        sb->setValue(sb->maximum());
        m_autoScrolling = false;
    });
}

void AiPanel::requestSettings()
{
    // 不再单独弹深色配置窗，交给主设置窗口并直接跳到「AI」分类
    emit settingsRequested();
}
