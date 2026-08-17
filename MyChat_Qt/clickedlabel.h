#ifndef CLICKEDLABEL_H
#define CLICKEDLABEL_H
#include <QLabel>
#include "global.h"

class ClickedLabel : public QLabel
{
    // 因为用到了信号跟槽，所以需要Q_OBJECT（如果报错，先右键项目执行qmake）
    Q_OBJECT
public:
    ClickedLabel(QWidget *parent);
    virtual void mousePressEvent(QMouseEvent *event) override;
    virtual void mouseReleaseEvent(QMouseEvent *event) override;
    virtual void enterEvent(QEvent* event) override;
    virtual void leaveEvent(QEvent* event) override;
    void SetState(QString normal = "", QString hover = "", QString press = "",
                  QString select = "", QString select_hover = "", QString select_press = "");

    ClickLbState GetCurState();
    bool SetCurState(ClickLbState state);
    void ResetNormalState();
private:
    QString _normal;
    QString _normal_hover;
    QString _normal_press;

    QString _selected;
    QString _selected_hover;
    QString _selected_press;

    ClickLbState _curstate;
signals:
    void clicked(QString, ClickLbState);
};

#endif // CLICKEDLABEL_H
