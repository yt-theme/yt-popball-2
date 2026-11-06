#include "aiagent.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QElapsedTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QDebug>

// =====================================================================================
//  协议元信息
// =====================================================================================
QString aiProtocolName(int protocol)
{
    switch (protocol) {
    case 1:  return QStringLiteral("Anthropic Messages");
    case 2:  return QStringLiteral("Google Gemini");
    default: return QStringLiteral("OpenAI 兼容");
    }
}

QString aiDefaultBaseUrl(int protocol)
{
    switch (protocol) {
    case 1:  return QStringLiteral("https://api.anthropic.com");
    case 2:  return QStringLiteral("https://generativelanguage.googleapis.com");
    default: return QStringLiteral("https://api.openai.com/v1");
    }
}

QString aiDefaultModel(int protocol)
{
    switch (protocol) {
    case 1:  return QStringLiteral("claude-sonnet-4-5");
    case 2:  return QStringLiteral("gemini-2.5-flash");
    default: return QStringLiteral("gpt-4o-mini");
    }
}

QString aiProtocolHint(int protocol)
{
    switch (protocol) {
    case 1:
        return QStringLiteral("Claude 原生协议：地址 https://api.anthropic.com，"
                              "会自动拼 /v1/messages。");
    case 2:
        return QStringLiteral("Gemini 原生协议：地址 https://generativelanguage.googleapis.com，"
                              "会自动拼 /v1beta/models/<模型>:streamGenerateContent。");
    default:
        return QStringLiteral("任何兼容 /chat/completions 的服务都行：OpenAI、Azure、DeepSeek、"
                              "Kimi、通义、智谱、硅基流动、OpenRouter、Ollama、vLLM、one-api…");
    }
}

namespace {

// ---------------- 内置工具（agent 能力：读写中转站 / 剪贴板） ----------------

struct ToolSpec {
    QString     name;
    QString     description;
    QJsonObject schema;
};

QJsonObject objSchema(const QJsonObject &props, const QJsonArray &required = QJsonArray())
{
    QJsonObject o;
    o.insert(QStringLiteral("type"), QStringLiteral("object"));
    o.insert(QStringLiteral("properties"), props);
    if (!required.isEmpty())
        o.insert(QStringLiteral("required"), required);
    return o;
}

QJsonObject prop(const QString &type, const QString &desc)
{
    QJsonObject o;
    o.insert(QStringLiteral("type"), type);
    o.insert(QStringLiteral("description"), desc);
    return o;
}

QList<ToolSpec> toolSpecs()
{
    QList<ToolSpec> out;

    out.append({ QStringLiteral("list_items"),
        QStringLiteral("列出数据中转站面板里当前的条目：序号、类型、名称，以及文本摘要或文件路径。"
                       "在需要了解用户手头有哪些素材时先调用它。"),
        objSchema({}) });

    out.append({ QStringLiteral("read_item"),
        QStringLiteral("读取某个条目的完整内容。文本条目返回全文；文本类文件返回文件内容"
                       "（最多 256KB）；图片/视频等二进制只返回类型和路径信息。"),
        objSchema({ { QStringLiteral("index"),
                      prop(QStringLiteral("integer"), QStringLiteral("条目序号（从 0 开始，来自 list_items）")) } },
                  { QJsonValue(QStringLiteral("index")) }) });

    out.append({ QStringLiteral("copy_to_clipboard"),
        QStringLiteral("把一段文本写入系统剪贴板（用户可以直接在其他应用里粘贴）。"),
        objSchema({ { QStringLiteral("text"),
                      prop(QStringLiteral("string"), QStringLiteral("要写入剪贴板的完整文本")) } },
                  { QJsonValue(QStringLiteral("text")) }) });

    out.append({ QStringLiteral("add_note"),
        QStringLiteral("把一段文本作为新的「记事」条目加入数据中转站。"),
        objSchema({ { QStringLiteral("text"),
                      prop(QStringLiteral("string"), QStringLiteral("记事正文")) } },
                  { QJsonValue(QStringLiteral("text")) }) });

    return out;
}

// Gemini 的 Schema 用大写类型枚举（STRING / INTEGER / OBJECT …），
// 与 JSON Schema 的小写写法不同，这里递归转换一份。
QJsonValue geminiSchemaValue(const QJsonValue &v);

QJsonObject geminiSchemaObject(const QJsonObject &in)
{
    QJsonObject out;
    for (auto it = in.constBegin(); it != in.constEnd(); ++it) {
        if (it.key() == QLatin1String("type")) {
            out.insert(it.key(), it.value().toString().toUpper());
        } else if (it.key() == QLatin1String("properties")) {
            QJsonObject props;
            const QJsonObject src = it.value().toObject();
            for (auto p = src.constBegin(); p != src.constEnd(); ++p)
                props.insert(p.key(), geminiSchemaValue(p.value()));
            out.insert(it.key(), props);
        } else if (it.key() == QLatin1String("items")) {
            out.insert(it.key(), geminiSchemaValue(it.value()));
        } else {
            out.insert(it.key(), it.value());
        }
    }
    return out;
}

QJsonValue geminiSchemaValue(const QJsonValue &v)
{
    if (v.isObject())
        return geminiSchemaObject(v.toObject());
    return v;
}

// 协议无关的工具声明 → 各协议的 tools 字段
QJsonArray toolsForProtocol(int protocol)
{
    QJsonArray arr;
    const QList<ToolSpec> specs = toolSpecs();
    for (const ToolSpec &t : specs) {
        QJsonObject decl;
        decl.insert(QStringLiteral("name"), t.name);
        decl.insert(QStringLiteral("description"), t.description);
        switch (protocol) {
        case 1: {
            decl.insert(QStringLiteral("input_schema"), t.schema);
            break;
        }
        case 2: {
            decl.insert(QStringLiteral("parameters"), geminiSchemaValue(t.schema).toObject());
            break;
        }
        default: {
            QJsonObject fn = decl;
            QJsonObject wrap;
            wrap.insert(QStringLiteral("type"), QStringLiteral("function"));
            wrap.insert(QStringLiteral("function"), fn);
            arr.append(wrap);
            continue;
        }
        }
        arr.append(decl);
    }
    return arr;
}

QString normalizedBase(const AiSettings &s)
{
    QString b = s.baseUrl.trimmed();
    if (b.isEmpty())
        b = aiDefaultBaseUrl(s.protocol);
    while (b.endsWith(QLatin1Char('/')))
        b.chop(1);
    return b;
}

QString modelOrDefault(const AiSettings &s)
{
    const QString m = s.model.trimmed();
    return m.isEmpty() ? aiDefaultModel(s.protocol) : m;
}

// 各协议的最终请求地址（测试连接与正式对话共用，避免两套拼接逻辑走样）
QUrl endpointFor(const AiSettings &s)
{
    QString base = normalizedBase(s);

    if (s.protocol == 1) {
        if (!base.contains(QLatin1String("/messages")))
            base += base.endsWith(QLatin1String("/v1"))
                        ? QStringLiteral("/messages")
                        : QStringLiteral("/v1/messages");
        return QUrl(base);
    }

    if (s.protocol == 2) {
        if (base.contains(QLatin1String(":streamGenerateContent")))
            return QUrl(base);
        if (!base.contains(QLatin1String("/v1beta")) && !base.endsWith(QLatin1String("/v1")))
            base += QStringLiteral("/v1beta");
        base += QStringLiteral("/models/") + modelOrDefault(s)
                + QStringLiteral(":streamGenerateContent?alt=sse");
        return QUrl(base);
    }

    // OpenAI 兼容：用户可以直接填完整的 chat/completions 地址
    if (!base.contains(QLatin1String("/chat/completions")))
        base += QStringLiteral("/chat/completions");
    return QUrl(base);
}

// 默认鉴权头 + 用户自定义的额外请求头（自定义的放最后，允许覆盖默认值）
void applyHeadersFor(QNetworkRequest &req, const AiSettings &s)
{
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QString key = s.apiKey.trimmed();

    if (s.protocol == 1) {
        req.setRawHeader("x-api-key", key.toUtf8());
        req.setRawHeader("anthropic-version", "2023-06-01");
    } else if (s.protocol == 2) {
        req.setRawHeader("x-goog-api-key", key.toUtf8());
    } else if (req.url().query().contains(QLatin1String("api-version"))) {
        // Azure OpenAI 用的是 api-key + api-version 查询参数，其余都是 Bearer
        req.setRawHeader("api-key", key.toUtf8());
    } else {
        req.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    }

    const QStringList lines = s.headers.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        req.setRawHeader(line.left(colon).trimmed().toUtf8(),
                         line.mid(colon + 1).trimmed().toUtf8());
    }
}

// 设定代理：空 = 跟随系统/应用代理；支持 http://host:port 与 socks5://host:port
void applyProxyFor(QNetworkAccessManager *nam, const AiSettings &s)
{
    const QString p = s.proxy.trimmed();
    if (p.isEmpty()) {
        nam->setProxy(QNetworkProxy::DefaultProxy);
        return;
    }

    const QString withScheme = p.contains(QLatin1String("://"))
                                   ? p
                                   : QStringLiteral("http://") + p;
    const QUrl u(withScheme);
    QNetworkProxy proxy;
    const QString scheme = u.scheme().toLower();
    proxy.setType(scheme.startsWith(QLatin1String("socks"))
                      ? QNetworkProxy::Socks5Proxy
                      : QNetworkProxy::HttpProxy);
    proxy.setHostName(u.host());
    proxy.setPort(quint16(u.port(8080)));
    if (!u.userName().isEmpty()) {
        proxy.setUser(u.userName());
        proxy.setPassword(u.password());
    }
    nam->setProxy(proxy);
}

} // namespace

// =====================================================================================
//  AiChat
// =====================================================================================
AiChat::AiChat(QObject *parent)
    : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

void AiChat::send(const QString &text)
{
    if (m_busy)
        return;
    const QString t = text.trimmed();
    if (t.isEmpty())
        return;

    m_history.append(AiMessage{ QStringLiteral("user"), t });
    m_roundText.clear();
    m_tools.clear();
    m_toolRound = 0;
    m_cancelled = false;
    m_sawDone = false;
    m_busy = true;
    startRequest(buildMessages());
}

void AiChat::cancel()
{
    if (!m_busy)
        return;
    m_cancelled = true;
    if (m_reply != nullptr) {
        m_reply->abort();   // abort 会触发 finished，统一在 onReplyFinished 收尾
    } else {
        m_busy = false;
        emit finished();
    }
}

QUrl AiChat::endpointUrl() const
{
    return endpointFor(m_settings);
}

void AiChat::applyHeaders(QNetworkRequest &req) const
{
    applyHeadersFor(req, m_settings);
}

QJsonArray AiChat::buildMessages() const
{
    QJsonArray arr;
    const int protocol = m_settings.protocol;

    if (protocol == 0 && !m_settings.systemPrompt.trimmed().isEmpty()) {
        QJsonObject sys;
        sys.insert(QStringLiteral("role"), QStringLiteral("system"));
        sys.insert(QStringLiteral("content"), m_settings.systemPrompt.trimmed());
        arr.append(sys);
    }

    // 只带最近 20 条，避免长会话把请求撑爆
    const int from = qMax(0, m_history.size() - 20);
    for (int i = from; i < m_history.size(); ++i) {
        const AiMessage &m = m_history.at(i);
        QJsonObject o;
        if (protocol == 2) {
            o.insert(QStringLiteral("role"),
                     m.role == QLatin1String("assistant") ? QStringLiteral("model")
                                                          : QStringLiteral("user"));
            QJsonArray parts;
            QJsonObject p;
            p.insert(QStringLiteral("text"), m.content);
            parts.append(p);
            o.insert(QStringLiteral("parts"), parts);
        } else {
            o.insert(QStringLiteral("role"), m.role);
            o.insert(QStringLiteral("content"), m.content);
        }
        arr.append(o);
    }
    return arr;
}

QJsonObject AiChat::buildBody(const QJsonArray &messages) const
{
    QJsonObject body;
    body.insert(QStringLiteral("model"), modelOrDefault(m_settings));

    if (m_settings.protocol == 1) {
        body.insert(QStringLiteral("messages"), messages);
        body.insert(QStringLiteral("max_tokens"),
                    m_settings.maxTokens > 0 ? m_settings.maxTokens : 4096);
        if (!m_settings.systemPrompt.trimmed().isEmpty())
            body.insert(QStringLiteral("system"), m_settings.systemPrompt.trimmed());
        body.insert(QStringLiteral("stream"), true);
        if (m_settings.temperature >= 0.0)
            body.insert(QStringLiteral("temperature"), m_settings.temperature);
        if (m_exec)
            body.insert(QStringLiteral("tools"), toolsForProtocol(1));
        return body;
    }

    if (m_settings.protocol == 2) {
        body.insert(QStringLiteral("contents"), messages);
        QJsonObject gen;
        if (m_settings.temperature >= 0.0)
            gen.insert(QStringLiteral("temperature"), m_settings.temperature);
        if (m_settings.maxTokens > 0)
            gen.insert(QStringLiteral("maxOutputTokens"), m_settings.maxTokens);
        if (!gen.isEmpty())
            body.insert(QStringLiteral("generationConfig"), gen);
        if (!m_settings.systemPrompt.trimmed().isEmpty()) {
            QJsonObject sys;
            QJsonArray parts;
            QJsonObject p;
            p.insert(QStringLiteral("text"), m_settings.systemPrompt.trimmed());
            parts.append(p);
            sys.insert(QStringLiteral("parts"), parts);
            body.insert(QStringLiteral("systemInstruction"), sys);
        }
        if (m_exec) {
            QJsonObject tools;
            tools.insert(QStringLiteral("functionDeclarations"), toolsForProtocol(2));
            body.insert(QStringLiteral("tools"), tools);
        }
        return body;
    }

    // OpenAI 兼容
    body.insert(QStringLiteral("messages"), messages);
    body.insert(QStringLiteral("stream"), true);
    if (m_settings.temperature >= 0.0)
        body.insert(QStringLiteral("temperature"), m_settings.temperature);
    if (m_settings.maxTokens > 0)
        body.insert(QStringLiteral("max_tokens"), m_settings.maxTokens);
    if (m_exec) {
        body.insert(QStringLiteral("tools"), toolsForProtocol(0));
        body.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
    }
    return body;
}

void AiChat::startRequest(const QJsonArray &messages)
{
    if (m_settings.apiKey.trimmed().isEmpty()) {
        m_busy = false;
        emit failed(tr("还没有配置 API Key：点右侧面板的齿轮按钮填写服务地址与密钥。"));
        return;
    }

    m_api = messages;
    m_roundText.clear();
    m_tools.clear();
    m_buf.clear();
    m_event.clear();
    m_sawDone = false;

    QNetworkRequest req(endpointUrl());
    applyHeaders(req);
    req.setTransferTimeout(180000);
    // 每次请求前同步代理设置（改配置后无需重建 AiChat 也生效）
    applyProxyFor(m_nam, m_settings);

    const QByteArray payload =
        QJsonDocument(buildBody(messages)).toJson(QJsonDocument::Compact);

    QNetworkReply *reply = m_nam->post(req, payload);
    m_reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, &AiChat::onReadyRead);
    connect(reply, &QNetworkReply::finished, this, &AiChat::onReplyFinished);
}

void AiChat::onReadyRead()
{
    if (m_reply == nullptr)
        return;
    const QByteArray chunk = m_reply->readAll();
    if (chunk.isEmpty())
        return;

    m_buf += chunk;
    // SSE：按行解析；空行表示一个事件结束（Anthropic 的 event/多行 data 都靠这个）
    while (true) {
        const int nl = m_buf.indexOf('\n');
        if (nl < 0)
            break;
        QByteArray line = m_buf.left(nl);
        m_buf.remove(0, nl + 1);
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.isEmpty()) {
            if (!m_event.isEmpty()) {
                consumeEvent(m_event);
                m_event.clear();
            }
            continue;
        }
        if (line.startsWith("data:")) {
            QByteArray d = line.mid(5);
            if (d.startsWith(' '))
                d.remove(0, 1);
            if (!m_event.isEmpty())
                m_event.append('\n');
            m_event += d;
        }
        // event: / id: / retry: 行对本实现无意义，忽略
    }
}

void AiChat::onReplyFinished()
{
    QNetworkReply *reply = m_reply;
    if (reply == nullptr)
        return;

    const QByteArray rest = reply->readAll();
    if (!rest.isEmpty()) {
        m_buf += rest;
        while (true) {
            const int nl = m_buf.indexOf('\n');
            if (nl < 0)
                break;
            QByteArray line = m_buf.left(nl);
            m_buf.remove(0, nl + 1);
            if (line.endsWith('\r'))
                line.chop(1);
            if (line.isEmpty()) {
                if (!m_event.isEmpty()) {
                    consumeEvent(m_event);
                    m_event.clear();
                }
                continue;
            }
            if (line.startsWith("data:")) {
                QByteArray d = line.mid(5);
                if (d.startsWith(' '))
                    d.remove(0, 1);
                if (!m_event.isEmpty())
                    m_event.append('\n');
                m_event += d;
            }
        }
    }
    if (!m_event.isEmpty()) {
        consumeEvent(m_event);
        m_event.clear();
    }

    m_reply = nullptr;
    reply->deleteLater();

    if (m_cancelled) {
        if (!m_roundText.isEmpty())
            m_history.append(AiMessage{ QStringLiteral("assistant"), m_roundText });
        m_busy = false;
        m_cancelled = false;
        emit finished();
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        QString detail = reply->errorString();
        // 有些服务的错误信息在 body 里，尽量带出来
        if (m_roundText.isEmpty() && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid())
            detail += QStringLiteral("（HTTP %1）")
                          .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
        fail(detail);
        return;
    }

    finishRound();
}

void AiChat::consumeEvent(const QByteArray &data)
{
    if (data == "[DONE]") {
        m_sawDone = true;
        return;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return;
    const QJsonObject o = doc.object();

    if (m_settings.protocol == 1) {
        handleAnthropicEvent(o);
        return;
    }
    if (m_settings.protocol == 2) {
        handleGeminiChunk(o);
        return;
    }
    handleOpenAiChunk(o);
}

void AiChat::handleOpenAiChunk(const QJsonObject &o)
{
    if (o.contains(QLatin1String("error"))) {
        const QJsonObject e = o.value(QStringLiteral("error")).toObject();
        fail(e.value(QStringLiteral("message")).toString(tr("服务返回错误")));
        return;
    }
    const QJsonArray choices = o.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty())
        return;
    const QJsonObject choice = choices.at(0).toObject();
    const QJsonObject deltaObj = choice.value(QStringLiteral("delta")).toObject();

    const QString content = deltaObj.value(QStringLiteral("content")).toString();
    if (!content.isEmpty()) {
        m_roundText += content;
        emit delta(content);
    }

    const QJsonArray calls = deltaObj.value(QStringLiteral("tool_calls")).toArray();
    for (const QJsonValue &cv : calls) {
        const QJsonObject c = cv.toObject();
        const int idx = c.value(QStringLiteral("index")).toInt();
        PendingTool *tool = nullptr;
        for (PendingTool &t : m_tools) {
            if (t.index == idx) {
                tool = &t;
                break;
            }
        }
        if (tool == nullptr) {
            PendingTool t;
            t.index = idx;
            m_tools.append(t);
            tool = &m_tools.last();
        }
        if (tool->id.isEmpty()) {
            const QString id = c.value(QStringLiteral("id")).toString();
            if (!id.isEmpty())
                tool->id = id;
            else
                tool->id = QStringLiteral("call_%1").arg(idx);
        }
        const QJsonObject fn = c.value(QStringLiteral("function")).toObject();
        const QString name = fn.value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            tool->name += name;   // 极少数服务会把名字分片
        const QString args = fn.value(QStringLiteral("arguments")).toString();
        if (!args.isEmpty())
            tool->argsRaw += args;
    }
}

void AiChat::handleAnthropicEvent(const QJsonObject &o)
{
    const QString type = o.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("error")) {
        const QJsonObject e = o.value(QStringLiteral("error")).toObject();
        fail(e.value(QStringLiteral("message")).toString(tr("服务返回错误")));
        return;
    }
    if (type == QLatin1String("content_block_start")) {
        const int idx = o.value(QStringLiteral("index")).toInt();
        const QJsonObject cb = o.value(QStringLiteral("content_block")).toObject();
        if (cb.value(QStringLiteral("type")).toString() == QLatin1String("tool_use")) {
            PendingTool t;
            t.index = idx;
            t.id = cb.value(QStringLiteral("id")).toString();
            t.name = cb.value(QStringLiteral("name")).toString();
            t.argsObj = cb.value(QStringLiteral("input")).toObject();
            t.hasObj = !t.argsObj.isEmpty();
            m_tools.append(t);
        }
        return;
    }
    if (type == QLatin1String("content_block_delta")) {
        const int idx = o.value(QStringLiteral("index")).toInt();
        const QJsonObject d = o.value(QStringLiteral("delta")).toObject();
        const QString dType = d.value(QStringLiteral("type")).toString();
        if (dType == QLatin1String("text_delta")) {
            const QString text = d.value(QStringLiteral("text")).toString();
            if (!text.isEmpty()) {
                m_roundText += text;
                emit delta(text);
            }
        } else if (dType == QLatin1String("input_json_delta")) {
            const QString part = d.value(QStringLiteral("partial_json")).toString();
            for (PendingTool &t : m_tools) {
                if (t.index == idx) {
                    t.argsRaw += part;
                    break;
                }
            }
        }
    }
}

void AiChat::handleGeminiChunk(const QJsonObject &o)
{
    if (o.contains(QLatin1String("error"))) {
        const QJsonObject e = o.value(QStringLiteral("error")).toObject();
        fail(e.value(QStringLiteral("message")).toString(tr("服务返回错误")));
        return;
    }
    if (o.contains(QLatin1String("promptFeedback"))) {
        const QJsonObject fb = o.value(QStringLiteral("promptFeedback")).toObject();
        const QString reason = fb.value(QStringLiteral("blockReason")).toString();
        if (!reason.isEmpty()) {
            fail(tr("请求被服务端拦截：%1").arg(reason));
            return;
        }
    }
    const QJsonArray cands = o.value(QStringLiteral("candidates")).toArray();
    if (cands.isEmpty())
        return;
    const QJsonObject cand = cands.at(0).toObject();
    const QJsonArray parts =
        cand.value(QStringLiteral("content")).toObject().value(QStringLiteral("parts")).toArray();
    for (const QJsonValue &pv : parts) {
        const QJsonObject p = pv.toObject();
        const QString text = p.value(QStringLiteral("text")).toString();
        if (!text.isEmpty()) {
            m_roundText += text;
            emit delta(text);
        }
        if (p.contains(QLatin1String("functionCall"))) {
            const QJsonObject fc = p.value(QStringLiteral("functionCall")).toObject();
            PendingTool t;
            t.index = m_tools.size();
            t.name = fc.value(QStringLiteral("name")).toString();
            t.argsObj = fc.value(QStringLiteral("args")).toObject();
            t.hasObj = true;
            t.id = QStringLiteral("call_%1").arg(t.index);
            m_tools.append(t);
        }
    }
}

void AiChat::finishRound()
{
    if (!m_tools.isEmpty() && m_exec && m_toolRound < kMaxToolRounds) {
        // 把"助手要调用工具"和"工具结果"追加进本轮消息，再请求一次
        if (m_settings.protocol == 0) {
            QJsonObject assistant;
            assistant.insert(QStringLiteral("role"), QStringLiteral("assistant"));
            assistant.insert(QStringLiteral("content"), m_roundText);
            QJsonArray calls;
            for (const PendingTool &t : m_tools) {
                QJsonObject c;
                c.insert(QStringLiteral("id"), t.id);
                c.insert(QStringLiteral("type"), QStringLiteral("function"));
                QJsonObject fn;
                fn.insert(QStringLiteral("name"), t.name);
                fn.insert(QStringLiteral("arguments"),
                          t.argsRaw.isEmpty() ? QStringLiteral("{}") : t.argsRaw);
                c.insert(QStringLiteral("function"), fn);
                calls.append(c);
            }
            assistant.insert(QStringLiteral("tool_calls"), calls);
            m_api.append(assistant);

            for (const PendingTool &t : m_tools) {
                emit toolCalled(t.name);
                const QString result = execTool(t.name, t.argsRaw);
                QJsonObject msg;
                msg.insert(QStringLiteral("role"), QStringLiteral("tool"));
                msg.insert(QStringLiteral("tool_call_id"), t.id);
                msg.insert(QStringLiteral("content"), result);
                m_api.append(msg);
            }
        } else if (m_settings.protocol == 1) {
            QJsonArray assistantContent;
            if (!m_roundText.isEmpty()) {
                QJsonObject p;
                p.insert(QStringLiteral("type"), QStringLiteral("text"));
                p.insert(QStringLiteral("text"), m_roundText);
                assistantContent.append(p);
            }
            for (const PendingTool &t : m_tools) {
                QJsonObject p;
                p.insert(QStringLiteral("type"), QStringLiteral("tool_use"));
                p.insert(QStringLiteral("id"), t.id);
                p.insert(QStringLiteral("name"), t.name);
                p.insert(QStringLiteral("input"), parseArgs(t));
                assistantContent.append(p);
            }
            QJsonObject assistant;
            assistant.insert(QStringLiteral("role"), QStringLiteral("assistant"));
            assistant.insert(QStringLiteral("content"), assistantContent);
            m_api.append(assistant);

            QJsonArray results;
            for (const PendingTool &t : m_tools) {
                emit toolCalled(t.name);
                QJsonObject r;
                r.insert(QStringLiteral("type"), QStringLiteral("tool_result"));
                r.insert(QStringLiteral("tool_use_id"), t.id);
                r.insert(QStringLiteral("content"), execTool(t.name, t.argsRaw));
                results.append(r);
            }
            QJsonObject user;
            user.insert(QStringLiteral("role"), QStringLiteral("user"));
            user.insert(QStringLiteral("content"), results);
            m_api.append(user);
        } else {
            QJsonArray modelParts;
            if (!m_roundText.isEmpty()) {
                QJsonObject p;
                p.insert(QStringLiteral("text"), m_roundText);
                modelParts.append(p);
            }
            for (const PendingTool &t : m_tools) {
                QJsonObject p;
                QJsonObject fc;
                fc.insert(QStringLiteral("name"), t.name);
                fc.insert(QStringLiteral("args"), t.argsObj);
                p.insert(QStringLiteral("functionCall"), fc);
                modelParts.append(p);
            }
            QJsonObject model;
            model.insert(QStringLiteral("role"), QStringLiteral("model"));
            model.insert(QStringLiteral("parts"), modelParts);
            m_api.append(model);

            QJsonArray respParts;
            for (const PendingTool &t : m_tools) {
                emit toolCalled(t.name);
                QJsonObject resp;
                QJsonObject inner;
                inner.insert(QStringLiteral("result"), execTool(t.name, t.argsRaw));
                resp.insert(QStringLiteral("functionResponse"),
                            QJsonObject{ { QStringLiteral("name"), t.name },
                                         { QStringLiteral("response"), inner } });
                respParts.append(resp);
            }
            QJsonObject user;
            user.insert(QStringLiteral("role"), QStringLiteral("user"));
            user.insert(QStringLiteral("parts"), respParts);
            m_api.append(user);
        }

        ++m_toolRound;
        startRequest(m_api);
        return;
    }

    if (!m_roundText.isEmpty())
        m_history.append(AiMessage{ QStringLiteral("assistant"), m_roundText });
    m_busy = false;
    emit finished();
}

QJsonObject AiChat::parseArgs(const PendingTool &t)
{
    if (t.hasObj)
        return t.argsObj;
    if (t.argsRaw.trimmed().isEmpty())
        return QJsonObject();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(t.argsRaw.toUtf8(), &err);
    return (err.error == QJsonParseError::NoError) ? doc.object() : QJsonObject();
}

QString AiChat::execTool(const QString &name, const QString &argsRaw)
{
    if (!m_exec)
        return QStringLiteral("错误：当前没有可用的工具。");
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(argsRaw.toUtf8(), &err);
    const QJsonObject args = (err.error == QJsonParseError::NoError) ? doc.object() : QJsonObject();
    return m_exec(name, args);
}

void AiChat::fail(const QString &message)
{
    if (m_reply != nullptr) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->abort();
        r->deleteLater();
    }
    if (!m_roundText.isEmpty()) {
        m_history.append(AiMessage{ QStringLiteral("assistant"), m_roundText });
        m_roundText.clear();
    }
    m_busy = false;
    emit failed(message);
}

// =====================================================================================
//  配置连通性测试
// =====================================================================================
void testAiConnection(const AiSettings &s, QObject *context,
                      std::function<void(bool ok, const QString &message)> done)
{
    QNetworkAccessManager *nam = new QNetworkAccessManager(context);
    applyProxyFor(nam, s);

    QNetworkRequest req(endpointFor(s));
    applyHeadersFor(req, s);
    req.setTransferTimeout(20000);

    // 最小请求：一句话、只要 1 个 token、非流式 —— 只为确认地址/Key/模型可用，
    // 不做真正的对话，也不写进对话历史。
    QJsonObject body;
    body.insert(QStringLiteral("model"), modelOrDefault(s));
    QJsonArray messages;
    QJsonObject msg;
    msg.insert(QStringLiteral("role"), QStringLiteral("user"));
    msg.insert(QStringLiteral("content"), QStringLiteral("ping"));
    messages.append(msg);

    if (s.protocol == 1) {
        body.insert(QStringLiteral("messages"), messages);
        body.insert(QStringLiteral("max_tokens"), 1);
        body.insert(QStringLiteral("stream"), false);
    } else if (s.protocol == 2) {
        QJsonArray contents;
        QJsonObject turn;
        QJsonArray parts;
        QJsonObject part;
        part.insert(QStringLiteral("text"), QStringLiteral("ping"));
        parts.append(part);
        turn.insert(QStringLiteral("parts"), parts);
        contents.append(turn);
        body.insert(QStringLiteral("contents"), contents);
        QJsonObject gen;
        gen.insert(QStringLiteral("maxOutputTokens"), 1);
        body.insert(QStringLiteral("generationConfig"), gen);
    } else {
        body.insert(QStringLiteral("messages"), messages);
        body.insert(QStringLiteral("max_tokens"), 1);
        body.insert(QStringLiteral("stream"), false);
    }

    QElapsedTimer timer;
    timer.start();
    QNetworkReply *reply = nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));

    QObject::connect(reply, &QNetworkReply::finished, nam, [nam, reply, timer, done, s]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        const int ms = int(timer.elapsed());

        // 服务端通常把失败原因放在 error.message（OpenAI/Anthropic/Gemini 都类似）
        QString detail;
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (doc.isObject()) {
            const QJsonValue e = doc.object().value(QStringLiteral("error"));
            if (e.isObject())
                detail = e.toObject().value(QStringLiteral("message")).toString();
            else if (e.isString())
                detail = e.toString();
        }

        const bool ok = status >= 200 && status < 300;
        QString message;
        if (ok) {
            message = QCoreApplication::translate("AiChat", "连接成功 · HTTP %1 · %2 ms · 模型 %3")
                          .arg(status).arg(ms).arg(modelOrDefault(s));
        } else {
            if (detail.isEmpty()) {
                // 常见网络错误给一句中文说明，比 Qt 的英文 errorString 好懂
                switch (reply->error()) {
                case QNetworkReply::TimeoutError:
                    detail = QCoreApplication::translate("AiChat", "连接超时（20 秒），检查地址或代理");
                    break;
                case QNetworkReply::HostNotFoundError:
                    detail = QCoreApplication::translate("AiChat", "无法解析主机名，检查服务地址");
                    break;
                case QNetworkReply::ConnectionRefusedError:
                    detail = QCoreApplication::translate("AiChat", "目标拒绝连接，检查地址/端口");
                    break;
                case QNetworkReply::ProxyConnectionRefusedError:
                    detail = QCoreApplication::translate("AiChat", "代理拒绝连接，检查代理地址/端口");
                    break;
                case QNetworkReply::ProxyTimeoutError:
                    detail = QCoreApplication::translate("AiChat", "代理连接超时");
                    break;
                case QNetworkReply::ProxyNotFoundError:
                    detail = QCoreApplication::translate("AiChat", "找不到代理服务器");
                    break;
                case QNetworkReply::SslHandshakeFailedError:
                    detail = QCoreApplication::translate("AiChat", "TLS 握手失败，检查代理或证书");
                    break;
                default:
                    detail = reply->errorString();
                    break;
                }
            }
            if (detail.isEmpty())
                detail = QString::fromUtf8(raw.left(200));
            message = status > 0
                          ? QCoreApplication::translate("AiChat", "HTTP %1 · %2")
                                .arg(status).arg(detail)
                          : detail;
        }

        if (done)
            done(ok, message);
        reply->deleteLater();
        nam->deleteLater();
    });
}
