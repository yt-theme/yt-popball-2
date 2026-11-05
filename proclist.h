#ifndef PROCLIST_H
#define PROCLIST_H

#include <QHash>
#include <QList>
#include <QString>

// 单个进程的一拍采样
struct ProcSample {
    int     pid = 0;
    QString name;
    double  cpuPercent = 0.0;   // 瞬时 CPU 占用 %（多核/多线程进程可 >100，等同 top 的 %CPU）
    double  memMb      = 0.0;   // 常驻内存，MB
    double  gpuPercent = 0.0;   // GPU 占用 %，-1 表示不可用（整机 GPU 都拿不到时）
};

// 跨平台进程采样器：维护上一拍的 CPU 时间基准，用「两拍差值」算瞬时 CPU%。
// 每进程 GPU 占用只有 Windows 能精确拿到；Linux(/proc) 与 macOS 没有公开的
// 每进程 GPU 接口，这里退化为「按 CPU 权重把整机 GPU 占用分摊到各进程」的估算值。
class ProcessSampler
{
public:
    ProcessSampler() = default;

    // 采样一次。wholeGpuPercent 传整机 GPU 占用 %（0~100）；<0 表示拿不到 GPU。
    // 首次调用只建立基准、返回空列表（还差一拍的差值）。
    QList<ProcSample> sample(double wholeGpuPercent);

    // 清空增量基准：面板重新显示 / 配置变更时调用，避免用陈旧基准算出异常的瞬时值。
    void reset();

private:
    qint64 m_lastMs = 0;

#if defined(Q_OS_WIN)
    struct WinBase { qulonglong kernel = 0; qulonglong user = 0; };
    QHash<quint32, WinBase> m_lastCpu;
#elif defined(Q_OS_MACOS)
    QHash<int, qulonglong> m_lastCpu;   // pid -> 累计(user+sys) CPU 时间，微秒
#elif defined(Q_OS_LINUX)
    QHash<int, qulonglong> m_lastCpu;   // pid -> 累计(user+sys) jiffies
#endif
};

#endif // PROCLIST_H