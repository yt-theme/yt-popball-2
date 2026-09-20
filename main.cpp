#include "widget.h"

#include <QApplication>
#include <QLocale>
#include <QTranslator>
#include <QIcon>
#include <QSettings>
#include <QDir>

#if defined(Q_OS_MACOS)
#  include "macwindow.h"
#endif

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // 悬浮挂件没有"可以关闭的窗口"，不应该因为窗口被隐藏/关闭就退出程序。
    // （Qt 默认 quitOnLastWindowClosed=true，窗口一旦被关掉整个进程就会结束）
    a.setQuitOnLastWindowClosed(false);

    // 应用图标：Linux 的任务栏 / 程序菜单 / 窗口标题栏都会用它。
    // （macOS 的 Dock 图标来自 .app 包里的 .icns，见 .pro 的 ICON 与 package.sh）
    a.setWindowIcon(QIcon(QStringLiteral(":/icons/resources/popball2.png")));

#if defined(Q_OS_MACOS)
    // 像 360 悬浮球那样：不在 Dock 显示应用图标，也不出现在 Cmd+Tab 里。
    // 必须在 QApplication 构造之后尽早调用。
    popballHideFromDock();
#endif

    // 界面语言：设置里的「界面语言」优先（0=跟随系统 1=简体中文 2=English）。
    // 跟随系统时按 Qt 惯例遍历 QLocale::system().uiLanguages() 逐级匹配（zh-CN → zh → …）。
    QTranslator translator;
    bool loaded = false;
    const QSettings userCfg(QDir::home().filePath(".popball2_config.ini"),
                            QSettings::IniFormat);
    const int lang = userCfg.value("/ui/language", 0).toInt();
    if (lang == 1) {
        loaded = translator.load(QStringLiteral(":/i18n/popball2_zh_CN"));
    } else if (lang == 2) {
        loaded = translator.load(QStringLiteral(":/i18n/popball2_en"));
    } else {
        const QStringList uiLanguages = QLocale::system().uiLanguages();
        for (const QString &locale : uiLanguages) {
            const QString baseName = "popball2_" + QLocale(locale).name();
            if (translator.load(QStringLiteral(":/i18n/") + baseName)) {
                loaded = true;
                break;
            }
        }
    }
    if (loaded)
        a.installTranslator(&translator);
    Widget w;
    w.show();
    return a.exec();
}
