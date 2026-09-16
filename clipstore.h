#ifndef CLIPSTORE_H
#define CLIPSTORE_H

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QVector>

// 一条剪贴板/中转站历史记录。
struct ClipRecord
{
    qint64     id     = -1;
    int        kind   = 0;      // ClipStore::TextKind / ImageKind / FileKind
    QString    hash;            // 内容指纹（去重键）
    QString    title;           // 展示名（文本摘要 / 文件名）
    QString    text;            // kind=Text：文本内容
    QString    path;            // kind=File：文件绝对路径
    QByteArray png;             // kind=Image：PNG 原始字节
    qint64     bytes  = 0;      // 内容大小（文本字节数 / 文件大小 / 图片字节数）
    qint64     usedAt = 0;      // 最近一次使用时间（ms since epoch），列表按它倒序
};

// 剪贴板历史（SQLite 持久化）。
//
// 设计要点：
//   * 走 Qt 的 QSqlDatabase + QSQLITE 驱动。构建时若 `qtHaveModule(sql)` 不成立
//     （或运行时装不到 QSQLITE 插件 → Debian/Ubuntu 需要 libqt6sql6-sqlite），
//     自动退化成"不持久化"：open() 返回 false、其余接口安全空转，
//     面板功能完全不受影响，只是重启后历史不保留。
//   * 图片以 PNG 二进制存库（不依赖临时文件）：系统清理临时目录、重启都不会丢，
//     读出时再由调用方落回临时文件供图标/拖拽使用。
//   * 去重靠 hash 的 UNIQUE 索引：重复内容只刷新 used_at（提升到最前），不新增行，
//     与面板里的去重行为保持一致。
class ClipStore : public QObject
{
    Q_OBJECT
public:
    enum Kind { TextKind = 0, ImageKind = 1, FileKind = 2 };
    enum { DefaultKeep = 200 };        // 历史条数上限

    explicit ClipStore(QObject *parent = nullptr);
    ~ClipStore() override;

    // 打开（或创建）数据库；返回 false 时 isReady() 为 false，调用方可继续用内存模式
    bool open(const QString &dbPath);
    bool isReady() const { return m_ready; }
    QString lastError() const { return m_lastError; }

    // 写入（同 hash 只刷新 used_at）。hash 为空表示不去重。
    // 返回该行的 id（>0）；失败返回 -1。调用方把 id 记在条目上，删除时才能同步删库。
    qint64 put(int kind, const QString &hash, const QString &title,
               const QString &text, const QString &path,
               const QByteArray &png, qint64 bytes);

    // 最近 limit 条，按最近使用时间倒序（不带 png 大字段，按需再取）
    QVector<ClipRecord> recent(int limit) const;
    // 按 id 取单条的图片数据（列表回填时按需拿，避免一次性读一堆大字段）
    QByteArray pngOf(qint64 id) const;

    bool remove(qint64 id);
    // 只保留最近 keep 条
    void prune(int keep = DefaultKeep);

private:
    bool  m_ready = false;
    QString m_lastError;
    QString m_connName;     // 独立连接名，避免和别的 QSqlDatabase 抢默认连接
};

#endif // CLIPSTORE_H
