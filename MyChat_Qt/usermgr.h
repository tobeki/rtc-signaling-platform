#ifndef USERMGR_H
#define USERMGR_H
#include <QObject>
#include <memory>
#include <singleton.h>
#include "userdata.h"
#include <vector>

class UserMgr:public QObject, public Singleton<UserMgr>,
        public std::enable_shared_from_this<UserMgr>
{
    Q_OBJECT
public:
    friend class Singleton<UserMgr>;
    ~UserMgr();
    void SetName(QString name);
    void SetUid(int uid);
    void SetToken(QString token);
    void SetUserInfo(std::shared_ptr<UserInfo> user_info);

    QString GetName();
    int GetUid();
    QString GetNick();
    QString GetIcon();
    QString GetDesc();
    std::vector<std::shared_ptr<ApplyInfo>> GetApplyList();
    std::shared_ptr<UserInfo> GetUserInfo();
    std::shared_ptr<FriendInfo> GetFriendById(int uid);

    bool AlreadyApply(int uid);
    void AddApplyList(std::shared_ptr<ApplyInfo> apply);
    void AppendApplyList(QJsonArray array);
    void AppendFriendList(QJsonArray array);
    void AddFriend(std::shared_ptr<AuthRsp> auth_rsp);
    void AddFriend(std::shared_ptr<AuthInfo> auth_info);
    bool CheckFriendById(int uid);
    std::vector<std::shared_ptr<FriendInfo>> GetChatListPerPage();
    bool IsLoadChatFinish();
    void UpdateChatLoadedCount();
    std::vector<std::shared_ptr<FriendInfo>> GetConListPerPage();
    void UpdateContactLoadedCount();
    bool IsLoadConFinish();
    void AppendFriendChatMsg(int friend_id,std::vector<std::shared_ptr<TextChatData>>);
private:
    UserMgr();
    std::shared_ptr<UserInfo> _user_info;
    QString _token;
    std::vector<std::shared_ptr<ApplyInfo>> _apply_list;
    QMap<int, std::shared_ptr<FriendInfo>> _friend_map;
    std::vector<std::shared_ptr<FriendInfo>> _friend_list;
    int _chat_loaded;
    int _contact_loaded;
};

#endif // USERMGR_H
