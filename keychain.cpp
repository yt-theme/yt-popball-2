#include "keychain.h"

#if defined(Q_OS_MACOS)

#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>

namespace {

CFStringRef cfString(const QString &s)
{
    return CFStringCreateWithCString(kCFAllocatorDefault,
                                     s.toUtf8().constData(),
                                     kCFStringEncodingUTF8);
}

// 通用密码条目的定位条件（class + service + account）
CFDictionaryRef itemQuery(CFStringRef svc, CFStringRef acc, int extraCount = 0,
                          const void **extraKeys = nullptr,
                          const void **extraVals = nullptr)
{
    const void *keys[5] = { kSecClass, kSecAttrService, kSecAttrAccount, nullptr, nullptr };
    const void *vals[5] = { kSecClassGenericPassword, svc, acc, nullptr, nullptr };
    for (int i = 0; i < extraCount; ++i) {
        keys[3 + i] = extraKeys[i];
        vals[3 + i] = extraVals[i];
    }
    return CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 3 + extraCount,
                              &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);
}

} // namespace

namespace Keychain {

QString readSecret(const QString &service, const QString &account)
{
    CFStringRef svc = cfString(service);
    CFStringRef acc = cfString(account);
    const void *extraKeys[] = { kSecReturnData, kSecMatchLimit };
    const void *extraVals[] = { kCFBooleanTrue, kSecMatchLimitOne };
    CFDictionaryRef query = itemQuery(svc, acc, 2, extraKeys, extraVals);

    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    CFRelease(svc);
    CFRelease(acc);

    QString out;
    if (status == errSecSuccess && result != nullptr
        && CFGetTypeID(result) == CFDataGetTypeID()) {
        CFDataRef data = static_cast<CFDataRef>(result);
        out = QString::fromUtf8(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
                                int(CFDataGetLength(data)));
        CFRelease(result);
    }
    return out;
}

bool writeSecret(const QString &service, const QString &account, const QString &value)
{
    if (value.isEmpty())
        return removeSecret(service, account);

    CFStringRef svc = cfString(service);
    CFStringRef acc = cfString(account);
    QByteArray utf8 = value.toUtf8();
    CFDataRef data = CFDataCreate(kCFAllocatorDefault,
                                  reinterpret_cast<const UInt8 *>(utf8.constData()),
                                  utf8.size());
    CFDictionaryRef query = itemQuery(svc, acc);

    // 先尝试更新已有条目，没有则新增
    const void *attrKeys[] = { kSecValueData };
    const void *attrVals[] = { data };
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, attrKeys, attrVals, 1,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    OSStatus status = SecItemUpdate(query, attrs);

    if (status == errSecItemNotFound) {
        const void *addKeys[] = { kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData };
        const void *addVals[] = { kSecClassGenericPassword, svc, acc, data };
        CFDictionaryRef add = CFDictionaryCreate(kCFAllocatorDefault, addKeys, addVals, 4,
                                                 &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
        status = SecItemAdd(add, nullptr);
        CFRelease(add);
    }

    CFRelease(attrs);
    CFRelease(query);
    CFRelease(data);
    CFRelease(svc);
    CFRelease(acc);
    return status == errSecSuccess;
}

bool removeSecret(const QString &service, const QString &account)
{
    CFStringRef svc = cfString(service);
    CFStringRef acc = cfString(account);
    CFDictionaryRef query = itemQuery(svc, acc);
    const OSStatus status = SecItemDelete(query);
    CFRelease(query);
    CFRelease(svc);
    CFRelease(acc);
    // 本来就没有也算成功（幂等）
    return status == errSecSuccess || status == errSecItemNotFound;
}

} // namespace Keychain

#else

// 非 macOS：不提供钥匙串实现，调用方继续使用配置文件保存。
namespace Keychain {

QString readSecret(const QString &, const QString &) { return QString(); }
bool writeSecret(const QString &, const QString &, const QString &) { return false; }
bool removeSecret(const QString &, const QString &) { return false; }

} // namespace Keychain

#endif
