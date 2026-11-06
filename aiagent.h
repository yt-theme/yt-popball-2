#ifndef AIAGENT_H
#define AIAGENT_H

#include <QObject>
#include <QString>
#include <QList>
#include <QUrl>
#include <QJsonObject>
#include <QJsonArray>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

// AI 服务配置。只保留"怎么说话"的差异，不绑定具体厂商：
//   0 = OpenAI Chat Completions 兼容协议
//       （OpenAI / Azure OpenAI / DeepSeek / Kimi / 通义千问 / 智谱 / 硅基流动 /
//          OpenRouter / Ollama / LM Studio / vLLM / one-api 中转 …）
//   1 = Anthropic Messages（Claude 原生协议）
//   2 = Google Gemini 原生协议
struct AiSettings {
    int     protocol     = 0;
    QString baseUrl;                 // 基础地址（可带 /v1）；留空按协议给默认值
    QString apiKey;
    QString model;
    QString systemPrompt;
    double  temperature  = 0.7;
    int     maxTokens    = 0;        // 0 = 不指定（Anthropic 必填，按 4096 兜底）
    int     width        = 360;      // 右侧面板宽度（px）
};

QString aiProtocolName(int protocol);
QString aiDefaultBaseUrl(int protocol);
QString aiDefaultModel(int protocol);
QString aiProtocolHint(int protocol);

// 一条对话消息（只用于展示与重建上下文）
struct AiMessage {
    QString role;      // "user" / "assistant"
    QString content;
};

// 单会话 AI 客户端：流式输出 + 工具调用（agent）循环。
// 只做网络与协议转换，不碰任何界面；工具执行通过 ToolExecutor 回调交给界面层。
class AiChat : public QObject
{
    Q_OBJECT
public:
    // 工具执行器：入参是工具名 + JSON 参数，返回给模型的文本结果。
    using ToolExecutor = std::function<QString(const QString &name, const QJsonObject &args)>;

    explicit AiChat(QObject *parent = nullptr);

    void setSettings(const AiSettings &s) { m_settings = s; }
    AiSettings settings() const { return m_settings; }
    void setToolExecutor(ToolExecutor fn) { m_exec = std::move(fn); }
    void clearHistory() { m_history.clear(); }

    bool busy() const { return m_busy; }
    // 追加一条用户消息并开始请求（内部会自动跑完工具调用轮次）
    void send(const QString &text);
    // 停止当前请求（已生成的部分保留）
    void cancel();

signals:
    void delta(const QString &text);        // 流式增量
    void toolCalled(const QString &name);  // 正在执行某个工具（界面可显示状态）
    void finished();                        // 本轮结束（含工具多轮）
    void failed(const QString &message);

private:
    struct PendingTool {
        int        index = 0;
        QString    id;
        QString    name;
        QString    argsRaw;     // OpenAI：arguments 字符串
        QJsonObject argsObj;    // Anthropic / Gemini：已解析的参数
        bool       hasObj = false;
    };

    void startRequest(const QJsonArray &messages);
    QJsonArray buildMessages() const;
    QJsonObject buildBody(const QJsonArray &messages) const;
    QUrl endpointUrl() const;
    void applyHeaders(QNetworkRequest &req) const;

    void onReadyRead();
    void onReplyFinished();
    void consumeEvent(const QByteArray &data);
    void handleOpenAiChunk(const QJsonObject &o);
    void handleAnthropicEvent(const QJsonObject &o);
    void handleGeminiChunk(const QJsonObject &o);
    void finishRound();
    void fail(const QString &message);
    QJsonObject parseArgs(const PendingTool &t);
    QString execTool(const QString &name, const QString &argsRaw);

    AiSettings m_settings;
    ToolExecutor m_exec;
    QList<AiMessage> m_history;

    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply         *m_reply = nullptr;
    QByteArray             m_buf;       // SSE 行缓冲
    QByteArray             m_event;     // 当前事件累积的 data 行
    QJsonArray             m_api;       // 本轮实际发送的消息（工具往返会追加）
    QList<PendingTool>     m_tools;     // 本轮解析到的工具调用
    QString                m_roundText; // 本轮助手输出
    int                    m_toolRound = 0;
    bool                   m_busy = false;
    bool                   m_cancelled = false;
    bool                   m_sawDone = false;

    static const int kMaxToolRounds = 4;
};

#endif // AIAGENT_H
