#include "proclist.h"

#include <QDateTime>
#include <QFile>
#include <QVector>
#include <cstring>

// 读一个小文件全文（/proc 下都是几十字节~几 KB 的文本），失败返回空串。
static QString readSmallFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    const QByteArray b = f.read(8192);
    f.close();
    return QString::fromLocal8Bit(b);
}

#if defined(Q_OS_LINUX)

#include <QDir>
#include <QStringList>
#include <unistd.h>

// Linux：进程都来自 /proc。CPU 用 /proc/<pid>/stat 的 utime+stime 两拍差值；
// 内存用 /proc/<pid>/statm 的 resident 页数；每进程 GPU 无接口 → 按 CPU 权重估算。
QList<ProcSample> ProcessSampler::sample(double wholeGpuPercent)
{
    QList<ProcSample> out;

    long ticksPerSec = sysconf(_SC_CLK_TCK);
    if (ticksPerSec <= 0) ticksPerSec = 100;
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) pageSize = 4096;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool haveBase = (m_lastMs != 0);
    const double dtSec = haveBase ? (now - m_lastMs) / 1000.0 : 0.0;
    m_lastMs = now;

    QHash<int, qulonglong> curJiffies;
    QHash<int, QString>    names;
    QHash<int, double>     memMb;

    const QDir procd(QStringLiteral("/proc"));
    const QStringList entries = procd.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &e : entries) {
        bool ok = false;
        const int pid = e.toInt(&ok);
        if (!ok) continue;
        const QString base = QStringLiteral("/proc/%1/").arg(pid);

        const QString comm = readSmallFile(base + QStringLiteral("comm")).trimmed();
        if (comm.isEmpty()) continue;
        names.insert(pid, comm);

        // /proc/<pid>/stat：comm 字段带括号且内部可能有空格，取最后一个 ')' 之后的部分。
        const QString stat = readSmallFile(base + QStringLiteral("stat"));
        const int rp = stat.lastIndexOf(QLatin1Char(')'));
        if (rp < 0) continue;
        const QStringList tk = stat.mid(rp + 1).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        // comm 之后：state, ppid, ...,（下标 11/12 分别是 utime/stime）
        if (tk.size() <= 12) continue;
        const qulonglong utime = tk.at(11).toULongLong();
        const qulonglong stime = tk.at(12).toULongLong();
        curJiffies.insert(pid, utime + stime);

        // /proc/<pid>/statm：第 2 项是 resident 页数
        const QStringList sm = readSmallFile(base + QStringLiteral("statm"))
                                   .split(QLatin1Char(' '), Qt::SkipEmptyParts);
        double mem = 0.0;
        if (sm.size() >= 2) {
            const qulonglong resPages = sm.at(1).toULongLong();
            mem = (resPages * (unsigned long)pageSize) / (1024.0 * 1024.0);
        }
        memMb.insert(pid, mem);
    }

    if (!haveBase) {
        // 首次没有历史基准：先把进程名/内存直接返回，CPU/GPU 暂记 0，
        // 让列表第一拍立刻有内容（下一拍再用两拍差值算出真实占用）。
        m_lastCpu = curJiffies;
        const double gpu0 = (wholeGpuPercent < 0.0) ? -1.0 : 0.0;
        for (auto it = curJiffies.constBegin(); it != curJiffies.constEnd(); ++it) {
            ProcSample s;
            s.pid = it.key();
            s.name = names.value(it.key());
            s.cpuPercent = 0.0;
            s.memMb = memMb.value(it.key(), 0.0);
            s.gpuPercent = gpu0;
            out.append(s);
        }
        return out;
    }

    double sumCpu = 0.0;
    for (auto it = curJiffies.constBegin(); it != curJiffies.constEnd(); ++it) {
        const qulonglong prev = m_lastCpu.value(it.key(), 0);
        const qulonglong delta = it.value() > prev ? it.value() - prev : 0;
        const double cpu = delta / (double(ticksPerSec) * dtSec) * 100.0;
        ProcSample s;
        s.pid = it.key();
        s.name = names.value(it.key());
        s.cpuPercent = cpu;
        s.memMb = memMb.value(it.key(), 0.0);
        s.gpuPercent = -1.0;   // 后面统一估
        sumCpu += cpu;
        out.append(s);
    }
    m_lastCpu = curJiffies;

    // GPU 估算：把整机 GPU 占用按各进程 CPU 占比分摊（没有每进程 GPU 接口）。
    const double denom = sumCpu > 0.0 ? sumCpu : 1.0;
    for (ProcSample &s : out) {
        s.gpuPercent = (wholeGpuPercent < 0.0)
                           ? -1.0
                           : wholeGpuPercent * (s.cpuPercent / denom);
    }
    return out;
}

void ProcessSampler::reset() { m_lastMs = 0; m_lastCpu.clear(); }

#elif defined(Q_OS_WIN)

// 与 sysInfo.h 保持一致的 SDK 版本门槛：PROCESS_QUERY_LIMITED_INFORMATION /
// PROCESS_MEMORY_COUNTERS_EX 需要 Vista+(0x0600)；老 MinGW 默认版本较低会报未声明。
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX                 // 避免 min/max 宏与 Qt 冲突
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

// Windows：Toolhelp 快照枚举 + GetProcessTimes(CPU 两拍差值) + GetProcessMemoryInfo(内存)。
// 每进程 GPU 不做 PDH 逐进程（估算策略与其它平台一致，保持列统一）。
QList<ProcSample> ProcessSampler::sample(double wholeGpuPercent)
{
    QList<ProcSample> out;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool haveBase = (m_lastMs != 0);
    const double dtMs = haveBase ? double(now - m_lastMs) : 0.0;
    m_lastMs = now;

    QHash<quint32, WinBase> cur;
    QHash<quint32, QString> names;
    QHash<quint32, double>  memMb;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return out;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            const DWORD pid = pe.th32ProcessID;
            if (pid == 0) continue;

            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (h == nullptr) continue;

            WinBase wb;
            FILETIME createT, exitT, kernelT, userT;
            if (GetProcessTimes(h, &createT, &exitT, &kernelT, &userT)) {
                wb.kernel = (qulonglong(kernelT.dwHighDateTime) << 32) | kernelT.dwLowDateTime;
                wb.user   = (qulonglong(userT.dwHighDateTime)   << 32) | userT.dwLowDateTime;
            }

            double mem = 0.0;
            PROCESS_MEMORY_COUNTERS_EX pmc;
            if (GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc),
                                     sizeof(pmc)))
                mem = pmc.WorkingSetSize / (1024.0 * 1024.0);

            CloseHandle(h);

            cur.insert(pid, wb);
            names.insert(pid, QString::fromWCharArray(pe.szExeFile));
            memMb.insert(pid, mem);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    if (!haveBase) {
        // 首次没有历史基准：先把进程名/内存直接返回，CPU/GPU 暂记 0，
        // 让列表第一拍立刻有内容（下一拍再用两拍差值算出真实占用）。
        m_lastCpu = cur;
        const double gpu0 = (wholeGpuPercent < 0.0) ? -1.0 : 0.0;
        for (auto it = cur.constBegin(); it != cur.constEnd(); ++it) {
            ProcSample s;
            s.pid = int(it.key());
            s.name = names.value(it.key());
            s.cpuPercent = 0.0;
            s.memMb = memMb.value(it.key(), 0.0);
            s.gpuPercent = gpu0;
            out.append(s);
        }
        return out;
    }

    double sumCpu = 0.0;
    for (auto it = cur.constBegin(); it != cur.constEnd(); ++it) {
        const WinBase prev = m_lastCpu.value(it.key());
        const qulonglong dk = it.value().kernel > prev.kernel ? it.value().kernel - prev.kernel : 0;
        const qulonglong du = it.value().user   > prev.user   ? it.value().user   - prev.user   : 0;
        const qulonglong d100ns = dk + du;
        ProcSample s;
        s.pid = int(it.key());
        s.name = names.value(it.key());
        s.cpuPercent = (dtMs > 0.0) ? double(d100ns) / (dtMs * 10000.0) * 100.0 : 0.0;
        s.memMb = memMb.value(it.key(), 0.0);
        s.gpuPercent = -1.0;
        sumCpu += s.cpuPercent;
        out.append(s);
    }
    m_lastCpu = cur;

    const double denom = sumCpu > 0.0 ? sumCpu : 1.0;
    for (ProcSample &s : out) {
        s.gpuPercent = (wholeGpuPercent < 0.0)
                           ? -1.0
                           : wholeGpuPercent * (s.cpuPercent / denom);
    }
    return out;
}

void ProcessSampler::reset() { m_lastMs = 0; m_lastCpu.clear(); }

#elif defined(Q_OS_MACOS)

#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/resource.h>

// macOS：proc_listpids 枚举 + proc_pid_rusage(CPU 两拍差值) + proc_pidinfo(常驻内存)。
// 每进程 GPU 无公开接口 → 按 CPU 权重把整机 GPU 占用分摊估算。
QList<ProcSample> ProcessSampler::sample(double wholeGpuPercent)
{
    QList<ProcSample> out;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool haveBase = (m_lastMs != 0);
    const double dtSec = haveBase ? (now - m_lastMs) / 1000.0 : 0.0;
    m_lastMs = now;

    const int nbytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (nbytes <= 0)
        return out;
    const int count = nbytes / int(sizeof(pid_t));
    QVector<pid_t> pids(count);
    const int filled = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), nbytes);
    const int realCount = filled / int(sizeof(pid_t));

    QHash<int, qulonglong> cur;
    QHash<int, QString>    names;
    QHash<int, double>     memMb;

    for (int i = 0; i < realCount; ++i) {
        const pid_t pid = pids[i];
        if (pid <= 0) continue;

        char namebuf[256];
        const int nlen = proc_name(pid, namebuf, sizeof(namebuf));
        const QString name = (nlen > 0) ? QString::fromLocal8Bit(namebuf, nlen)
                                        : QStringLiteral("?");
        names.insert(int(pid), name);

        struct rusage_info_v4 ri;
        memset(&ri, 0, sizeof(ri));
        if (proc_pid_rusage(pid, RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t *>(&ri)) == 0) {
            // rusage_info_v4 的 ri_user_time / ri_system_time 已是 uint64_t 微秒，
            // 直接相加即可（老 SDK 里它们是 timeval，新 SDK 已改为累计微秒）。
            cur.insert(int(pid), qulonglong(ri.ri_user_time) + qulonglong(ri.ri_system_time));
        }

        struct proc_taskinfo ti;
        memset(&ti, 0, sizeof(ti));
        if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) == int(sizeof(ti)))
            memMb.insert(int(pid), ti.pti_resident_size / (1024.0 * 1024.0));
    }

    if (!haveBase) {
        // 首次没有历史基准：先把进程名/内存直接返回，CPU/GPU 暂记 0，
        // 让列表第一拍立刻有内容（下一拍再用两拍差值算出真实占用）。
        m_lastCpu = cur;
        const double gpu0 = (wholeGpuPercent < 0.0) ? -1.0 : 0.0;
        for (auto it = cur.constBegin(); it != cur.constEnd(); ++it) {
            ProcSample s;
            s.pid = int(it.key());
            s.name = names.value(it.key());
            s.cpuPercent = 0.0;
            s.memMb = memMb.value(it.key(), 0.0);
            s.gpuPercent = gpu0;
            out.append(s);
        }
        return out;
    }

    double sumCpu = 0.0;
    for (auto it = cur.constBegin(); it != cur.constEnd(); ++it) {
        const qulonglong prev = m_lastCpu.value(it.key(), 0);
        const qulonglong delta = it.value() > prev ? it.value() - prev : 0;
        const double cpu = double(delta) / (dtSec * 1000000.0) * 100.0;
        ProcSample s;
        s.pid = it.key();
        s.name = names.value(it.key());
        s.cpuPercent = cpu;
        s.memMb = memMb.value(it.key(), 0.0);
        s.gpuPercent = -1.0;
        sumCpu += cpu;
        out.append(s);
    }
    m_lastCpu = cur;

    const double denom = sumCpu > 0.0 ? sumCpu : 1.0;
    for (ProcSample &s : out) {
        s.gpuPercent = (wholeGpuPercent < 0.0)
                           ? -1.0
                           : wholeGpuPercent * (s.cpuPercent / denom);
    }
    return out;
}

void ProcessSampler::reset() { m_lastMs = 0; m_lastCpu.clear(); }

#else

QList<ProcSample> ProcessSampler::sample(double) { return {}; }
void ProcessSampler::reset() { m_lastMs = 0; }

#endif