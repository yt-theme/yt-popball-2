#include "clipstore.h"

#ifdef POPBALL2_HAVE_QT_SQL
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#endif

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QDebug>

ClipStore::ClipStore(QObject *parent)
    : QObject(parent)
{
    // 连接名要唯一：同一个进程里可能同时存在多份（设置窗口预览、自检程序等）
    m_connName = QStringLiteral("popball2_clip_%1")
                     .arg(reinterpret_cast<quintptr>(this), 0, 16);
}

ClipStore::~ClipStore()
{
#ifdef POPBALL2_HAVE_QT_SQL
    if (!m_connName.isEmpty()) {
        {
            // QSqlDatabase 必须在使用它的作用域内销毁，再 removeDatabase 才不会告警
            QSqlDatabase db = QSqlDatabase::database(m_connName, /*open=*/false);
            if (db.isOpen())
                db.close();
        }
        QSqlDatabase::removeDatabase(m_connName);
    }
#endif
}

bool ClipStore::open(const QString &dbPath)
{
#ifdef POPBALL2_HAVE_QT_SQL
    if (dbPath.trimmed().isEmpty()) {
        m_lastError = tr("数据库路径为空");
        return false;
    }
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        // 常见原因：Debian/Ubuntu 少了 libqt6sql6-sqlite（只装了 qt6-base）
        m_lastError = tr("缺少 QSQLITE 驱动（Debian/Ubuntu 请装 libqt6sql6-sqlite）");
        qWarning() << "[ClipStore]" << m_lastError;
        return false;
    }

    const QFileInfo fi(dbPath);
    if (!fi.absolutePath().isEmpty())
        QDir().mkpath(fi.absolutePath());

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connName);
    db.setDatabaseName(dbPath);
    if (!db.open()) {
        m_lastError = db.lastError().text();
        qWarning() << "[ClipStore] 打开数据库失败:" << m_lastError;
        return false;
    }

    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));      // 并发读更稳、写入更省事
    q.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));

    const QString ddl = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS clips ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  kind       INTEGER NOT NULL,"
        "  hash       TEXT    NOT NULL UNIQUE,"
        "  title      TEXT    NOT NULL DEFAULT '',"
        "  text       TEXT,"
        "  path       TEXT,"
        "  png        BLOB,"
        "  bytes      INTEGER NOT NULL DEFAULT 0,"
        "  used_at    INTEGER NOT NULL,"
        "  created_at INTEGER NOT NULL)");
    if (!q.exec(ddl)) {
        m_lastError = q.lastError().text();
        qWarning() << "[ClipStore] 建表失败:" << m_lastError;
        return false;
    }
    if (!q.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_clips_used ON clips(used_at DESC)"))) {
        // 索引建不出来不算致命
        qWarning() << "[ClipStore] 建索引失败:" << q.lastError().text();
    }

    m_ready = true;
    return true;
#else
    Q_UNUSED(dbPath);
    m_lastError = tr("编译时未启用 QtSql（qtHaveModule(sql) 不成立，历史不持久化）");
    return false;
#endif
}

qint64 ClipStore::put(int kind, const QString &hash, const QString &title,
                      const QString &text, const QString &path,
                      const QByteArray &png, qint64 bytes)
{
    if (!m_ready)
        return -1;
#ifdef POPBALL2_HAVE_QT_SQL
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // 同 hash 直接 UPSERT：只把 used_at 顶到最新（重复内容不新增行，提到最前）
    const QString sql = QStringLiteral(
        "INSERT INTO clips(kind,hash,title,text,path,png,bytes,used_at,created_at)"
        " VALUES(:kind,:hash,:title,:text,:path,:png,:bytes,:used,:created)"
        " ON CONFLICT(hash) DO UPDATE SET"
        "   kind=excluded.kind, title=excluded.title, text=excluded.text,"
        "   path=excluded.path, png=excluded.png, bytes=excluded.bytes,"
        "   used_at=excluded.used_at");

    QSqlDatabase db = QSqlDatabase::database(m_connName, false);
    QSqlQuery q(db);
    q.prepare(sql);
    q.bindValue(QStringLiteral(":kind"),    kind);
    q.bindValue(QStringLiteral(":hash"),    hash);
    q.bindValue(QStringLiteral(":title"),   title);
    q.bindValue(QStringLiteral(":text"),    text.isEmpty() ? QVariant() : text);
    q.bindValue(QStringLiteral(":path"),    path.isEmpty() ? QVariant() : path);
    q.bindValue(QStringLiteral(":png"),     png.isEmpty() ? QVariant() : png);
    q.bindValue(QStringLiteral(":bytes"),   bytes);
    q.bindValue(QStringLiteral(":used"),    now);
    q.bindValue(QStringLiteral(":created"), now);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        qWarning() << "[ClipStore] 写入失败:" << m_lastError;
        return -1;
    }

    // 回查 id：UPSERT 走的是 UPDATE 分支时 lastInsertId() 不会更新，
    // 所以统一按 hash 查一次，保证"条目 → 库行"的对应关系永远准确。
    QSqlQuery q2(db);
    q2.prepare(QStringLiteral("SELECT id FROM clips WHERE hash=:hash"));
    q2.bindValue(QStringLiteral(":hash"), hash);
    if (q2.exec() && q2.next())
        return q2.value(0).toLongLong();
    return -1;
#else
    Q_UNUSED(kind); Q_UNUSED(hash); Q_UNUSED(title); Q_UNUSED(text);
    Q_UNUSED(path); Q_UNUSED(png);  Q_UNUSED(bytes);
    return -1;
#endif
}

QVector<ClipRecord> ClipStore::recent(int limit) const
{
    QVector<ClipRecord> out;
    if (!m_ready || limit <= 0)
        return out;
#ifdef POPBALL2_HAVE_QT_SQL
    QSqlDatabase db = QSqlDatabase::database(m_connName, false);
    QSqlQuery q(db);
    // 列表回填不取 png 大字段（图片按需用 pngOf() 取），避免一次性读一堆二进制
    q.prepare(QStringLiteral(
        "SELECT id,kind,hash,title,text,path,bytes,used_at FROM clips"
        " ORDER BY used_at DESC LIMIT :n"));
    q.bindValue(QStringLiteral(":n"), limit);
    if (!q.exec()) {
        qWarning() << "[ClipStore] 读取失败:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        ClipRecord r;
        r.id     = q.value(0).toLongLong();
        r.kind   = q.value(1).toInt();
        r.hash   = q.value(2).toString();
        r.title  = q.value(3).toString();
        r.text   = q.value(4).toString();
        r.path   = q.value(5).toString();
        r.bytes  = q.value(6).toLongLong();
        r.usedAt = q.value(7).toLongLong();
        out.append(r);
    }
#endif
    return out;
}

QByteArray ClipStore::pngOf(qint64 id) const
{
#ifdef POPBALL2_HAVE_QT_SQL
    if (!m_ready || id < 0)
        return QByteArray();
    QSqlDatabase db = QSqlDatabase::database(m_connName, false);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT png FROM clips WHERE id=:id"));
    q.bindValue(QStringLiteral(":id"), id);
    if (q.exec() && q.next())
        return q.value(0).toByteArray();
#else
    Q_UNUSED(id);
#endif
    return QByteArray();
}

bool ClipStore::remove(qint64 id)
{
    if (!m_ready || id < 0)
        return false;
#ifdef POPBALL2_HAVE_QT_SQL
    QSqlDatabase db = QSqlDatabase::database(m_connName, false);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM clips WHERE id=:id"));
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec()) {
        qWarning() << "[ClipStore] 删除失败:" << q.lastError().text();
        return false;
    }
    return true;
#else
    Q_UNUSED(id);
    return false;
#endif
}

void ClipStore::prune(int keep)
{
    if (!m_ready)
        return;
#ifdef POPBALL2_HAVE_QT_SQL
    if (keep <= 0)
        keep = DefaultKeep;
    QSqlDatabase db = QSqlDatabase::database(m_connName, false);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "DELETE FROM clips WHERE id NOT IN"
        " (SELECT id FROM clips ORDER BY used_at DESC LIMIT :n)"));
    q.bindValue(QStringLiteral(":n"), keep);
    if (!q.exec())
        qWarning() << "[ClipStore] 清理旧记录失败:" << q.lastError().text();
#else
    Q_UNUSED(keep);
#endif
}
