#ifndef AIPANEL_H
#define AIPANEL_H

#include <QWidget>
#include <QColor>
#include <functional>

#include "aiagent.h"

class QScrollArea;
class QVBoxLayout;
class QPlainTextEdit;
class QToolButton;
class QLabel;
class QResizeEvent;

// 弹窗右侧的 AI 助手面板（可关闭）。
// - 即时通讯式聊天记录（左右气泡 + 流式输出）
// - 输入框（Enter 发送 / Shift+Enter 换行），发送中按钮变成「停止」
// - 右上角配置协议 / 地址 / 密钥 / 模型 / 系统提示词
// - 右上角 × 关闭整块区域（PopDock 负责落盘与重新定位）
class AiPanel : public QWidget
{
    Q_OBJECT
public:
    explicit AiPanel(QWidget *parent = nullptr);

    void setSettings(const AiSettings &s);
    AiSettings settings() const { return m_settings; }
    void setAccentColor(const QColor &c);
    void setToolExecutor(AiChat::ToolExecutor fn) { m_chat->setToolExecutor(std::move(fn)); }
    void focusInput();
    bool hasApiKey() const { return !m_settings.apiKey.trimmed().isEmpty(); }

signals:
    void closeRequested();
    // 点「配置」：请求打开主设置窗口的 AI 分类
    void settingsRequested();
    // 面板"正在被使用"（输入框有焦点或正在生成）→ PopDock 加交互锁，鼠标移开也不收起
    void panelActiveChanged(bool active);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void buildUi();
    void requestSettings();
    void submit();
    void setBusyUi(bool busy);

    // 追加一条消息气泡。mine=true 靠右（用户），false 靠左（AI）。
    // error=true 用红色提示底色。返回该气泡的标签，便于流式追加文本。
    QLabel *addBubble(const QString &text, bool mine, bool error = false);
    void appendUser(const QString &text);
    void appendStreamText(const QString &text);
    void appendError(const QString &text);
    void appendHint(const QString &text);
    void clearChat();
    void scrollToEnd(bool force);
    // 气泡最大宽度 = 聊天区宽度 - 头像/边距（面板改宽或窗口缩放时重算）
    int  bubbleMaxWidth() const;
    void updateBubbleWidths();
    void refreshStatus();
    void notifyActive();

    AiChat         *m_chat = nullptr;
    QScrollArea    *m_chatScroll = nullptr;
    QWidget        *m_chatBody = nullptr;
    QVBoxLayout    *m_chatLayout = nullptr;
    QLabel         *m_streamLbl = nullptr;   // 正在流式输出的那条 AI 气泡
    QPlainTextEdit *m_input = nullptr;
    QToolButton    *m_sendBtn = nullptr;
    QToolButton    *m_clearBtn = nullptr;
    QToolButton    *m_cfgBtn = nullptr;
    QToolButton    *m_closeBtn = nullptr;
    QLabel         *m_modelLbl = nullptr;
    QLabel         *m_statusLbl = nullptr;

    AiSettings m_settings;
    QColor     m_accent = QColor::fromRgb(0x41, 0xB0, 0xDD);
    bool       m_busy = false;
    bool       m_inputFocused = false;
    bool       m_gotDelta = false;
    // 是否跟随输出贴底：停在底部时新内容自动下滚；用户手动上翻后暂停跟随
    bool       m_stickBottom = true;
    // 代码主动滚到底时置位，避免把这次 valueChanged 误判成"用户上翻"
    bool       m_autoScrolling = false;
};

#endif // AIPANEL_H
