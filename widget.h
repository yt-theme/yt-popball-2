#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include "config.h"
#include "sysInfo.h"

class Widget : public QWidget
{
    Q_OBJECT
private:
    Config *config;
    SysInfo *sysInfo;

public:
    Widget(QWidget *parent = nullptr);
    ~Widget();

    void setPosition();
    void setUiFrame();


};
#endif // WIDGET_H
