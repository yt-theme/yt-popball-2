#include "sysInfo.h"

#include <QRegularExpression>
#include <QHash>
#include <QSysInfo>
#include <cstring>

#if defined(Q_OS_MACOS)
#include <IOKit/IOKitLib.h>
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
#endif // Q_OS_WIN

} // namespace

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
    delete this->_file_obj;
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
    this->cpuTemperatureOk = false;
    double c = 0.0;
    if (probeWinCpuTempCelsius(&c)) {
        this->cpuTemperature   = c;
        this->cpuTemperatureOk = true;
    } else {
        this->cpuTemperature   = 0.0;
    }
    qDebug() << "[SysInfo] arch=" << QSysInfo::currentCpuArchitecture()
             << (this->cpuTemperatureOk
                     ? QString("ACPI 热区温度: %1 度").arg(this->cpuTemperature)
                     : QString("未读到 ACPI 热区温度（常发生于虚拟机），将不显示温度"));
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

        this->receive  = (rx >= this->receive_last)  ? (rx - this->receive_last)  : 0;
        this->transmit = (tx >= this->transmit_last) ? (tx - this->transmit_last) : 0;
        this->receive_last  = rx;
        this->transmit_last = tx;
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

bool SysInfo::isMemAvailable()            { return this->memOk; }
bool SysInfo::isSwapAvailable()           { return this->swapOk; }
bool SysInfo::isCpuFreqAvailable()        { return this->cpuFreqOk; }
bool SysInfo::isCpuTemperatureAvailable() { return this->cpuTemperatureOk; }
