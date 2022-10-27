#include "widget.h"

Widget::Widget(QWidget *parent)
    : QWidget(parent)
{
    // config file check and read
    this->config = new Config();
    // get system info
    this->sysInfo = new SysInfo();

    // widget set
    this->setPosition();
    this->setUiFrame();
}

Widget::~Widget()
{
    delete this->config;
    delete this->sysInfo;
}

void Widget::setPosition()
{
    this->setGeometry(config->getX(), config->getY(), config->getWidth(), config->getHeight());
}

void Widget::setUiFrame()
{
    this->setWindowFlag(Qt::WindowStaysOnTopHint);
    this->setAttribute(Qt::WA_TranslucentBackground);
    this->setWindowOpacity(config->getOpacity());
    this->setFixedSize(config->getWidth(), config->getHeight());
}

