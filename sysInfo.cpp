#include "sysInfo.h"

#include <QRegularExpression>
#include <QHash>
#include <QSysInfo>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QStandardPaths>
#include <cstring>
#include <cwchar>

#if defined(Q_OS_MACOS)
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

/* =====================================================================================
 *                                  公共小工具
 * ===================================================================================== */
namespace {

#if defined(Q_OS_LINUX)
// 读取文本文件并去掉首尾空白；失败返回空串
QString readTextFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromLatin1(f.readAll()).trimmed();
}

// ---------------------------------------------------------------------------------------
//  Linux：跨发行版 / 跨芯片架构的 CPU 温度与频率探测
//
//  温度源在不同发行版与 SoC 上差别极大，只按 temp1_input 取最大值会把 NVMe、GPU、
//  电池的温度当成 CPU。这里给每个候选源打「CPU 相似度」分，取并列最高分的一组再取最大值。
//
//  已覆盖的典型命名（节选）：
//    Intel x86   : hwmon=coretemp                       zone=x86_pkg_temp / acpitz
//    AMD   x86   : hwmon=k10temp / zenpower / k8temp     zone=k10temp
//    ARM   通用  : hwmon=cpu_thermal / soc_thermal       zone=cpu-thermal / soc-thermal
//    ARM big.LITTLE : zone=big0-thermal / little0-thermal / cluster0
//    树莓派      : hwmon=cpu_thermal                      zone=cpu-thermal
//    Rockchip    : hwmon=soc_thermal / rk_thermal         zone=cpu-thermal / soc-thermal
//    Allwinner   : hwmon=cpu_thermal                      zone=cpu_thermal
//    高通骁龙    : hwmon=qcom-tsens / tsens               zone=cpu0-0-0(逐核) / cpu4-0-0
//    联发科天玑  : hwmon=mtk_thermal                      zone=mtktscpu / mtktsAP
//    三星 Exynos : hwmon=exynos_thermal                   zone=cpu0-thermal
//    英伟达 Tegra: hwmon=tegra_thermal                    zone=cpu-therm / soc0-therm
//    IBM POWER   : hwmon=powernv / occ / ibmpowernv      （PowerVM 虚拟机通常无温度传感器）
//    IBM Z(s390x): 无温度传感器 → 标记不可用，UI 不显示
// ---------------------------------------------------------------------------------------

// 列出目录下所有 tempN_input 文件
QStringList listTempInputs(const QString &dir)
{
    QStringList out;
    const QStringList entries =
        QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System);
    for (const QString &e : entries) {
        if (e.startsWith(QLatin1String("temp")) && e.endsWith(QLatin1String("_input")))
            out << (dir + QLatin1Char('/') + e);
    }
    return out;
}

bool containsAny(const QString &lower, const char *const *needles)
{
    for (int i = 0; needles[i]; ++i)
        if (lower.contains(QLatin1String(needles[i])))
            return true;
    return false;
}

// 明确不是 CPU 的传感器（GPU / 硬盘 / 无线 / 电池 …）——一律不采用，
// 宁可显示「不可用」，也不把硬盘温度当成 CPU 温度。
bool isDefinitelyNotCpu(const QString &lower)
{
    static const char *kNever[] = {
        "gpu", "nvme", "wifi", "wlan", "ssd", "battery", "charger", "bluetooth",
        "keyboard", "touchpad", "amdgpu", "nouveau", "i915", "radeon",
        "display", "camera", "modem", "ddr", "nsp", nullptr
    };
    return containsAny(lower, kNever);
}

// hwmon 芯片名的 CPU 相似度：100=基本确定是 CPU，50=片上/SoC 温度，1=未知兜底，0=明确不是
int scoreHwmonName(const QString &name)
{
    if (name.isEmpty())
        return 1;
    const QString l = name.toLower();
    if (isDefinitelyNotCpu(l))
        return 0;

    static const char *kStrong[] = {
        "coretemp", "k10temp", "zenpower", "k8temp", "peci",
        "cpu", "pkg", "core", "tsens", "powernv", "occ", nullptr
    };
    if (containsAny(l, kStrong))
        return 100;

    static const char *kSoc[] = {
        "soc", "thermal", "mtk", "imx", "rk", "sunxi", "qcom", "tegra",
        "exynos", "amlogic", "rockchip", "mediatek", "stm", "ingenic",
        "allwinner", nullptr
    };
    if (containsAny(l, kSoc))
        return 50;

    return 1;
}

// thermal_zone 的 type 的 CPU 相似度
int scoreThermalType(const QString &type)
{
    if (type.isEmpty())
        return 1;
    const QString l = type.toLower();
    if (isDefinitelyNotCpu(l))
        return 0;

    // 逐核 / 集群 / 封装：cpu0-0-0(骁龙)、cpu-thermal(树莓派)、big0-thermal(big.LITTLE)、
    // x86_pkg_temp、mtktscpu(天玑)、cpu0-thermal(Exynos)、cpu-therm(Tegra)
    static const char *kStrong[] = {
        "cpu", "pkg", "big", "little", "cluster", "core", "mtktscpu", "mtktsap", nullptr
    };
    if (containsAny(l, kStrong))
        return 100;

    static const char *kSoc[] = { "soc", "thermal", "board", "ap-therm", "ap_thermal", nullptr };
    if (containsAny(l, kSoc))
        return 50;

    // acpitz：通用 ACPI 热区，很多笔记本 / 虚拟机只有它，可以接受
    if (l.contains(QLatin1String("acpitz")))
        return 40;

    return 1;   // tsens_tz_sensorN、quiet-therm、xo-therm 等，仅作兜底
}

// 探测 CPU 温度来源：给所有候选打分，返回并列最高分的那组文件路径
QStringList probeCpuTempPaths()
{
    QList<QPair<int, QString>> candidates;   // (score, path)

    // /sys/class/hwmon/hwmonX/  —— 按芯片名
    const QStringList hwmons =
        QDir(QStringLiteral("/sys/class/hwmon"))
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System);
    for (const QString &d : hwmons) {
        const QString base = QStringLiteral("/sys/class/hwmon/") + d;
        const QStringList inputs = listTempInputs(base);
        if (inputs.isEmpty())
            continue;
        const int sc = scoreHwmonName(readTextFile(base + QStringLiteral("/name")));
        if (sc <= 0)
            continue;
        for (const QString &p : inputs)
            candidates.append(qMakePair(sc, p));
    }

    // /sys/class/thermal/thermal_zoneX/  —— 按热区类型
    const QStringList zones =
        QDir(QStringLiteral("/sys/class/thermal"))
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System);
    for (const QString &z : zones) {
        if (!z.startsWith(QLatin1String("thermal_zone")))
            continue;
        const QString base = QStringLiteral("/sys/class/thermal/") + z;
        const int sc = scoreThermalType(readTextFile(base + QStringLiteral("/type")));
        if (sc <= 0)
            continue;
        candidates.append(qMakePair(sc, base + QStringLiteral("/temp")));
    }

    if (candidates.isEmpty())
        return QStringList();

    int best = 0;
    for (const auto &c : candidates)
        best = qMax(best, c.first);

    QStringList out;
    for (const auto &c : candidates)
        if (c.first == best)
            out << c.second;
    return out;
}

// 探测 CPU 当前频率（MHz）—— 覆盖 x86_64 / ARM64 / POWER / RISC-V / LoongArch 等
bool probeCpuFrequencyMHz(double *outMHz)
{
    // cpufreq 有两种目录布局，都要处理：
    //   逐 CPU：/sys/devices/system/cpu/cpuN/cpufreq/     （老内核、ARM、x86 常见）
    //   按策略：/sys/devices/system/cpu/cpufreq/policyN/  （新内核、IBM POWER 等用这种）
    QStringList cpuDirs;
    QStringList policyDirs;

    const QStringList entries =
        QDir(QStringLiteral("/sys/devices/system/cpu"))
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System);
    for (const QString &e : entries) {
        // 注意：目录名 "cpufreq" 同样以 "cpu" 开头，必须先单独判断，
        // 否则会被下面的逐 CPU 分支吞掉，policy 布局就永远读不到。
        if (e == QLatin1String("cpufreq")) {
            const QStringList policies =
                QDir(QStringLiteral("/sys/devices/system/cpu/cpufreq"))
                    .entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System);
            for (const QString &p : policies)
                if (p.startsWith(QLatin1String("policy")))
                    policyDirs << (QStringLiteral("/sys/devices/system/cpu/cpufreq/") + p);
            continue;
        }
        if (e.startsWith(QLatin1String("cpu")) && e.size() >= 4) {
            bool digits = true;
            for (int i = 3; i < e.size(); ++i)
                if (!e.at(i).isDigit()) { digits = false; break; }
            if (digits)
                cpuDirs << (QStringLiteral("/sys/devices/system/cpu/") + e
                            + QStringLiteral("/cpufreq"));
        }
    }

    auto maxFrom = [](const QStringList &dirs, const QStringList &files) -> double {
        double best = -1.0;
        for (const QString &d : dirs)
            for (const QString &f : files) {
                const double v = readTextFile(d + QLatin1Char('/') + f).toDouble();
                if (v > best)
                    best = v;
            }
        return best;
    };

    const QStringList curFiles = { QStringLiteral("scaling_cur_freq"),
                                   QStringLiteral("cpuinfo_cur_freq") };
    const QStringList maxFiles = { QStringLiteral("cpuinfo_max_freq"),
                                   QStringLiteral("scaling_max_freq") };

    // 1) 当前频率（kHz）—— 取所有核里最大的（big.LITTLE 下即大核频率）
    double cur = maxFrom(cpuDirs, curFiles);
    if (cur <= 0)
        cur = maxFrom(policyDirs, curFiles);
    if (cur > 0) { *outMHz = cur / 1000.0; return true; }

    // 2) /proc/cpuinfo
    //    x86 / 新 ARM : "cpu MHz : 3600.000"
    //    IBM POWER    : "clock   : 3800.000000MHz"
    double fromCpuinfo = -1.0;
    double fromClock   = -1.0;
    const QStringList lines =
        readTextFile(QStringLiteral("/proc/cpuinfo")).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        const QString key = line.left(colon).trimmed().toLower();

        if (key == QLatin1String("cpu mhz")) {
            const double v = line.mid(colon + 1).trimmed().toDouble();
            if (v > fromCpuinfo)
                fromCpuinfo = v;
        } else if (key.contains(QLatin1String("clock"))) {
            // IBM POWER / 部分 ARM 板子，形如 "clock : 3800.000000MHz"
            QString s = line.mid(colon + 1).trimmed();
            const bool ghz = s.contains(QLatin1String("GHz"), Qt::CaseInsensitive);
            s.remove(QLatin1String("MHz"), Qt::CaseInsensitive);
            s.remove(QLatin1String("GHz"), Qt::CaseInsensitive);
            double v = s.trimmed().toDouble();
            if (ghz)
                v *= 1000.0;
            if (v > fromClock)
                fromClock = v;
        }
    }
    if (fromCpuinfo > 0) { *outMHz = fromCpuinfo; return true; }
    if (fromClock   > 0) { *outMHz = fromClock;   return true; }

    // 3) 只能拿到额定最大频率
    double mx = maxFrom(cpuDirs, maxFiles);
    if (mx <= 0)
        mx = maxFrom(policyDirs, maxFiles);
    if (mx > 0) { *outMHz = mx / 1000.0; return true; }

    return false;   // 确实拿不到（Apple Silicon、IBM Z 等）→ 标记不可用，UI 不显示
}

// 是否虚拟 / 桥接 / 隧道网卡（统计流量时排除，避免重复计数）
bool isVirtualNetInterface(const QString &name)
{
    static const char *kPrefixes[] = {
        // 通用 Linux
        "lo", "veth", "docker", "br-", "virbr", "vnet", "tap", "tun", "utun",
        "wg", "zt", "dummy", "vmnet", "vboxnet", "bond", "bridge", "kube",
        "cni", "flannel", "cali", "lxc", "podman",
        // 移动端（骁龙 / 天玑等 Android 设备）：clat 与 v4-* 是 IPv6 转换隧道，
        // 与 rmnet_data* 同时统计会重复计数
        "sit", "ip6tnl", "p2p", "clat", "v4-", "rmnet_ipa",
        nullptr
    };
    for (int i = 0; kPrefixes[i]; ++i)
        if (name.startsWith(QLatin1String(kPrefixes[i])))
            return true;
    return false;
}
#endif // Q_OS_LINUX

#if defined(Q_OS_MACOS)
// ---------------------------------------------------------------------------------------
//  macOS：AppleSMC 读取 CPU 温度
//
//  温度键在不同架构上完全不同，且数据类型也不同：
//    * Intel Mac ： TC0P / TC0D ...            类型 sp78（定点数）
//    * Apple Silicon： Tp*(性能核) / Te*(能效核) / Tc*(集群)  类型 flt（float32，小端）
//  注意 SMC 返回的 dataType 字段字节序与主机相反（" tlf" 实际是 "flt "）。
// ---------------------------------------------------------------------------------------

constexpr int kSmcKernelIndex    = 2;
constexpr int kSmcCmdReadBytes   = 5;
constexpr int kSmcCmdReadIndex   = 8;
constexpr int kSmcCmdReadKeyInfo = 9;

struct SmcVers    { char major, minor, build, reserved[1]; unsigned short release; };
struct SmcPLimit  { unsigned short version, length; unsigned int cpuPLimit, gpuPLimit, memPLimit; };
struct SmcKeyInfo { unsigned int dataSize, dataType; char dataAttributes; };
struct SmcKeyData {
    unsigned int  key;
    SmcVers       vers;
    SmcPLimit     pLimitData;
    SmcKeyInfo    keyInfo;
    char          result, status, data8;
    unsigned int  data32;
    unsigned char bytes[32];
};

kern_return_t smcCall(io_connect_t conn, int index, SmcKeyData *in, SmcKeyData *out)
{
    size_t outSize = sizeof(SmcKeyData);
    return IOConnectCallStructMethod(conn, index, in, sizeof(SmcKeyData), out, &outSize);
}

quint32 smcKeyFromStr(const char *s)
{
    return (quint32(uchar(s[0])) << 24) | (quint32(uchar(s[1])) << 16)
         | (quint32(uchar(s[2])) << 8)  |  quint32(uchar(s[3]));
}

void smcKeyToStr(char o[5], quint32 v)
{
    o[0] = char(v >> 24); o[1] = char(v >> 16);
    o[2] = char(v >> 8);  o[3] = char(v);
    o[4] = '\0';
}

bool smcKeyInfo(io_connect_t conn, quint32 key, unsigned int *size, char type[5])
{
    SmcKeyData in, out;
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.key   = key;
    in.data8 = kSmcCmdReadKeyInfo;
    if (smcCall(conn, kSmcKernelIndex, &in, &out) != kIOReturnSuccess || out.result != 0)
        return false;
    if (size) *size = out.keyInfo.dataSize;
    if (type) { memcpy(type, &out.keyInfo.dataType, 4); type[4] = '\0'; }
    return true;
}

bool smcReadBytes(io_connect_t conn, quint32 key, unsigned int size, unsigned char *buf)
{
    if (size == 0 || size > 32)
        return false;
    SmcKeyData in, out;
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.key              = key;
    in.keyInfo.dataSize = size;
    in.data8            = kSmcCmdReadBytes;
    if (smcCall(conn, kSmcKernelIndex, &in, &out) != kIOReturnSuccess || out.result != 0)
        return false;
    memcpy(buf, out.bytes, size);
    return true;
}

// 解析温度值，无法识别返回 -1
double smcParseTemp(const char *type, unsigned int size, const unsigned char *b)
{
    char t[5] = { type[3], type[2], type[1], type[0], '\0' };   // 字节序反转
    if (strncmp(t, "flt", 3) == 0 && size == 4) {
        float f; memcpy(&f, b, 4); return f;
    }
    if (strncmp(t, "sp78", 4) == 0 && size >= 2) {
        const short v = short((b[0] << 8) | b[1]); return v / 256.0;
    }
    if (strncmp(t, "ui8", 3) == 0 && size >= 1)
        return b[0];
    if (strncmp(t, "ui16", 4) == 0 && size >= 2)
        return (b[0] << 8) | b[1];
    return -1.0;
}

// 枚举 SMC 全部键，挑出 CPU 温度相关的键（只需在启动时做一次）
QList<quint32> smcDiscoverCpuTempKeys(io_connect_t conn)
{
    QList<quint32> keys;

    const quint32 keyCount = smcKeyFromStr("#KEY");
    unsigned int  ksize    = 0;
    char          ktype[5] = {0};
    unsigned char kb[32]   = {0};
    if (!smcKeyInfo(conn, keyCount, &ksize, ktype) || !smcReadBytes(conn, keyCount, ksize, kb))
        return keys;

    unsigned int total = 0;
    for (unsigned int i = 0; i < ksize && i < 4; ++i)
        total = (total << 8) | kb[i];
    if (total == 0 || total > 20000)
        return keys;

#if defined(__arm64__) || defined(__aarch64__)
    static const char *kPrefixes[] = { "Tp", "Te", "Tc", nullptr };            // Apple Silicon
#else
    static const char *kPrefixes[] = { "TC", "Tc", "Tp", "Te", nullptr };      // Intel Mac
#endif

    for (unsigned int i = 0; i < total; ++i) {
        SmcKeyData in, out;
        memset(&in, 0, sizeof(in));
        memset(&out, 0, sizeof(out));
        in.data8  = kSmcCmdReadIndex;
        in.data32 = i;
        if (smcCall(conn, kSmcKernelIndex, &in, &out) != kIOReturnSuccess)
            continue;

        char k[5];
        smcKeyToStr(k, out.key);
        bool match = false;
        for (int p = 0; kPrefixes[p] && !match; ++p)
            if (k[0] == kPrefixes[p][0] && k[1] == kPrefixes[p][1])
                match = true;
        if (!match)
            continue;

        unsigned int size = 0;
        char type[5] = {0};
        if (!smcKeyInfo(conn, out.key, &size, type))
            continue;
        if (size != 4 && size != 2)
            continue;
        keys.append(out.key);
    }
    return keys;
}

// 读取所有候选键，取最大值作为 CPU 温度
bool smcReadCpuTemperature(io_connect_t conn, const QList<quint32> &keys, double *outTemp)
{
    double best = -1.0;
    for (quint32 k : keys) {
        unsigned int size = 0;
        char type[5] = {0};
        if (!smcKeyInfo(conn, k, &size, type))
            continue;
        unsigned char buf[32] = {0};
        if (!smcReadBytes(conn, k, size, buf))
            continue;
        const double v = smcParseTemp(type, size, buf);
        if (v > 0.0 && v < 150.0 && v > best)
            best = v;
    }
    if (best < 0.0)
        return false;
    *outTemp = best;
    return true;
}
#endif // Q_OS_MACOS

#if defined(Q_OS_WIN)
// ---------------------------------------------------------------------------------------
//  Windows：CPU 频率 / 温度
//
//  * 频率：Windows 没有「用户态读实时频率」的公开 API，注册表里的 "~MHz" 是标称主频，
//          作为显示值足够；拿不到则标记不可用（UI 自动隐藏）。
//  * 温度：同样没有公开的「读 CPU 核心温度」API。常见的做法是查 WMI 的 ACPI 热区
//          MSAcpi_ThermalZoneTemperature（命名空间 root\WMI），它给的是热区温度（十分
//          之一开尔文），很多机器上是整机/主板温度而非核心温度，且虚拟机通常没有。
//          因此这里只把读数当温度来源：能读到就给，读不到就标记不可用，UI 自动隐藏，
//          绝不把假数据当成 CPU 温度。
// ---------------------------------------------------------------------------------------

bool probeWinCpuFreqMHz(double *outMHz)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;

    DWORD val = 0, type = 0, sz = sizeof(val);
    const LONG r = RegQueryValueExW(key, L"~MHz", nullptr, &type,
                                    reinterpret_cast<LPBYTE>(&val), &sz);
    RegCloseKey(key);

    if (r != ERROR_SUCCESS || val == 0)
        return false;
    *outMHz = static_cast<double>(val);
    return true;
}

bool probeWinCpuTempCelsius(double *outC)
{
    *outC = -1.0;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE)
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = (hr == S_OK || hr == S_FALSE);
    if (FAILED(hr))
        return false;

    // 本地 WMI 简单查询通常无需严格安全上下文；失败也无妨，继续查询。
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                         RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                         nullptr, EOAC_NONE, nullptr);

    IWbemLocator *wmiLocator = nullptr;
    const HRESULT hrCo = CoCreateInstance(
        CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
        reinterpret_cast<void **>(&wmiLocator));
    if (FAILED(hrCo)) {
        if (needUninit) CoUninitialize();
        return false;
    }

    bool ok = false;
    double best = -1.0;

    BSTR ns = SysAllocString(L"root\\WMI");
    IWbemServices *svc = nullptr;
    if (ns && wmiLocator->ConnectServer(ns, nullptr, nullptr, nullptr, 0,
                                        nullptr, nullptr, &svc) == WBEM_S_NO_ERROR) {
        BSTR lang  = SysAllocString(L"WQL");
        BSTR query = SysAllocString(
            L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature");
        IEnumWbemClassObject *enumObj = nullptr;
        if (lang && query
            && svc->ExecQuery(lang, query, WBEM_FLAG_FORWARD_ONLY, nullptr,
                              &enumObj) == WBEM_S_NO_ERROR) {
            IWbemClassObject *obj = nullptr;
            ULONG got = 0;
            while (enumObj->Next(WBEM_INFINITE, 1, &obj, &got) == WBEM_S_NO_ERROR && got) {
                VARIANT var;
                VariantInit(&var);
                if (obj->Get(L"CurrentTemperature", 0, &var, nullptr, nullptr) == WBEM_S_NO_ERROR) {
                    LONG raw = 0;
                    if (var.vt == VT_UI4)      raw = static_cast<LONG>(var.ulVal);
                    else if (var.vt == VT_I4)  raw = var.lVal;
                    if (raw > 0) {
                        const double c = static_cast<double>(raw) / 10.0 - 273.15;
                        if (c > 0.0 && c < 120.0 && c > best) best = c;
                    }
                }
                VariantClear(&var);
                obj->Release();
                obj = nullptr;
                got = 0;
            }
            enumObj->Release();
        }
        if (lang)  SysFreeString(lang);
        if (query) SysFreeString(query);
        svc->Release();
    }
    if (ns) SysFreeString(ns);

    wmiLocator->Release();
    if (needUninit) CoUninitialize();

    if (best > 0.0) { *outC = best; ok = true; }
    return ok;
}

// ---------------------------------------------------------------------------------------
//  Windows 优先温度源：LibreHardwareMonitor（LHM）的本地 Web 服务
//
//  Windows 没有公开的「读 CPU 核心温度」API，ACPI 热区(MSAcpi_ThermalZoneTemperature)
//  在多数台式机/虚拟机上不存在或只是主板温度。LHM 自带 ring-0 驱动，能读到 Intel/AMD
//  的真实核心/封装温度，并以 JSON 形式通过内置 Web 服务器暴露。
//  使用：运行 LibreHardwareMonitor.exe，勾选 Options → Web server（或命令行 /web），
//  默认监听 8085 端口，本程序即能从 http://localhost:8085/data.json 读到温度。
//  读不到时由下方 readWinCpuTempCelsius() 回退到原有 ACPI 热区温度。
// ---------------------------------------------------------------------------------------

// LHM Web 服务默认地址（如需改端口，改这里即可）
static const char *kLhmUrl = "http://localhost:8085/data.json";

// 同步 HTTP GET（仅 localhost，带超时）。成功返回 true 并把响应体写入 out。
bool winHttpGet(const QString &url, int timeoutMs, QByteArray *out)
{
    wchar_t scheme[32]  = {0};
    wchar_t host[256]   = {0};
    wchar_t path[2048]  = {0};
    URL_COMPONENTS uc   = {0};
    uc.dwStructSize     = sizeof(uc);
    uc.lpszScheme       = scheme; uc.dwSchemeLength   = sizeof(scheme) / sizeof(scheme[0]);
    uc.lpszHostName     = host;   uc.dwHostNameLength = sizeof(host)   / sizeof(host[0]);
    uc.lpszUrlPath      = path;   uc.dwUrlPathLength  = sizeof(path)   / sizeof(path[0]);
    if (!WinHttpCrackUrl(reinterpret_cast<LPCWSTR>(url.utf16()),
                         static_cast<DWORD>(url.length()), 0, &uc))
        return false;

    const DWORD port = uc.nPort ? uc.nPort
                                : (uc.nScheme == INTERNET_SCHEME_HTTPS ? 443 : 80);

    HINTERNET sess = WinHttpOpen(L"popball2/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess)
        return false;
    // 解析/连接/发送/接收全部超时都设小一点：LHM 没跑时连接被拒会立刻失败，
    // 跑着时 localhost 也很快；即使异常也不会长时间卡住主线程。
    WinHttpSetTimeouts(sess, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET conn = WinHttpConnect(sess, host, port, 0);
    HINTERNET req  = nullptr;
    bool      ok   = false;
    if (conn) {
        req = WinHttpOpenRequest(conn, L"GET", path[0] ? path : L"/",
                                 nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 (uc.nScheme == INTERNET_SCHEME_HTTPS)
                                     ? WINHTTP_FLAG_SECURE : 0);
    }
    if (req) {
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            && WinHttpReceiveResponse(req, nullptr)) {
            DWORD avail = 0;
            QByteArray body;
            while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                QByteArray buf(static_cast<int>(avail), 0);
                DWORD read = 0;
                if (!WinHttpReadData(req, buf.data(), avail, &read) || read == 0)
                    break;
                body.append(buf.constData(), static_cast<int>(read));
            }
            *out = body;
            ok = !body.isEmpty();
        }
    }
    if (req)  WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(sess);
    return ok;
}

// 从 LHM 的字符串读数里取出数值。LHM 的 Value 形如 "45.0 °C" / "12.5 %" / "3.60 GHz"，
// 部分区域设置会用逗号当小数点（"45,0 °C"），这里一并兼容。
bool parseLhmNumber(const QString &s, double *out)
{
    int i = 0;
    while (i < s.size()) {
        const QChar c = s.at(i);
        if (c.isDigit() || c == QLatin1Char('-') || c == QLatin1Char('+') || c == QLatin1Char('.'))
            break;
        ++i;
    }
    int j = i;
    while (j < s.size()) {
        const QChar c = s.at(j);
        if (c.isDigit() || c == QLatin1Char('.') || c == QLatin1Char(',')
            || c == QLatin1Char('-') || c == QLatin1Char('+'))
            ++j;
        else
            break;
    }
    if (i == j)
        return false;
    QString num = s.mid(i, j - i);
    if (!num.contains(QLatin1Char('.')))
        num.replace(QLatin1Char(','), QLatin1Char('.'));   // 逗号小数点兼容
    bool ok = false;
    const double v = num.toDouble(&ok);
    if (!ok)
        return false;
    *out = v;
    return true;
}

// 递归遍历 LHM 的 JSON 树，收集「CPU 相关」的温度传感器值。
// 注意 LHM 的实际格式：没有 SensorType 字段，Value 是带单位的字符串（如 "45.0 °C"），
// 类型要靠单位 °C 或 ImageURL 里的 temperature 判断；节点 Text 可能是 "/intelcpu/0"。
void lhmCollectCpuTemps(const QJsonObject &node, const QStringList &ancestors,
                        QList<double> *out)
{
    const QString text     = node.value(QLatin1String("Text")).toString();
    const QJsonValue valV  = node.value(QLatin1String("Value"));
    const QString valueStr = valV.isString() ? valV.toString()
                                             : QString::number(valV.toDouble());
    const QString imageUrl = node.value(QLatin1String("ImageURL")).toString();
    const QString sensorType = node.value(QLatin1String("SensorType")).toString();

    QStringList path = ancestors;
    path.append(text);

    // 判定是不是温度传感器：单位含 °C / 图标是温度计 / 老版本可能带 SensorType
    const bool isTemperature =
        valueStr.contains(QStringLiteral("°C"))
        || imageUrl.contains(QLatin1String("temperature"), Qt::CaseInsensitive)
        || sensorType.compare(QLatin1String("Temperature"), Qt::CaseInsensitive) == 0;

    if (isTemperature) {
        bool isCpu = false;
        for (const QString &p : path) {
            const QString l = p.toLower();
            if (l.contains(QLatin1String("cpu")) || l.contains(QLatin1String("core"))
                || l.contains(QLatin1String("package"))
                || l.contains(QStringLiteral("中央处理器"))) {
                isCpu = true;
                break;
            }
        }
        double v = 0.0;
        if (isCpu && parseLhmNumber(valueStr, &v) && v > 0.0 && v < 150.0)
            out->append(v);
    }

    const QJsonArray children = node.value(QLatin1String("Children")).toArray();
    for (const QJsonValue &c : children)
        if (c.isObject())
            lhmCollectCpuTemps(c.toObject(), path, out);
}

// 从 LibreHardwareMonitor 的 Web 服务读 CPU 温度（取所有 CPU 相关传感器的最大值）。
bool readLibreHardwareMonitorCpuTemp(double *outC)
{
    QByteArray body;
    if (!winHttpGet(QLatin1String(kLhmUrl), 800, &body))
        return false;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (doc.isNull() || !doc.isObject())
        return false;

    QList<double> temps;
    lhmCollectCpuTemps(doc.object(), QStringList(), &temps);
    if (temps.isEmpty())
        return false;

    double best = 0.0;
    for (double t : temps)
        if (t > best)
            best = t;
    *outC = best;
    return true;
}

// =======================================================================================
//  内置驱动读取：WinRing0 + 直接读 CPU 的 MSR（与软媒魔方 / Open Hardware Monitor 同路数）
//
//  Windows 没有用户态的「读核心温度」API，必须借助内核驱动访问 CPU 的 MSR 寄存器。
//  WinRing0 是最通用的方案：把 WinRing0x64.sys(64 位) 或 WinRing0.sys(32 位) 放在
//  popball2.exe 同目录，程序会自动把驱动注册成内核服务并加载，然后读 MSR 取温度。
//  驱动文件可从头里自带的监控软件目录里复制，例如 软媒魔方 / Open Hardware Monitor。
//
//  * Intel：MSR 0x1A2(IA32_TEMPERATURE_TARGET) 取 TjMax，
//           MSR 0x19C(IA32_THERM_STATUS) 取 DTS，温度 = TjMax - DTS。
//  * AMD  ：经 PCI 寄存器 0xB8/0xBC 访问 SMN 寄存器 0x00059800(THM_TCON_CUR_TMP) 取 Tctl。
//
//  需要管理员权限（加载驱动）；未附带驱动或加载失败时自动回退到 LHM/OHM/ACPI。
// =======================================================================================

#ifndef OLS_TYPE
#define OLS_TYPE 40000
#endif
#define IOCTL_OLS_READ_MSR          CTL_CODE(OLS_TYPE, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_OLS_WRITE_MSR         CTL_CODE(OLS_TYPE, 0x822, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_OLS_READ_PCI_CONFIG   CTL_CODE(OLS_TYPE, 0x851, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_OLS_WRITE_PCI_CONFIG  CTL_CODE(OLS_TYPE, 0x852, METHOD_BUFFERED, FILE_WRITE_ACCESS)

// 读取 CPU 厂商标识（注册表里就有，无需 cpuid）：GenuineIntel / AuthenticAMD
QString cpuVendorIdentifier()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return QString();
    wchar_t buf[128] = {0};
    DWORD sz = sizeof(buf), type = 0;
    RegQueryValueExW(key, L"VendorIdentifier", nullptr, &type,
                     reinterpret_cast<LPBYTE>(buf), &sz);
    RegCloseKey(key);
    return QString::fromWCharArray(buf);
}

// 当前是否 64 位 Windows（用于挑 x64 / x86 驱动文件）
bool is64BitWindows()
{
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    return si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64;
}

// 当前进程是否以管理员(提权)身份运行
bool isCurrentProcessElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    DWORD elevation = 0, ret = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &ret)
                    && elevation != 0;
    CloseHandle(token);
    return ok;
}

// 把指定 .sys 注册成内核服务并启动（需要管理员权限）。
// 每一步失败都带 Win32 错误码打日志，便于在 Win11 上定位：
//   5    = 没有管理员权限；1275 = 被微软易受攻击驱动黑名单拦截；
//   1073 = 服务已存在；2/3   = 驱动文件路径不对。
// 最近一次 SCM 操作的 Win32 错误码（供管理员子进程落盘排查用）
static DWORD g_winRing0SvcErr = 0;

bool winRing0InstallService(const QString &sysPath)
{
    const QString nativePath = QDir::toNativeSeparators(sysPath);
    g_winRing0SvcErr = 0;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        g_winRing0SvcErr = GetLastError();
        qDebug() << "[WinRing0] OpenSCManager failed, err =" << g_winRing0SvcErr
                 << (g_winRing0SvcErr == ERROR_ACCESS_DENIED ? QStringLiteral("(非管理员)")
                                                           : QString());
        return false;
    }

    const wchar_t *kService = L"WinRing0_1_2_0";
    SC_HANDLE s = CreateServiceW(
        scm, kService, kService, SERVICE_START | SERVICE_QUERY_STATUS,
        SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
        reinterpret_cast<const wchar_t *>(nativePath.utf16()),
        nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!s) {
        const DWORD err = GetLastError();
        g_winRing0SvcErr = err;
        if (err == ERROR_SERVICE_EXISTS || err == ERROR_SERVICE_MARKED_FOR_DELETE) {
            s = OpenServiceW(scm, kService, SERVICE_START | SERVICE_QUERY_STATUS);
            qDebug() << "[WinRing0] 服务已存在，直接打开，err =" << err;
        } else {
            qDebug() << "[WinRing0] CreateService failed, err =" << err;
        }
    }

    bool ok = false;
    if (s) {
        if (StartServiceW(s, 0, nullptr)) {
            ok = true;
        } else {
            const DWORD err = GetLastError();
            if (err == ERROR_SERVICE_ALREADY_RUNNING) {
                ok = true;                 // 已在运行（软媒/OHM/LHM 装过）
            } else {
                g_winRing0SvcErr = err;
                qDebug() << "[WinRing0] StartService failed, err =" << err
                         << (err == 1275 ? QStringLiteral("(被 Windows 驱动黑名单拦截)")
                                         : QString());
            }
        }
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
    return ok;
}

// 非管理员时：通过 UAC 自提权，用自身带 --install-winring0-driver 参数装驱动。
// 用户取消 UAC / 提权失败都返回 false，主流程继续回退到 LHM / ACPI。
bool triggerElevatedDriverInstall(const QString &sysPath)
{
    if (isCurrentProcessElevated())
        return false;                      // 已是管理员却失败，说明不是权限问题

    const QString exe = QCoreApplication::applicationFilePath();
    const QString params =
        QStringLiteral("--install-winring0-driver \"%1\"").arg(
            QDir::toNativeSeparators(sysPath));

    SHELLEXECUTEINFOW sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_UNICODE;
    sei.lpVerb       = L"runas";           // 触发 UAC 提权
    sei.lpFile       = reinterpret_cast<const wchar_t *>(exe.utf16());
    sei.lpParameters = reinterpret_cast<const wchar_t *>(params.utf16());
    sei.nShow        = SW_HIDE;

    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        qDebug() << "[WinRing0] UAC 自提权失败（用户取消或被策略禁止），err ="
                 << GetLastError();
        return false;
    }
    WaitForSingleObject(sei.hProcess, 15000);
    DWORD code = 0;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    qDebug() << "[WinRing0] 提权安装驱动退出码 =" << code;
    return code == 0;
}

// WinRing0 用户态封装：打开设备、按需安装/启动内核服务、读写 MSR / PCI 配置空间
struct WinRing0 {
    HANDLE dev = INVALID_HANDLE_VALUE;

    ~WinRing0() { close(); }

    bool open()
    {
        const wchar_t *kDevice = L"\\\\.\\WinRing0_1_2_0";
        dev = CreateFileW(kDevice, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dev != INVALID_HANDLE_VALUE)
            return true;                       // 驱动已被别的程序（OHM/软媒）加载
        if (!installAndStart())
            return false;
        dev = CreateFileW(kDevice, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return dev != INVALID_HANDLE_VALUE;
    }

    void close()
    {
        if (dev != INVALID_HANDLE_VALUE) { CloseHandle(dev); dev = INVALID_HANDLE_VALUE; }
    }

    bool readMsr(DWORD reg, DWORD *eax, DWORD *edx)
    {
        if (dev == INVALID_HANDLE_VALUE) return false;
        struct { DWORD eax; DWORD edx; } out = {0, 0};
        DWORD ret = 0;
        if (!DeviceIoControl(dev, IOCTL_OLS_READ_MSR, &reg, sizeof(reg),
                             &out, sizeof(out), &ret, nullptr))
            return false;
        *eax = out.eax; *edx = out.edx;
        return true;
    }

    bool readPci(DWORD busDevFunc, DWORD reg, DWORD *value)
    {
        if (dev == INVALID_HANDLE_VALUE) return false;
        struct { DWORD addr; DWORD reg; } in = { busDevFunc, reg };
        DWORD out = 0, ret = 0;
        if (!DeviceIoControl(dev, IOCTL_OLS_READ_PCI_CONFIG, &in, sizeof(in),
                             &out, sizeof(out), &ret, nullptr))
            return false;
        *value = out;
        return true;
    }

    bool writePci(DWORD busDevFunc, DWORD reg, DWORD value)
    {
        if (dev == INVALID_HANDLE_VALUE) return false;
        struct { DWORD addr; DWORD reg; DWORD value; } in = { busDevFunc, reg, value };
        DWORD ret = 0;
        return DeviceIoControl(dev, IOCTL_OLS_WRITE_PCI_CONFIG, &in, sizeof(in),
                               nullptr, 0, &ret, nullptr) != FALSE;
    }

private:
    // 把同目录下的 WinRing0x64.sys / WinRing0.sys 注册成内核服务并启动。
    // 非管理员时自动走一次 UAC 自提权（装成功后服务常驻，之后无需再提权）。
    bool installAndStart()
    {
        const QString dir = QCoreApplication::applicationDirPath();
        QStringList candidates;
        if (is64BitWindows())
            candidates << QStringLiteral("WinRing0x64.sys") << QStringLiteral("WinRing0.sys");
        else
            candidates << QStringLiteral("WinRing0.sys") << QStringLiteral("WinRing0x64.sys");

        for (const QString &name : candidates) {
            const QString p = dir + QLatin1Char('/') + name;
            if (QFile::exists(p)) {
                if (winRing0InstallService(p))
                    return true;
                // 典型失败原因：非管理员装不了服务 → UAC 提权再试一次
                if (triggerElevatedDriverInstall(p) && winRing0InstallService(p))
                    return true;
                return false;              // 该驱动文件在，但加载被拦（黑名单等）
            }
        }
        qDebug() << "[WinRing0] 程序目录未找到 WinRing0x64.sys / WinRing0.sys，"
                    "无法读取 CPU 核心温度，回退其它温度来源";
        return false;   // 没附带驱动：交给上层回退
    }
};

// Intel：MSR 0x1A2 取 TjMax，0x19C 取 DTS，温度 = TjMax - DTS
bool winRing0IntelTemp(WinRing0 &wr, double *outC)
{
    DWORD eax = 0, edx = 0;
    int tjMax = 100;
    if (wr.readMsr(0x1A2, &eax, &edx)) {
        const int t = static_cast<int>((eax >> 16) & 0xFF);
        if (t > 0 && t < 150) tjMax = t;
    }
    if (!wr.readMsr(0x19C, &eax, &edx))
        return false;
    if (!(eax & 0x80000000u))              // bit31：读数有效位
        return false;
    const int dts = static_cast<int>((eax >> 16) & 0x7F);
    const double t = static_cast<double>(tjMax - dts);
    if (t > 0.0 && t < 150.0) { *outC = t; return true; }
    return false;
}

// AMD(Zen)：SMN 寄存器 0x00059800 -> Tctl（0.125°C 分辨率）
bool winRing0AmdTemp(WinRing0 &wr, double *outC)
{
    const DWORD bdf = 0;                     // bus0 / dev0 / func0
    if (!wr.writePci(bdf, 0xB8, 0x00059800))
        return false;
    DWORD v = 0;
    if (!wr.readPci(bdf, 0xBC, &v))
        return false;
    const int raw = static_cast<int>((v >> 21) & 0x7FF);
    const double t = raw / 8.0;
    if (t > 0.0 && t < 150.0) { *outC = t; return true; }
    return false;
}

bool probeWinRing0CpuTempCelsius(double *outC)
{
    static WinRing0 s_wr;
    static int  s_state = 0;                 // 0=未尝试 1=可用 -1=不可用
    static qint64 s_lastFailMs = 0;          // 上次失败时间：失败后每 30s 重试一次
    static QString s_vendor;
    if (s_state == 0
        || (s_state == -1
            && QDateTime::currentMSecsSinceEpoch() - s_lastFailMs > 30000)) {
        // 先试已打开的设备；失败再重新走一遍 open（装驱动是异步的：
        // UAC 提权 / LHM 稍后拉起驱动都可能让设备在几秒后才可用）
        s_wr.close();
        s_state = s_wr.open() ? 1 : -1;
        if (s_state == -1)
            s_lastFailMs = QDateTime::currentMSecsSinceEpoch();
        s_vendor = cpuVendorIdentifier().toLower();
    }
    if (s_state != 1)
        return false;

    if (s_vendor.contains(QLatin1String("intel")))
        return winRing0IntelTemp(s_wr, outC);
    if (s_vendor.contains(QLatin1String("amd")))
        return winRing0AmdTemp(s_wr, outC);
    // 未知厂商：Intel 读法优先，再试 AMD
    return winRing0IntelTemp(s_wr, outC) || winRing0AmdTemp(s_wr, outC);
}

// 组合 Windows 温度来源：按「内置驱动(WinRing0) → LibreHardwareMonitor(Web) →
// OpenHardwareMonitor(WMI) → ACPI 热区」依次尝试，全都读不到才标记不可用。
bool probeOpenHardwareMonitorCpuTempCelsius(double *outC);   // 前向声明（定义见下方）
bool readWinCpuTempCelsius(double *outC)
{
    double v = 0.0;
    if (probeWinRing0CpuTempCelsius(&v))             { *outC = v; return true; }
    if (readLibreHardwareMonitorCpuTemp(&v))         { *outC = v; return true; }
    if (probeOpenHardwareMonitorCpuTempCelsius(&v))  { *outC = v; return true; }
    if (probeWinCpuTempCelsius(&v))                  { *outC = v; return true; }
    return false;
}

// ---------------------------------------------------------------------------------------
//  OpenHardwareMonitor 的 WMI 提供器（root\OpenHardwareMonitor → Sensor 类）。
//  OHM 运行时会在 WMI 注册传感器；覆盖「装了 OHM 但没装 LHM」的场景。
//  传感器示例：Name="CPU Package" / "CPU Core #1"，SensorType="Temperature"，Value=摄氏度。
// ---------------------------------------------------------------------------------------
bool probeOpenHardwareMonitorCpuTempCelsius(double *outC)
{
    *outC = -1.0;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE)
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = (hr == S_OK || hr == S_FALSE);
    if (FAILED(hr))
        return false;

    // 本地查询无需严格安全上下文；失败也不影响后续回退。
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                         RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                         nullptr, EOAC_NONE, nullptr);

    IWbemLocator *loc = nullptr;
    const HRESULT hrCo = CoCreateInstance(CLSID_WbemLocator, nullptr,
                                         CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                         reinterpret_cast<void **>(&loc));
    bool    ok   = false;
    double  best = -1.0;
    if (SUCCEEDED(hrCo) && loc) {
        BSTR ns = SysAllocString(L"root\\OpenHardwareMonitor");
        IWbemServices *svc = nullptr;
        if (ns && loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc)
                    == WBEM_S_NO_ERROR) {
            BSTR lang  = SysAllocString(L"WQL");
            BSTR query = SysAllocString(
                L"SELECT Name, Value FROM Sensor WHERE SensorType='Temperature'");
            IEnumWbemClassObject *e = nullptr;
            if (lang && query
                && svc->ExecQuery(lang, query, WBEM_FLAG_FORWARD_ONLY, nullptr, &e)
                       == WBEM_S_NO_ERROR) {
                IWbemClassObject *obj = nullptr;
                ULONG got = 0;
                while (e->Next(WBEM_INFINITE, 1, &obj, &got) == WBEM_S_NO_ERROR && got) {
                    VARIANT vn, vv;
                    VariantInit(&vn); VariantInit(&vv);
                    QString name;
                    double  val = -1.0;
                    if (obj->Get(L"Name", 0, &vn, nullptr, nullptr) == WBEM_S_NO_ERROR
                        && vn.vt == VT_BSTR)
                        name = QString::fromWCharArray(vn.bstrVal);
                    if (obj->Get(L"Value", 0, &vv, nullptr, nullptr) == WBEM_S_NO_ERROR) {
                        if (vv.vt == VT_R8)      val = vv.dblVal;
                        else if (vv.vt == VT_R4) val = static_cast<double>(vv.fltVal);
                    }
                    const QString l = name.toLower();
                    if ((l.contains(QLatin1String("cpu")) || l.contains(QLatin1String("core"))
                         || l.contains(QLatin1String("package")))
                        && val > 0.0 && val < 150.0 && val > best)
                        best = val;
                    VariantClear(&vn); VariantClear(&vv);
                    obj->Release(); obj = nullptr; got = 0;
                }
                e->Release();
            }
            if (lang)  SysFreeString(lang);
            if (query) SysFreeString(query);
            svc->Release();
        }
        if (ns) SysFreeString(ns);
        loc->Release();
    }
    if (needUninit) CoUninitialize();
    if (best > 0.0) { *outC = best; ok = true; }
    return ok;
}

// ---------------------------------------------------------------------------------------
//  随附进程：若 popball2 同目录下带了 LibreHardwareMonitor.exe，就把它以隐藏窗口方式
//  拉起（LHM 已在跑则跳过）。这样用户无需每次手动启动，放一份在旁边即可。
//  注意：LHM 的 ring-0 驱动需要管理员权限才能加载（否则读不到 CPU 温度），
//  因此请以管理员身份运行 popball2；LHM 会继承父进程的权限。
//  读不到温度时不影响主程序，会自动回退到其它来源 / ACPI，绝不显示假数据。
// ---------------------------------------------------------------------------------------
void ensureLibreHardwareMonitor()
{
    // 端口已在服务（LHM 已经跑着）→ 不必再拉起
    QByteArray probe;
    if (winHttpGet(QLatin1String(kLhmUrl), 300, &probe))
        return;

    const QString exe = QCoreApplication::applicationDirPath()
                        + QLatin1Char('/') + QLatin1String("LibreHardwareMonitor.exe");
    if (!QFile::exists(exe))
        return;   // 没附带 LHM：保持原有 ACPI 行为

    STARTUPINFOA si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags      = STARTF_USESHOWWINDOW;
    si.wShowWindow  = SW_HIDE;          // 尽量不让 LHM 主窗口闪出来
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));

    // CreateProcessA 会改写命令行缓冲区，必须用可写副本
    const QString cmdLine = QLatin1Char('"') + exe + QLatin1String("\" /web");
    QByteArray buf = cmdLine.toLocal8Bit();
    if (CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}
#endif // Q_OS_WIN

} // namespace

#if defined(Q_OS_WIN)
// 供 main.cpp 的 "--install-winring0-driver <sys>" 管理员子进程调用：
// 只装驱动服务，装完立刻退出，不进 GUI。
// 子进程没有控制台，qDebug 不可见，因此把结果写到 %TEMP% 下的日志文件。
bool installWinRing0DriverService(const QString &sysPath)
{
    const bool ok = winRing0InstallService(sysPath);
    QFile log(QStandardPaths::writableLocation(QStandardPaths::TempLocation)
              + QStringLiteral("/popball2_winring0_install.log"));
    if (log.open(QIODevice::WriteOnly | QIODevice::Text)) {
        log.write(QString(QStringLiteral("ok=%1 serviceErr=%2 driver=%3\n"))
                      .arg(ok).arg(g_winRing0SvcErr)
                      .arg(QDir::toNativeSeparators(sysPath)).toUtf8());
    }
    return ok;
}
#endif

/* =====================================================================================
 *                                  构造 / 析构
 * ===================================================================================== */
SysInfo::SysInfo()
{
    this->checkTemperatorFilePath();
    this->updateSysinfo();
}

SysInfo::~SysInfo()
{
#if defined(Q_OS_WIN)
    if (this->pdhQuery != nullptr) {
        PdhCloseQuery(this->pdhQuery);
        this->pdhQuery = nullptr;
    }
#endif
#if defined(Q_OS_MACOS)
    if (this->_smcOpen && this->_smcConn) {
        IOServiceClose(static_cast<io_connect_t>(this->_smcConn));
        this->_smcConn = 0;
        this->_smcOpen = false;
    }
#endif
}

/* =====================================================================================
 *                                  Linux 平台实现
 * ===================================================================================== */
#if defined(Q_OS_LINUX)

void SysInfo::checkTemperatorFilePath()
{
    this->_temp_paths      = probeCpuTempPaths();
    this->cpuTemperatureOk = !this->_temp_paths.isEmpty();

    // 便于在不同发行版 / 芯片上排查：启动时打印一次实际选中的温度来源
    if (this->cpuTemperatureOk)
        qDebug() << "[SysInfo] arch=" << QSysInfo::currentCpuArchitecture()
                 << "CPU 温度源:" << this->_temp_paths.size() << "个传感器，例如"
                 << this->_temp_paths.first();
    else
        qDebug() << "[SysInfo] arch=" << QSysInfo::currentCpuArchitecture()
                 << "未找到可用的 CPU 温度传感器，将不显示温度";
}

void SysInfo::updateSysinfo()
{
    // ---------------------------------------------------------------- 内存 / 交换分区
    // /proc/meminfo 各发行版通用；老内核可能没有 MemAvailable，此时自行估算
    const QString memText = readTextFile(QStringLiteral("/proc/meminfo"));
    QHash<QString, qulonglong> kv;
    const QStringList memLines = memText.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : memLines) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        const QString key = line.left(colon).trimmed();
        const QString val = line.mid(colon + 1).trimmed().section(QLatin1Char(' '), 0, 0);
        bool ok = false;
        const qulonglong v = val.toULongLong(&ok);
        if (ok)
            kv.insert(key, v);
    }

    const qulonglong mem_total   = kv.value(QStringLiteral("MemTotal"), 0);
    const qulonglong mem_free    = kv.value(QStringLiteral("MemFree"), 0);
    const qulonglong buffers     = kv.value(QStringLiteral("Buffers"), 0);
    const qulonglong cached      = kv.value(QStringLiteral("Cached"), 0);
    qulonglong       mem_avail   = kv.value(QStringLiteral("MemAvailable"), 0);
    const qulonglong swap_total  = kv.value(QStringLiteral("SwapTotal"), 0);
    const qulonglong swap_free   = kv.value(QStringLiteral("SwapFree"), 0);

    if (mem_avail == 0)
        mem_avail = mem_free + buffers + cached;   // 老内核兜底

    memoryInfo.mem_total     = mem_total;
    memoryInfo.mem_free      = mem_free;
    memoryInfo.mem_available = mem_avail;
    memoryInfo.cached        = cached;
    memoryInfo.buffers       = buffers;
    memoryInfo.swap_total    = swap_total;
    memoryInfo.swap_free     = swap_free;

    // 与 free 命令口径一致：used = total - free - buffers - cached
    qulonglong used = 0;
    if (mem_total > mem_free + buffers + cached)
        used = mem_total - mem_free - buffers - cached;
    memoryInfo.mem_used  = used;
    memoryInfo.swap_used = (swap_total > swap_free) ? (swap_total - swap_free) : 0;

    this->memOk  = (mem_total  > 0);
    this->swapOk = (swap_total > 0);

    // ---------------------------------------------------------------------- CPU 温度
    // 启动时若没探测到（模块后加载），每隔一段时间重试一次
    if (this->_temp_paths.isEmpty()) {
        static int probeRetry = 0;
        if (++probeRetry >= 50) {
            probeRetry = 0;
            this->checkTemperatorFilePath();
        }
    }

    double tempMax = 0.0;
    for (const QString &p : this->_temp_paths) {
        const double v = readTextFile(p).toDouble() / 1000.0;   // 毫摄氏度 -> 摄氏度
        if (v > tempMax && v < 150.0)
            tempMax = v;
    }
    if (!this->_temp_paths.isEmpty()) {
        this->cpuTemperature   = tempMax;
        this->cpuTemperatureOk = (tempMax > 0.0);
    } else {
        this->cpuTemperature   = 0.0;
        this->cpuTemperatureOk = false;
    }

    // ---------------------------------------------------------------------- CPU 频率
    double mhz = 0.0;
    if (probeCpuFrequencyMHz(&mhz)) {
        this->cpuFreq   = mhz;
        this->cpuFreqOk = true;
    } else {
        this->cpuFreq   = 0.0;
        this->cpuFreqOk = false;
    }

    // ---------------------------------------------------------------------- CPU 占用
    const QString statLine =
        readTextFile(QStringLiteral("/proc/stat")).section(QLatin1Char('\n'), 0, 0);
    const QStringList f =
        statLine.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (f.size() >= 8 && f.value(0) == QLatin1String("cpu")) {
        auto at = [&f](int i) -> double {
            return (i < f.size()) ? f.at(i).toDouble() : 0.0;
        };
        const double user    = at(1);
        const double nice    = at(2);
        const double sys     = at(3);
        const double idle    = at(4);
        const double iowait  = at(5);
        const double irq     = at(6);
        const double softirq = at(7);
        const double steal   = at(8);

        const double idleAll = idle + iowait;
        const double total   = user + nice + sys + idleAll + irq + softirq + steal;

        if (this->cpuUsageHasPrev && total > this->cpuUsageTotalLast) {
            const double dTotal = total - this->cpuUsageTotalLast;
            const double dIdle  = idleAll - this->cpuUsageIdleLast;
            double usage = (dTotal - dIdle) / dTotal * 100.0;
            if (usage < 0.0)   usage = 0.0;
            if (usage > 100.0) usage = 100.0;
            this->cpuUsage = usage;
        }
        this->cpuUsageTotalLast = total;
        this->cpuUsageIdleLast  = idleAll;
        this->cpuUsageHasPrev   = true;
    }

    // ---------------------------------------------------------------------- 网速
    const QString netText = readTextFile(QStringLiteral("/proc/net/dev"));
    const QStringList netLines = netText.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    quint64 rx = 0;
    quint64 tx = 0;
    for (const QString &line : netLines) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon < 0)
            continue;
        const QString name = line.left(colon).trimmed();
        if (name.isEmpty() || isVirtualNetInterface(name))
            continue;
        const QStringList ite = line.mid(colon + 1)
                                    .split(QRegularExpression(QStringLiteral("\\s+")),
                                           Qt::SkipEmptyParts);
        if (ite.size() < 16)
            continue;
        rx += ite.at(0).toULongLong();   // 接收字节
        tx += ite.at(8).toULongLong();   // 发送字节
    }

    this->receive  = (rx >= this->receive_last)  ? (rx - this->receive_last)  : 0;
    this->transmit = (tx >= this->transmit_last) ? (tx - this->transmit_last) : 0;
    this->receive_last  = rx;
    this->transmit_last = tx;

    // ---------------------------------------------------------------------- 磁盘 IO
    // /proc/diskstats：各字段含义（内核文档 Documentation/admin-guide/iostats.rst）
    //   字段 3(读完成次数)  5(读扇区数)  6(写完成次数)  9(写扇区数)
    // 扇区大小通常 512 字节，部分新设备可能 4K；这里统一按 512 算（与 iostat 一致）
    // 逐盘统计累计读/写字节，再交给 finalizeDiskIo() 选出生效盘。
    const QString diskText = readTextFile(QStringLiteral("/proc/diskstats"));
    QHash<QString, QPair<quint64, quint64>> diskDelta;   // 设备名 -> (本间隔读字节, 写字节)
    for (const QString &line : diskText.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (f.size() < 13) continue;
        // 跳过分区（主设备名的分区号）、ramdisk、loop、cdrom 等
        const QString name = f.value(2);
        if (name.contains(QLatin1Char('p')) && name.size() > 2) {
            bool allDigit = true;
            for (int i = 1; i < name.size(); ++i)
                if (!name.at(i).isDigit()) { allDigit = false; break; }
            if (allDigit) continue;   // sda1, nvme0n1p2 这类分区跳过
        }
        if (name.startsWith(QLatin1String("loop")) ||
            name.startsWith(QLatin1String("ram"))  ||
            name.startsWith(QLatin1String("zram")) ||
            name.startsWith(QLatin1String("sr"))) continue;
        // 友好显示名：/sys/block/<dev>/device/model（每块盘只读一次，之后复用）
        if (!this->diskLabelMap.contains(name)) {
            const QString model = readTextFile(QStringLiteral("/sys/block/%1/device/model").arg(name));
            if (!model.isEmpty())
                this->diskLabelMap.insert(name, name + QStringLiteral(" · ") + model);
        }
        const quint64 rdTotal = f.at(5).toULongLong() * 512;   // 累计读字节
        const quint64 wrTotal = f.at(9).toULongLong() * 512;   // 累计写字节
        DiskIoStat &s = this->diskStats[name];
        const quint64 rdDelta = (rdTotal >= s.read_total)  ? (rdTotal  - s.read_total)  : 0;
        const quint64 wrDelta = (wrTotal >= s.write_total) ? (wrTotal - s.write_total) : 0;
        s.read_total  = rdTotal;
        s.write_total = wrTotal;
        diskDelta.insert(name, qMakePair(rdDelta, wrDelta));
    }
    this->finalizeDiskIo(diskDelta);

    this->lastUpdateTime = QDateTime::currentMSecsSinceEpoch();
}

#endif // Q_OS_LINUX

/* =====================================================================================
 *                                  macOS 平台实现
 *
 *    - 内存：sysctl(hw.memsize) + Mach host_statistics64(HOST_VM_INFO64)
 *    - 交换分区：sysctl(vm.swapusage)
 *    - CPU 占用：Mach host_statistics(HOST_CPU_LOAD_INFO) 的 tick 差值
 *    - CPU 温度：AppleSMC（Intel 与 Apple Silicon 键名/编码不同，已分别处理）
 *    - CPU 频率：sysctl(hw.cpufrequency)，Intel Mac 有，Apple Silicon 无
 *    - 网速：getifaddrs() 读取 AF_LINK 接口的 ifi_ibytes / ifi_obytes
 *  内存/交换分区统一使用 KB，与 Linux 侧保持一致。
 * ===================================================================================== */
#if defined(Q_OS_MACOS)

void SysInfo::checkTemperatorFilePath()
{
    this->cpuTemperatureOk = false;

    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching("AppleSMC"));
    if (!svc) {
        qDebug() << "[SysInfo] arch=" << QSysInfo::currentCpuArchitecture()
                 << "未找到 AppleSMC（常见于虚拟机），将不显示温度";
        return;
    }

    io_connect_t conn = 0;
    const kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != kIOReturnSuccess) {
        qDebug() << "[SysInfo] AppleSMC 打开失败，将不显示温度";
        return;
    }

    this->_smcConn     = static_cast<unsigned int>(conn);
    this->_smcOpen     = true;
    this->_smcCpuKeys  = smcDiscoverCpuTempKeys(conn);
    this->cpuTemperatureOk = !this->_smcCpuKeys.isEmpty();

    if (this->cpuTemperatureOk)
        qDebug() << "[SysInfo] arch=" << QSysInfo::currentCpuArchitecture()
                 << "AppleSMC CPU 温度键:" << this->_smcCpuKeys.size() << "个";
    else
        qDebug() << "[SysInfo] arch=" << QSysInfo::currentCpuArchitecture()
                 << "未枚举到 CPU 温度键，将不显示温度";
}

void SysInfo::updateSysinfo()
{
    // ---------------------------------------------------------------- 内存 / 交换分区
    uint64_t page_size = 0;
    size_t   size      = sizeof(page_size);
    if (sysctlbyname("hw.pagesize", &page_size, &size, nullptr, 0) != 0 || page_size == 0)
        page_size = 4096;

    uint64_t mem_total_bytes = 0;
    size = sizeof(mem_total_bytes);
    sysctlbyname("hw.memsize", &mem_total_bytes, &size, nullptr, 0);

    memoryInfo.mem_total = mem_total_bytes / 1024;

    vm_statistics64_data_t vmstat;
    mach_msg_type_number_t vm_count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&vmstat), &vm_count) == KERN_SUCCESS)
    {
        const uint64_t page_free       = uint64_t(vmstat.free_count)            * page_size;
        const uint64_t page_inactive   = uint64_t(vmstat.inactive_count)        * page_size;
        const uint64_t page_active     = uint64_t(vmstat.active_count)          * page_size;
        const uint64_t page_wired      = uint64_t(vmstat.wire_count)            * page_size;
        const uint64_t page_compressed = uint64_t(vmstat.compressor_page_count) * page_size;

        // 与「活动监视器」口径接近：App 内存 + 联动内存 + 已压缩内存
        const uint64_t used_bytes = page_active + page_wired + page_compressed;

        memoryInfo.mem_free      = page_free / 1024;
        memoryInfo.mem_available = (page_free + page_inactive) / 1024;
        memoryInfo.cached        = page_inactive / 1024;
        memoryInfo.mem_used      = used_bytes / 1024;
    }

    struct xsw_usage swap_usage;
    size = sizeof(swap_usage);
    if (sysctlbyname("vm.swapusage", &swap_usage, &size, nullptr, 0) == 0) {
        memoryInfo.swap_total = swap_usage.xsu_total / 1024;
        memoryInfo.swap_free  = swap_usage.xsu_avail / 1024;
        memoryInfo.swap_used  = swap_usage.xsu_used  / 1024;
    }

    this->memOk  = (memoryInfo.mem_total  > 0);
    this->swapOk = (memoryInfo.swap_total > 0);

    // ---------------------------------------------------------------------- CPU 温度
    double temp = 0.0;
    if (this->_smcOpen && !this->_smcCpuKeys.isEmpty()
        && smcReadCpuTemperature(static_cast<io_connect_t>(this->_smcConn), this->_smcCpuKeys, &temp)) {
        this->cpuTemperature   = temp;
        this->cpuTemperatureOk = true;
    } else {
        this->cpuTemperature   = 0.0;
        this->cpuTemperatureOk = false;
    }

    // ---------------------------------------------------------------------- CPU 频率
    {   // Intel Mac 可用；Apple Silicon 无公开接口
        uint64_t hz = 0;
        size = sizeof(hz);
        this->cpuFreqOk = false;
        if (sysctlbyname("hw.cpufrequency", &hz, &size, nullptr, 0) == 0 && hz > 0) {
            this->cpuFreq   = double(hz) / 1000000.0;   // Hz -> MHz
            this->cpuFreqOk = true;
        } else {
            size = sizeof(hz);
            if (sysctlbyname("hw.cpufrequency_max", &hz, &size, nullptr, 0) == 0 && hz > 0) {
                this->cpuFreq   = double(hz) / 1000000.0;
                this->cpuFreqOk = true;
            } else {
                this->cpuFreq = 0.0;
            }
        }
    }

    // ---------------------------------------------------------------------- CPU 占用
    host_cpu_load_info_data_t cpu_load;
    mach_msg_type_number_t cpu_count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        reinterpret_cast<host_info_t>(&cpu_load), &cpu_count) == KERN_SUCCESS)
    {
        const double user = double(cpu_load.cpu_ticks[CPU_STATE_USER]);
        const double sys  = double(cpu_load.cpu_ticks[CPU_STATE_SYSTEM]);
        const double nice = double(cpu_load.cpu_ticks[CPU_STATE_NICE]);
        const double idle = double(cpu_load.cpu_ticks[CPU_STATE_IDLE]);

        const double busy  = user + sys + nice;
        const double total = busy + idle;

        if (this->cpuUsageHasPrev && total > this->cpuUsageTotalLast) {
            const double dTotal = total - this->cpuUsageTotalLast;
            const double dBusy  = busy  - (this->cpuUsageTotalLast - this->cpuUsageIdleLast);
            double usage = dBusy / dTotal * 100.0;
            if (usage < 0.0)   usage = 0.0;
            if (usage > 100.0) usage = 100.0;
            this->cpuUsage = usage;
        }
        this->cpuUsageTotalLast = total;
        this->cpuUsageIdleLast  = idle;
        this->cpuUsageHasPrev   = true;
    }

    // ---------------------------------------------------------------------- 网速
    struct ifaddrs *if_addr = nullptr;
    if (getifaddrs(&if_addr) == 0) {
        uint64_t rx = 0;
        uint64_t tx = 0;
        for (struct ifaddrs *ifa = if_addr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_LINK)
                continue;
            if ((ifa->ifa_flags & IFF_LOOPBACK) != 0 || (ifa->ifa_flags & IFF_UP) == 0)
                continue;
            struct if_data *data = reinterpret_cast<struct if_data *>(ifa->ifa_data);
            if (data != nullptr) {
                rx += data->ifi_ibytes;
                tx += data->ifi_obytes;
            }
        }
        freeifaddrs(if_addr);

        this->receive  = (rx >= this->receive_last)  ? (rx - this->receive_last)  : 0;
        this->transmit = (tx >= this->transmit_last) ? (tx - this->transmit_last) : 0;
        this->receive_last  = rx;
        this->transmit_last = tx;
    }

    // ---------------------------------------------------------------------- 磁盘 IO（macOS：IOKit 枚举所有 IOBlockStorageDriver，按 BSD 名逐盘统计）
    //  注意两个容易踩的点（已核对 ioreg 实测结构）：
    //   1) 累计字节在 IOBlockStorageDriver 的 "Statistics" 字典里，键是 "Bytes (Read)" / "Bytes (Write)"，
    //      而不是 "Statistics.ReadBytes" 这种点号拼法。
    //   2) "BSD Name"（disk0/disk6…）不在 driver 自身，而在它的子节点 IOMedia（整盘）上。
    {
        QHash<QString, QPair<quint64, quint64>> diskDelta;   // 设备名 -> (本间隔读字节, 写字节)
        io_iterator_t iter = IO_OBJECT_NULL;
        const kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching("IOBlockStorageDriver"), &iter);
        if (kr == KERN_SUCCESS && iter != IO_OBJECT_NULL) {
            io_object_t obj = 0;
            while ((obj = IOIteratorNext(iter)) != 0) {
                // 1) BSD 名 + 介质名：在 driver 的子节点 IOMedia（Whole=Yes，即整盘）上取
                QString bsdName, mediaName;
                io_iterator_t kids = IO_OBJECT_NULL;
                if (IORegistryEntryGetChildIterator(obj, kIOServicePlane, &kids) == KERN_SUCCESS) {
                    io_object_t kid = 0;
                    while ((kid = IOIteratorNext(kids)) != 0) {
                        CFStringRef nameRef = (CFStringRef)IORegistryEntryCreateCFProperty(
                            kid, CFSTR("BSD Name"), kCFAllocatorDefault, 0);
                        if (nameRef) {
                            const CFIndex len = CFStringGetLength(nameRef);
                            if (len > 0) {
                                // BSD 名都是 ASCII，转 UTF-8 读即可
                                QByteArray buf(int(len) * 4 + 1, 0);
                                if (CFStringGetCString(nameRef, buf.data(), buf.size(), kCFStringEncodingUTF8))
                                    bsdName = QString::fromUtf8(buf.constData());
                            }
                            CFRelease(nameRef);
                        }
                        // 取该介质在注册表里的条目名（如 "APPLE SSD AP0256Q Media"），用于友好显示
                        char entryName[128] = {0};
                        if (IORegistryEntryGetName(kid, entryName) == KERN_SUCCESS && entryName[0] != 0)
                            mediaName = QString::fromUtf8(entryName);
                        IOObjectRelease(kid);
                        if (!bsdName.isEmpty()) break;
                    }
                    IOObjectRelease(kids);
                }
                // 取不到 BSD 名就跳过（避免多块盘混叠到同一个 key 上，导致差值失真）
                if (bsdName.isEmpty()) { IOObjectRelease(obj); continue; }

                if (!mediaName.isEmpty()) this->diskLabelMap.insert(bsdName, bsdName + QStringLiteral(" · ") + mediaName);

                // 2) 累计读写字节：Statistics 字典
                quint64 totalRead = 0, totalWrite = 0;
                CFMutableDictionaryRef props = nullptr;
                if (IORegistryEntryCreateCFProperties(obj, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS && props) {
                    CFDictionaryRef stats = (CFDictionaryRef)CFDictionaryGetValue(props, CFSTR("Statistics"));
                    if (stats && CFGetTypeID(stats) == CFDictionaryGetTypeID()) {
                        CFNumberRef rdNum = (CFNumberRef)CFDictionaryGetValue(stats, CFSTR("Bytes (Read)"));
                        CFNumberRef wrNum = (CFNumberRef)CFDictionaryGetValue(stats, CFSTR("Bytes (Write)"));
                        double val = 0.0;
                        if (rdNum && CFNumberGetValue(rdNum, kCFNumberDoubleType, &val)) totalRead  = quint64(val);
                        val = 0.0;
                        if (wrNum && CFNumberGetValue(wrNum, kCFNumberDoubleType, &val)) totalWrite = quint64(val);
                    }
                    CFRelease(props);
                }
                IOObjectRelease(obj);

                DiskIoStat &s = this->diskStats[bsdName];
                const quint64 rdDelta = (totalRead  >= s.read_total)  ? (totalRead  - s.read_total)  : 0;
                const quint64 wrDelta = (totalWrite >= s.write_total) ? (totalWrite - s.write_total) : 0;
                s.read_total  = totalRead;
                s.write_total = totalWrite;
                diskDelta.insert(bsdName, qMakePair(rdDelta, wrDelta));
            }
            IOObjectRelease(iter);
        }
        this->finalizeDiskIo(diskDelta);
    }

    this->lastUpdateTime = QDateTime::currentMSecsSinceEpoch();
}

#endif // Q_OS_MACOS

/* =====================================================================================
 *                                  Windows 平台实现
 *
 *    - 内存 / 页面文件(swap) : GlobalMemoryStatusEx（KB，与 Linux/macOS 口径一致）
 *    - CPU 占用             : GetSystemTimes 的 tick 差值
 *    - CPU 频率             : 注册表 ~MHz（标称主频）
 *    - CPU 温度             : WMI MSAcpi_ThermalZoneTemperature（ACPI 热区，启动时探测一次）
 *    - 网速                 : GetIfTable2 累计字节的差值
 * ===================================================================================== */
#if defined(Q_OS_WIN)

void SysInfo::checkTemperatorFilePath()
{
    // 若同目录附带了 LibreHardwareMonitor.exe，先把它拉起（隐藏窗口），
    // 之后每轮的 Web 读取才能取到真实 CPU 温度。
    ensureLibreHardwareMonitor();

    double c = 0.0;
    if (readWinCpuTempCelsius(&c)) {
        this->cpuTemperature   = c;
        this->cpuTemperatureOk = true;
    } else {
        this->cpuTemperature   = 0.0;
        this->cpuTemperatureOk = false;
    }
    qDebug() << "[SysInfo] arch=" << QSysInfo::currentCpuArchitecture()
             << (this->cpuTemperatureOk
                     ? QString("CPU 温度: %1 度（来源：WinRing0 驱动 / LibreHardwareMonitor / ACPI 热区）").arg(this->cpuTemperature)
                     : QString("未读到 CPU 温度（需管理员运行；可把 WinRing0x64.sys 放到程序同目录，或运行 LibreHardwareMonitor 并开启 Web 服务），将不显示温度"));
}

void SysInfo::updateSysinfo()
{
    // ---------------------------------------------------------------- 内存 / 页面文件
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        memoryInfo.mem_total     = ms.ullTotalPhys / 1024;          // KB
        memoryInfo.mem_available = ms.ullAvailPhys / 1024;
        memoryInfo.mem_free      = ms.ullAvailPhys / 1024;
        memoryInfo.mem_used      = (ms.ullTotalPhys - ms.ullAvailPhys) / 1024;
        // Windows 没有独立 swap，用「页面文件(pagefile)」体积作为交换分区口径
        memoryInfo.swap_total    = ms.ullTotalPageFile / 1024;
        memoryInfo.swap_free     = ms.ullAvailPageFile / 1024;
        memoryInfo.swap_used     = (ms.ullTotalPageFile - ms.ullAvailPageFile) / 1024;
        // Windows 不把 buffers/缓存按用户程序口径暴露，图表用不到，置 0
        memoryInfo.cached        = 0;
        memoryInfo.buffers       = 0;
        this->memOk  = (memoryInfo.mem_total  > 0);
        this->swapOk = (memoryInfo.swap_total > 0);
    } else {
        this->memOk = this->swapOk = false;
    }

    // ---------------------------------------------------------------------- CPU 占用
    // GetSystemTimes：kernel 时间已包含 idle 线程，total = kernel + user、
    // 空闲部分 = idle，占用 = (total - idle) / total。
    FILETIME ftIdle, ftKernel, ftUser;
    if (GetSystemTimes(&ftIdle, &ftKernel, &ftUser)) {
        const double idle   = double((quint64(ftIdle.dwHighDateTime)  << 32) | ftIdle.dwLowDateTime);
        const double kernel = double((quint64(ftKernel.dwHighDateTime) << 32) | ftKernel.dwLowDateTime);
        const double user   = double((quint64(ftUser.dwHighDateTime)  << 32) | ftUser.dwLowDateTime);
        const double total  = kernel + user;

        if (this->cpuUsageHasPrev && total > this->cpuUsageTotalLast) {
            const double dTotal = total - this->cpuUsageTotalLast;
            const double dIdle  = idle  - this->cpuUsageIdleLast;
            double usage = (1.0 - dIdle / dTotal) * 100.0;
            if (usage < 0.0)   usage = 0.0;
            if (usage > 100.0) usage = 100.0;
            this->cpuUsage = usage;
        }
        this->cpuUsageTotalLast = total;
        this->cpuUsageIdleLast  = idle;
        this->cpuUsageHasPrev   = true;
    }

    // ---------------------------------------------------------------------- CPU 频率
    double mhz = 0.0;
    if (probeWinCpuFreqMHz(&mhz)) {
        this->cpuFreq   = mhz;
        this->cpuFreqOk = true;
    } else {
        this->cpuFreq   = 0.0;
        this->cpuFreqOk = false;
    }

    // ---------------------------------------------------------------------- CPU 温度
    // 每轮都重新读取（优先 LibreHardwareMonitor，回退 ACPI 热区），
    // 这样温度会实时刷新，而不是像以前只在启动时读一次。
    double t = 0.0;
    if (readWinCpuTempCelsius(&t)) {
        this->cpuTemperature   = t;
        this->cpuTemperatureOk = true;
    } else {
        this->cpuTemperature   = 0.0;
        this->cpuTemperatureOk = false;
    }

    // ---------------------------------------------------------------------- 网速
    MIB_IF_TABLE2 *table = nullptr;
    if (GetIfTable2(&table) == NO_ERROR && table != nullptr) {
        quint64 rx = 0;
        quint64 tx = 0;
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2 &r = table->Table[i];
            if (r.OperStatus != IfOperStatusUp)      continue;   // 只统计已连接的接口
            if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;   // 回环
            if (r.Type == IF_TYPE_TUNNEL)            continue;   // 隧道（IPv6 转换等）
            // 常见虚拟/桥接网卡（VirtualBox / VMware / Hyper-V / WSL），避免重复计数
            const QString alias = QString::fromWCharArray(r.Alias);
            if (alias.startsWith(QLatin1String("vEthernet"))
                || alias.contains(QLatin1String("Virtual"))
                || alias.startsWith(QLatin1String("Local Area Connection*")))
                continue;
            rx += quint64(r.InOctets);
            tx += quint64(r.OutOctets);
        }
        FreeMibTable(table);

        if (!this->netInited) {
            // 首帧只记基准不产出速率：receive_last 初值 0，若直接相减会把网卡累计字节
            // 当成"本间隔增量"，除以 450ms 后显示成几千 MB/s（9999 封顶），持续约 10 秒
            this->receive = 0;
            this->transmit = 0;
            this->receive_last = rx;
            this->transmit_last = tx;
            this->netInited = true;
        } else {
            this->receive  = (rx >= this->receive_last)  ? (rx - this->receive_last)  : 0;
            this->transmit = (tx >= this->transmit_last) ? (tx - this->transmit_last) : 0;
            this->receive_last  = rx;
            this->transmit_last = tx;
        }
    }

    // ---------------------------------------------------------------------- 磁盘 IO（Windows：PDH 性能计数器读每秒速率，逐盘统计）
    // 之前用 WMI Win32_PerfFormattedData_PerfDisk_PhysicalDisk：首次查询 2~3 秒、
    // 之后每次也要 300ms+，会卡住 450ms 周期的采样线程；而且初始化一旦失败就
    // 永久跳过（wmiTried），磁盘速度从此恒为 0。改用 PDH：每次查询 <10ms，
    // 实例名（如 "0 C:"）与 WMI 完全一致，磁盘选择（disk_io_mode / disk_io_name）不用改。
    {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        double sec = (this->diskLastSampleMs > 0) ? (nowMs - this->diskLastSampleMs) / 1000.0 : 1.0;
        if (sec <= 0.0) sec = 1.0;
        this->diskLastSampleMs = nowMs;

        // 初始化：打开查询 + 枚举 PhysicalDisk 实例 + 逐盘加读/写计数器。
        // 失败不忙循环：至少隔 2 秒再重试一次（PDH 是系统内置 API，几乎不会失败，仅兜底）。
        if (!this->pdhReady) {
            if (this->pdhTried && (nowMs - this->pdhTryMs) < 2000) {
                this->finalizeDiskIo(QHash<QString, QPair<quint64, quint64>>());
                return;
            }
            this->pdhTried = true;
            this->pdhTryMs  = nowMs;
            this->pdhDisks.clear();
            if (this->pdhQuery != nullptr) { PdhCloseQuery(this->pdhQuery); this->pdhQuery = nullptr; }

            PDH_HQUERY query = nullptr;
            if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS || query == nullptr) {
                this->finalizeDiskIo(QHash<QString, QPair<quint64, quint64>>());
                return;
            }

            // 用通配符展开枚举实例。实测本机 PdhEnumObjectItemsW 的缓冲语义不稳定
            // （第二次调用返回 0xC0000BBD=PDH_CSTATUS_INVALID_DATA 且实例列表为空），
            // 而 PdhExpandCounterPathW 稳定返回 "\\MACHINE\PhysicalDisk(0 C:)\..." 路径，
            // 解析路径即可拿到与 WMI 一致的实例名。
            DWORD pathSize = 0;
            PDH_STATUS st = PdhExpandCounterPathW(L"\\PhysicalDisk(*)\\Disk Read Bytes/sec",
                                                  nullptr, &pathSize);
            if (st == 0x800007D2L && pathSize > 0) {
                QVector<wchar_t> pbuf(pathSize);
                st = PdhExpandCounterPathW(L"\\PhysicalDisk(*)\\Disk Read Bytes/sec",
                                           pbuf.data(), &pathSize);
                if (st == ERROR_SUCCESS) {
                    for (const wchar_t *p = pbuf.constData(); *p != L'\0'; p += wcslen(p) + 1) {
                        // 路径形如 \\MACHINE\PhysicalDisk(0 C:)\Disk Read Bytes/sec
                        const QString path = QString::fromWCharArray(p);
                        const int lp = path.indexOf(QLatin1String("PhysicalDisk("));
                        const int rp = path.indexOf(QLatin1Char(')'), lp + 13);
                        if (lp < 0 || rp < 0) continue;
                        const QString inst = path.mid(lp + 13, rp - lp - 13);
                        if (inst == QLatin1String("_Total")) continue;   // 汇总伪实例，跳过
                        const QString rPath = QStringLiteral("\\PhysicalDisk(") + inst
                                              + QStringLiteral(")\\Disk Read Bytes/sec");
                        const QString wPath = QStringLiteral("\\PhysicalDisk(") + inst
                                              + QStringLiteral(")\\Disk Write Bytes/sec");
                        PDH_HCOUNTER hR = nullptr, hW = nullptr;
                        PdhAddEnglishCounterW(query, reinterpret_cast<LPCWSTR>(rPath.utf16()), 0, &hR);
                        PdhAddEnglishCounterW(query, reinterpret_cast<LPCWSTR>(wPath.utf16()), 0, &hW);
                        if (hR != nullptr || hW != nullptr)
                            this->pdhDisks.push_back({ inst, hR, hW });
                    }
                }
            }
            if (!this->pdhDisks.isEmpty()) {
                this->pdhQuery = query;
                this->pdhReady = true;
                PdhCollectQueryData(query);   // 首次调用只是建立基线（返回 PDH_NO_DATA 属正常）
            } else {
                PdhCloseQuery(query);
            }
        }

        if (!this->pdhReady) {
            this->finalizeDiskIo(QHash<QString, QPair<quint64, quint64>>());
            return;
        }

        // 每次采样：收集一轮数据，逐盘取"每秒速率"，乘间隔秒数换算成本间隔字节数
        // （与 Linux/macOS 的差值口径一致，UI 侧再除以秒数还原成 MB/s）。
        PdhCollectQueryData(this->pdhQuery);
        QHash<QString, QPair<quint64, quint64>> diskDelta;   // 设备名 -> (本间隔读字节, 写字节)
        for (const PdhDiskCounter &d : this->pdhDisks) {
            quint64 rdRate = 0, wrRate = 0;
            PDH_FMT_COUNTERVALUE v;
            DWORD fmtType = 0;
            if (d.hRead != nullptr
                && PdhGetFormattedCounterValue(d.hRead, PDH_FMT_LARGE | PDH_FMT_NOCAP100, &fmtType, &v) == ERROR_SUCCESS
                && v.CStatus == ERROR_SUCCESS)
                rdRate = static_cast<quint64>(v.largeValue);
            if (d.hWrite != nullptr
                && PdhGetFormattedCounterValue(d.hWrite, PDH_FMT_LARGE | PDH_FMT_NOCAP100, &fmtType, &v) == ERROR_SUCCESS
                && v.CStatus == ERROR_SUCCESS)
                wrRate = static_cast<quint64>(v.largeValue);
            diskDelta.insert(d.name, qMakePair(quint64(rdRate * sec), quint64(wrRate * sec)));
        }
        this->finalizeDiskIo(diskDelta);
    }

    this->lastUpdateTime = QDateTime::currentMSecsSinceEpoch();
}

#endif // Q_OS_WIN

/* =====================================================================================
 *                                       get
 * ===================================================================================== */
qulonglong SysInfo::getMemTotal()      { return this->memoryInfo.mem_total; }
qulonglong SysInfo::getMemUsed()       { return this->memoryInfo.mem_used; }
qulonglong SysInfo::getMemFree()       { return this->memoryInfo.mem_free; }
qulonglong SysInfo::getSwapTotal()     { return this->memoryInfo.swap_total; }
qulonglong SysInfo::getSwapUsed()      { return this->memoryInfo.swap_used; }
qulonglong SysInfo::getSwapFree()      { return this->memoryInfo.swap_free; }
double     SysInfo::getCpuFreq()       { return this->cpuFreq; }
double     SysInfo::getCpuUsage()      { return this->cpuUsage; }
double     SysInfo::getCpuTemperature(){ return this->cpuTemperature; }
qulonglong SysInfo::getReceive()       { return this->receive; }
qulonglong SysInfo::getTransmit()      { return this->transmit; }
qulonglong SysInfo::getDiskReadBytes()  { return this->disk_read; }
qulonglong SysInfo::getDiskWriteBytes() { return this->disk_write; }

// 设置生效盘：mode 0 = IO 最高的盘, 1 = 指定盘(name)
void SysInfo::setDiskSelection(qint8 mode, const QString &name)
{
    this->diskSelectMode = mode;
    this->diskSelectName = name;
}

// 当前生效的磁盘名（供悬浮球 tooltip / 设置回显）
QString SysInfo::getDiskActiveName() const
{
    return this->diskActiveName;
}

// 最近一次能取到的磁盘名列表（供设置 UI 填充下拉框）
QStringList SysInfo::getDiskNames() const
{
    return this->diskAvailableNames;
}

// 磁盘的友好显示名（取不到时返回原名本身）
QString SysInfo::getDiskLabel(const QString &name) const
{
    const QString label = this->diskLabelMap.value(name);
    return label.isEmpty() ? name : label;
}

// 公共收尾：根据各盘本间隔 (读,写) 差值，按"指定盘 / IO 最高的盘"选出生效盘。
void SysInfo::finalizeDiskIo(const QHash<QString, QPair<quint64, quint64>> &delta)
{
    this->diskAvailableNames.clear();
    for (auto it = delta.begin(); it != delta.end(); ++it) {
        const QString &name = it.key();
        DiskIoStat &s = this->diskStats[name];
        s.read_speed  = it.value().first;
        s.write_speed = it.value().second;
        s.seen = true;
        this->diskAvailableNames.append(name);
    }

    // 选生效盘
    QString active;
    if (this->diskSelectMode == 1 && this->diskStats.contains(this->diskSelectName)
        && this->diskStats.value(this->diskSelectName).seen) {
        active = this->diskSelectName;          // 指定盘且当前存在
    } else {
        // IO 最高的盘：读+写 速度最大者；并列或全 0 时取第一个 seen 的盘
        quint64 best = 0;
        bool first = true;
        for (auto it = this->diskStats.begin(); it != this->diskStats.end(); ++it) {
            if (!it.value().seen) continue;
            const quint64 io = it.value().read_speed + it.value().write_speed;
            if (first || io > best) { best = io; active = it.key(); first = false; }
        }
    }

    if (!active.isEmpty()) {
        this->diskActiveName = active;
        this->disk_read  = this->diskStats.value(active).read_speed;
        this->disk_write = this->diskStats.value(active).write_speed;
        this->diskIoOk = true;
    } else {
        this->diskActiveName.clear();
        this->disk_read = this->disk_write = 0;
        this->diskIoOk = false;
    }
}

bool SysInfo::isMemAvailable()            { return this->memOk; }
bool SysInfo::isSwapAvailable()           { return this->swapOk; }
bool SysInfo::isCpuFreqAvailable()        { return this->cpuFreqOk; }
bool SysInfo::isCpuTemperatureAvailable() { return this->cpuTemperatureOk; }
bool SysInfo::isDiskIoAvailable()         { return this->diskIoOk; }
