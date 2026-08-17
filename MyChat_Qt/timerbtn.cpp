#include "timerbtn.h"
#include <QMouseEvent>
#include <QDebug>

TimerBtn::TimerBtn(QWidget *parent):QPushButton(parent), _counter(30), _isCounting(false)
{
    _timer = new QTimer(this);

    connect(_timer, &QTimer::timeout, [this](){
        _counter--;
        if (_counter <= 0){
            _timer->stop();
            _counter = 30;
            this->setText("获取");
            this->setEnabled(true);
            _isCounting = false;
            return;
        }
        this->setText(QString::number(_counter));
    });
}

TimerBtn::~TimerBtn()
{
    _timer->stop();
}

void TimerBtn::startCountdown(int seconds)
{
    if (_isCounting) return;
    _counter = seconds;
    _isCounting = true;
    this->setEnabled(false);
    this->setText(QString::number(_counter));
    _timer->start(1000);
}

void TimerBtn::stopCountdown()
{
    _timer->stop();
    this->setText("获取");
    this->setEnabled(true);
    _isCounting = false;
}

//void TimerBtn::mouseReleaseEvent(QMouseEvent *e)
//{
//    if (e->button() == Qt::LeftButton){
//        // 在这里处理鼠标左键释放事件
//        qDebug() << "MyTimerButton was released!";
//        this->setEnabled(false);
//        this->setText(QString::number(_counter));
//        _timer->start(1000);
//        emit clicked();
//    }
//    // 调用基类的mouseReleaseEvent以确保正常的事件处理（如点击效果）
//    QPushButton::mouseReleaseEvent(e);
//}
