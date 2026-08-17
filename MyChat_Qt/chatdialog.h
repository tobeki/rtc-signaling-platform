#ifndef CHATDIALOG_H
#define CHATDIALOG_H
#include <QDialog>
#include "global.h"
#include "statewidget.h"
#include "userdata.h"
#include <QListWidgetItem>

namespace Ui {
class ChatDialog;
}

class ChatDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ChatDialog(QWidget *parent = nullptr);
    ~ChatDialog();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

    void handleGlobalMousePress(QMouseEvent *event);

private:
    void ShowSearch(bool b_search);
    void addChatUserList();
    void AddLBGroup(StateWidget *lb);
    void ClearLabelState(StateWidget* lb);
    void loadMoreChatUser();
    void loadMoreConUser();
    void SetSelectChatItem(int uid = 0);
    void SetSelectChatPage(int uid = 0);
    void UpdateChatMsg(std::vector<std::shared_ptr<TextChatData> > msgdata);

    Ui::ChatDialog *ui;
    ChatUIMode _mode;
    ChatUIMode _state;
    bool _b_loading;
    QList<StateWidget*> _lb_list;
    QWidget* _last_widget;
    QMap<int, QListWidgetItem*> _chat_items_added;
    int _cur_chat_uid;
private slots:
    void slot_loading_chat_user();
    void slot_loading_contact_user();
    void slot_side_chat();
    void slot_side_contact();
    void slot_text_changed(const QString &str);
public slots:
    void slot_apply_friend(std::shared_ptr<AddFriendApply> apply);
    void slot_auth_rsp(std::shared_ptr<AuthRsp> auth_rsp);
    void slot_add_auth_friend(std::shared_ptr<AuthInfo> auth_info);
    void slot_jump_chat_item(std::shared_ptr<SearchInfo> si);
    void slot_jump_chat_item_from_infopage(std::shared_ptr<UserInfo> user_info);
    void slot_friend_info_page(std::shared_ptr<UserInfo> user_info);
    void slot_append_send_chat_msg(std::shared_ptr<TextChatData> msgdata);
    void slot_text_chat_msg(std::shared_ptr<TextChatMsg> msg);
};

#endif // CHATDIALOG_H
