#ifndef KEYCHAIN_H
#define KEYCHAIN_H

#include <QString>

// 系统钥匙串读写（目前仅 macOS 有实现；其它平台返回空/失败，调用方自动退回配置文件）。
// 用于保存 AI 的 API Key，避免明文写进 ~/.popball2_config.ini。
namespace Keychain {

// 读取；不存在或无法访问时返回空字符串
QString readSecret(const QString &service, const QString &account);

// 写入（已存在则覆盖）；返回是否成功
bool writeSecret(const QString &service, const QString &account, const QString &value);

// 删除；不存在也算成功
bool removeSecret(const QString &service, const QString &account);

} // namespace Keychain

#endif // KEYCHAIN_H
