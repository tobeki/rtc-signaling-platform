#ifndef TIMERBTN_H
#define TIMERBTN_H
#include <QPushButton>
#include <QTimer>

class TimerBtn:public QPushButton
{
public:
    TimerBtn(QWidget *parent = nullptr);
    ~TimerBtn();

    void startCountdown(int seconds = 30);
    void stopCountdown();
    // 重写mouseReleaseEvent (在基类中)
//    virtual void mouseReleaseEvent(QMouseEvent *e) override;
private:
    QTimer* _timer;
    int _counter;
    bool _isCounting;
};

#endif // TIMERBTN_H
