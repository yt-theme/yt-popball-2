# drivers/ —— Windows CPU 温度所需的内核驱动

Windows 没有公开的「用户态读 CPU 核心温度」API，必须借助内核驱动访问 CPU 的 MSR
寄存器（Intel：MSR 0x1A2/0x19C；AMD：SMN 0x00059800）。这里放的就是 WinRing0 驱动，
`package.sh win` 会把对应架构的 `.sys` 一并打进 Windows 发行包。

| 文件 | 架构 | 大小 | SHA-256 |
|---|---|---|---|
| `WinRing0x64.sys` | x86_64 | 14,544 B | `11bd2c9f9e2397c9a16e0990e4ed2cf0679498fe0fd418a3dfdac60b5c160ee5` |
| `WinRing0.sys` | x86 | 14,416 B | `206ee7a7c3f4d9496f742ccb84718f556ecb4ba2a95fe7e0cdf3a003ffbe4597` |

## 来源与提取方式

这两个文件是从 **LibreHardwareMonitor v0.9.4 官方发行包**（`LibreHardwareMonitor-net472.zip`）
的 `LibreHardwareMonitorLib.dll` 中提取的：驱动以 gzip 压缩的托管资源内嵌在该程序集里
（资源名 `WinRing0.gz` / `WinRing0x64.gz`）。提取脚本见 `.workbuddy/extract_gz3.py`。

> 注意：同一个程序集里还有一对约 96 KB 的 PE 镜像，**那不是** WinRing0 驱动；
> 真正的驱动是含 `WinRing0` 字符串、大小约 14 KB 的那两个。

## 许可

WinRing0 源自 OpenLibSys（Noriyuki Miyazaki），以 **GPLv2** 发布，与本项目许可一致，
可随包分发。

## 运行时行为

- 程序启动时若发现同目录存在 `WinRing0x64.sys`（32 位系统为 `WinRing0.sys`），会把它注册成
  内核服务 `WinRing0_1_2_0` 并启动，随后读 MSR 取温度。
- 注册内核服务需要管理员权限：非管理员运行时会**弹一次 UAC 提权**完成安装，之后服务常驻，
  不再重复提示。
- 安装结果记录在 `%TEMP%\popball2_winring0_install.log`（提权子进程无控制台，故落盘）。

## 已知限制

该驱动版本（1.2.0）**未签名**，在 Microsoft 易受攻击驱动黑名单（Microsoft Vulnerable
Driver Blocklist）中。因此：

- 系统未开启「内存完整性 / HVCI」时：可正常加载（Win11 24H2 默认多数情况如此）。
- 开启了「内存完整性」时：加载会被拦截（`StartService` 返回 1275），此时 CPU 温度读不到，
  程序会自动回退到其它温度来源（LibreHardwareMonitor Web / OpenHardwareMonitor WMI /
  ACPI 热区），读不到就不显示温度，绝不显示假数据。
