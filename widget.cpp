#include "widget.h"

Widget::Widget(QWidget *parent)
    : QWidget(parent)
{
    // config file check and read
    this->config = new Config();
    // get system info
    this->sysInfo = new SysInfo();
    // update data timer
    this->updateDataTimer = new QTimer();
    connect(updateDataTimer, &QTimer::timeout, this, &Widget::onTimerIntervalForUpdateData);
    // widget set
    this->setPosition();
    this->setUiFrame();

    // ################# widget ###################
    // cpu freq LCD
    this->cpuFreqLCD = new QLCDNumber(this);
    this->cpuFreqLCD->setDigitCount(5);
    this->cpuFreqLCD->setMode(QLCDNumber::Dec);
    this->cpuFreqLCD->setSegmentStyle(QLCDNumber::Flat);
    this->cpuFreqLCD->setGeometry(0, config->getHeight()/5.5, config->getWidth(), config->getWidth()/6);
    this->cpuFreqLCD->setStyleSheet("border: 0;color:" + config->getCpuFreqColor() + ";");
    this->cpuFreqLCD->display("00'c");
}

Widget::~Widget()
{
    delete this->config;
    delete this->sysInfo;
    delete this->updateDataTimer;
    delete this->winShadow;
    delete this->cpuFreqLCD;
}

void Widget::setPosition()
{
    this->setGeometry(config->getX(), config->getY(), config->getWidth(), config->getHeight());
}

void Widget::setUiFrame()
{
    this->setWindowFlag(Qt::WindowStaysOnTopHint);
    this->setWindowFlag(Qt::FramelessWindowHint);
    this->setAttribute(Qt::WA_TranslucentBackground);
    this->setWindowOpacity(config->getOpacity());
    this->setFixedSize(config->getWidth(), config->getHeight());

    // shadow
    this->winShadow = new QGraphicsDropShadowEffect(this);
    winShadow->setOffset(0, 0);
    winShadow->setColor(Qt::black);
    winShadow->setBlurRadius(config->getShadowRadius());
    this->setGraphicsEffect(winShadow);

    // charts rows
    qint32 charts_rows = config->getChartsRows();
    for (int i=0; i<charts_rows; i++)
    {
        this->mem_data_history.push_back(0);
        this->swap_data_history.push_back(0);
        this->cpuUsage_data_history.push_back(0);
    }

    // timer
    this->updateDataTimer->stop();
    this->updateDataTimer->setInterval(config->getUpdateDataInterval());
    this->updateDataTimer->start();



}

void Widget::onTimerIntervalForUpdateData()
{
    this->updateDataAndHistory();

    // tmp update ui
    this->update();
}

void Widget::updateDataAndHistory()
{
    this->sysInfo->updateSysinfo();
    // cpu usage history
    if ((cpuUsage_data_history.size() + 1) >= config->getChartsRows()) cpuUsage_data_history.pop_front();
    this->cpuUsage_data_history.push_back(this->sysInfo->getCpuUsage());
    // mem history
    if ((mem_data_history.size() + 1) >= config->getChartsRows()) mem_data_history.pop_front();
    this->mem_data_history.push_back(this->sysInfo->getMemTotal() - this->sysInfo->getMemFree());
    // swap history
    if ((swap_data_history.size() + 1) >= config->getChartsRows()) swap_data_history.pop_front();
    this->swap_data_history.push_back(this->sysInfo->getSwapTotal() - this->sysInfo->getSwapFree());
}

void Widget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    QPainterPath painterPath;

    // ui shape
    switch (this->config->getShape())
    {
    case SHAPE_CIRCLE:
    {
        qint32 main_border_width    = config->getMainBorderWidth();
        qint32 shadow_radius        = config->getShadowRadius();
        qint32 main_width           = config->getWidth();
        qint32 main_height          = config->getHeight();

        this->setFixedSize(main_width, main_height);
        painter.setRenderHint(QPainter::Antialiasing);

        // main circle && border
        painter.setBrush(QColor(config->getMainColor()));
        QPen pen(QColor(config->getMainBorderColor()), main_border_width, Qt::SolidLine, Qt::SquareCap, Qt::RoundJoin);
        painter.setPen(pen);
        painter.drawEllipse(
                    main_border_width/2 + shadow_radius,
                    main_border_width/2 + shadow_radius,
                    main_width  - main_border_width - (shadow_radius * 2),
                    main_height - main_border_width - (shadow_radius * 2) );

        // clip
        painterPath.moveTo(main_border_width/2 + shadow_radius, main_border_width/2 + shadow_radius);
        painterPath.arcTo(main_border_width/2 + shadow_radius,main_border_width/2 + shadow_radius,main_width  - main_border_width - (shadow_radius * 2), main_height - main_border_width - (shadow_radius * 2), 0, 360);
        painter.setClipPath(painterPath);

        // cpu freq LCD

        // temp LCD
        this->cpuFreqLCD->display(QString("%1'c").arg(qRound(this->sysInfo->getCpuTemperature())));
        qDebug() << "show LCD =>" << this->sysInfo->getCpuTemperature() << "\n";


    }
    }
}
