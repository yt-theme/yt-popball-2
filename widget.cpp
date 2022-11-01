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
    // update UI timer
    this->updateUITimer = new QTimer();
    connect(updateUITimer, &QTimer::timeout, this, &Widget::onTimerIntervalForUpdateUI);

    // widget set
    this->setPosition();
    this->setUiFrame();

    // ################# widget ###################
    // cpu temp LCD
    this->cpuTempLCD = new QLCDNumber(this);
    this->cpuTempLCD->setDigitCount(5);
    this->cpuTempLCD->setMode(QLCDNumber::Dec);
    this->cpuTempLCD->setSegmentStyle(QLCDNumber::Flat);
    this->cpuTempLCD->setGeometry(0, config->getHeight()/5.9, config->getWidth(), config->getWidth()/5.5);
    this->cpuTempLCD->setStyleSheet("border: 0;color:" + config->getCpuFreqColor() + ";");
    this->cpuTempLCD->display("00'c");
    // cpu freq LCD
    this->cpuFreqLCD = new QLCDNumber(this);
    this->cpuFreqLCD->setDigitCount(9);
    this->cpuFreqLCD->setMode(QLCDNumber::Dec);
    this->cpuFreqLCD->setSegmentStyle(QLCDNumber::Flat);
    this->cpuFreqLCD->setGeometry(0, config->getHeight()/5.7 + config->getWidth()/5.5, config->getWidth(), config->getWidth()/6.5);
    this->cpuFreqLCD->setStyleSheet("border: 0;color:" + config->getCpuFreqColor() + ";");
    this->cpuFreqLCD->display("000000000");
    // net upload LCD
    this->netUploadLCD = new QLCDNumber(this);
    this->netUploadLCD->setDigitCount(7);
    this->netUploadLCD->setMode(QLCDNumber::Dec);
    this->netUploadLCD->setSegmentStyle(QLCDNumber::Flat);
    this->netUploadLCD->setGeometry(0, config->getHeight()/5.7 + config->getWidth()/5.5 * 2, config->getWidth(), config->getWidth()/6.5);
    this->netUploadLCD->setStyleSheet("border: 0;color:" + config->getCpuFreqColor() + ";");
    this->netUploadLCD->display("0000000");
    // net downlod LCD
    this->netDownloadLCD = new QLCDNumber(this);
    this->netDownloadLCD->setDigitCount(7);
    this->netDownloadLCD->setMode(QLCDNumber::Dec);
    this->netDownloadLCD->setSegmentStyle(QLCDNumber::Flat);
    this->netDownloadLCD->setGeometry(0, config->getHeight()/5.7 + config->getWidth()/5.5*2.9, config->getWidth(), config->getWidth()/6.5);
    this->netDownloadLCD->setStyleSheet("border: 0;color:" + config->getCpuFreqColor() + ";");
    this->netDownloadLCD->display("0000000");
}

Widget::~Widget()
{
    delete this->config;
    delete this->sysInfo;
    delete this->updateDataTimer;
    delete this->updateUITimer;
    delete this->winShadow;
    delete this->cpuTempLCD;
    delete this->cpuFreqLCD;
    delete this->netUploadLCD;
    delete this->netDownloadLCD;
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

    QRect primaryScreenRect = QGuiApplication::primaryScreen()->geometry();

    // set default shape
    this->config->setShape(SHAPE_CIRCLE);

    // check y
    if (this->config->getY() <= 0)
    {
        this->config->setY(0);
    }
    if (this->config->getY() >= (primaryScreenRect.height() - this->frameGeometry().height()))
    {
        this->config->setY(primaryScreenRect.height() - this->frameGeometry().height());
    }

    // windowif at aside
    if ((this->config->getX() + this->frameGeometry().width()) >= primaryScreenRect.width()
            || this->config->getX() <= 0)
    {
        // set shape
        this->config->setShape(SHAPE_ASIDE);
        // check at aside left or right
        // left
        if (this->config->getX() <= 0)
        {
            this->config->setX(0 - this->config->getShadowRadius());
        }
        // right
        if ((this->config->getX() + this->frameGeometry().width()) >= primaryScreenRect.width())
        {
            this->config->setX(primaryScreenRect.width() - this->frameGeometry().width() + this->config->getShadowRadius());
        }
    }
    // set geometry
    this->setGeometry(this->config->getX(), this->config->getY(), this->config->getWidth(), this->config->getHeight());


    // shadow
    this->winShadow = new QGraphicsDropShadowEffect(this);
    winShadow->setOffset(0, 0);
    winShadow->setColor(Qt::black);
    winShadow->setBlurRadius(config->getShadowRadius());
    this->setGraphicsEffect(winShadow);

    // charts rows
    // charts rows
    qint32 charts_rows = config->getChartsRows();
    if (mem_data_history.size() < charts_rows) {
        for (int i=0; i<charts_rows; i++) this->mem_data_history.push_back(0);
    }
    if (swap_data_history.size() < charts_rows) {
        for (int i=0; i<charts_rows; i++) this->swap_data_history.push_back(0);
    }
    if (cpuUsage_data_history.size() < charts_rows) {
        for (int i=0; i<charts_rows; i++) this->cpuUsage_data_history.push_back(0);
    }

    // timer
    this->updateDataTimer->stop();
    this->updateDataTimer->setInterval(config->getUpdateDataInterval());
    this->updateDataTimer->start();

    this->updateUITimer->stop();
    this->updateUITimer->setInterval(config->getUpdateUIInterval());
    this->updateUITimer->start();

    this->update();

}

void Widget::onTimerIntervalForUpdateData()
{
    this->updateDataAndHistory();
}

void Widget::onTimerIntervalForUpdateUI()
{
    this->update();
}

void Widget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        this->isMousePressed = true;
        this->curWindowPos = event->pos();
    } else if (event->button() == Qt::RightButton) {

    }
}

void Widget::mouseMoveEvent(QMouseEvent *event)
{
    if (this->isMousePressed == true)
    {
        this->move(event->pos() - this->curWindowPos + this->pos());
    }
}

void Widget::mouseReleaseEvent(QMouseEvent *event)
{
    // store to config
    this->config->setX(this->frameGeometry().x());
    this->config->setY(this->frameGeometry().y());

    // reset ui frame
    this->setUiFrame();

    Q_UNUSED(event);
    this->isMousePressed = false;
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
    painter.setRenderHint(QPainter::Antialiasing);

    // ui shape
    switch (this->config->getShape())
    {
    case SHAPE_CIRCLE:
    case SHAPE_ASIDE: // tmp
    {
        this->cpuFreqLCD->show();

        qint32 main_border_width    = config->getMainBorderWidth();
        qint32 shadow_radius        = config->getShadowRadius();
        qint32 main_width           = config->getWidth();
        qint32 main_height          = config->getHeight();
        qint32 charts_rows          = config->getChartsRows();
        qint32 edging_width         = config->getMainBorderWidth() + config->getShadowRadius();


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
        QPainterPath clipPath;
        clipPath.moveTo(main_border_width/2 + shadow_radius, main_border_width/2 + shadow_radius);
        clipPath.arcTo(main_border_width/2 + shadow_radius + 2,
                       main_border_width/2 + shadow_radius + 2,
                       main_width  - main_border_width - (shadow_radius * 2) - 4,
                       main_height - main_border_width - (shadow_radius * 2) - 4,
                       0, 360);
        painter.setClipPath(clipPath);

        // cpu freq LCD
        this->cpuFreqLCD->display(QString("CPU %1").arg(qRound(this->sysInfo->getCpuFreq())));

        // temp LCD
        this->cpuTempLCD->display(QString("%1'c").arg(qRound(this->sysInfo->getCpuTemperature())));

        // net upload LCD
        this->netUploadLCD->display(QString("u %1").arg(QString::number(this->sysInfo->getTransmit()/1024.0/this->config->getUpdateDataInterval(), 'f', 2)));

        // net download LCD
        qDebug() << "receive =>" << this->sysInfo->getTransmit()/1024.0/this->config->getUpdateDataInterval();
        this->netDownloadLCD->display(QString("d %1").arg(QString::number(this->sysInfo->getReceive()/1024.0/this->config->getUpdateDataInterval(), 'f', 2)));

        // mem charts
        QPainterPath memPath;
        memPath.moveTo(0, main_height - edging_width);
        quint64 mem_total = this->sysInfo->getMemTotal();
        for (int i=0; i<this->mem_data_history.size(); i++)
        {
            memPath.lineTo((main_width/charts_rows)*i,
                           main_height - floor(this->mem_data_history[i] / 1024) / floor(mem_total / 1024) * main_height - edging_width);
        }
        memPath.lineTo(main_width, main_height - edging_width);
        memPath.lineTo(0, main_height - edging_width);
        painter.fillPath(memPath, QColor(this->config->getMemColor()));

        // swap charts
        QPainterPath swapPath;
        swapPath.moveTo(0, main_height - edging_width);
        quint64 swap_total = this->sysInfo->getSwapTotal();
        for (int i=0; i<this->swap_data_history.size(); i++)
        {
            swapPath.lineTo((main_width/charts_rows)*i,
                           main_height - floor(this->swap_data_history[i] / 1024) / floor(swap_total / 1024) * main_height - edging_width);
        }
        swapPath.lineTo(main_width, main_height - edging_width);
        swapPath.lineTo(0, main_height);
        painter.fillPath(swapPath, QColor(this->config->getSwapColor()));

        // cpu usage charts
        QPen cpuUsagePen;
        cpuUsagePen.setColor(config->getCpuUsageColor());
        cpuUsagePen.setStyle(Qt::SolidLine);
        cpuUsagePen.setWidthF(config->getCpuUsageWidth());
        painter.setPen(cpuUsagePen);
        QVector<double> cpuUsageData = this->cpuUsage_data_history;
        QPointF cpuUsagePoints[charts_rows];
        for (int i=0; i<charts_rows; i++)
        {
            cpuUsagePoints[i] = QPointF(main_width / charts_rows * i, main_height - (cpuUsageData[i]) - edging_width);
        }
        painter.drawPolyline(cpuUsagePoints, charts_rows);

        painter.end();
        break;
    }
//    case SHAPE_ASIDE:
//    {
//        this->cpuFreqLCD->hide();
//        qint32 main_border_width    = config->getMainBorderWidth();
//        qint32 shadow_radius        = config->getShadowRadius();
//        qint32 main_width           = config->getWidth();
//        qint32 main_height          = config->getHeight();

//        // main color
//        painter.setBrush(QColor(config->getMainColor()));
//        QPen pen(QColor(config->getMainBorderColor()), main_border_width, Qt::SolidLine, Qt::SquareCap, Qt::RoundJoin);
//        painter.setPen(pen);
//        painter.drawEllipse(
//                    main_border_width/2 + shadow_radius,
//                    main_border_width/2 + shadow_radius,
//                    main_width  - main_border_width - (shadow_radius * 2),
//                    main_height - main_border_width - (shadow_radius * 2) );



//        painter.end();
//        break;
//    }

    default: break;

    }
}
