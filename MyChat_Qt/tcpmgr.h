#ifndef TCPMGR_H
#define TCPMGR_H
#include "global.h"
#include "singleton.h"
#include <QTcpSocket>
#include <QObject>
#include "userdata.h"

class TcpMgr: public QObject, public Singleton<TcpMgr>,
        public std::enable_shared_from_this<TcpMgr>
{
    Q_OBJECT
public:
    ~TcpMgr();
private:
    friend class Singleton<TcpMgr>;
    TcpMgr();
    void initHandlers();
    void handleMsg(ReqId id, int len, QByteArray data);

    QTcpSocket _socket;
    QString _host;
    uint16_t _port;
    QByteArray _buffer;
    bool _b_recv_pending;
    quint16 _message_id;
    quint16 _message_len;
    QMap<ReqId, std::function<void(ReqId id, int len, QByteArray data)>> _handlers;
public slots:
    void slot_tcp_connect(ServerInfo);
    void slot_send_data(ReqId reqId, QByteArray dataBytes);
signals:
    void sig_con_success(bool b_success);
    void sig_send_data(ReqId reqId, QByteArray data);
    void sig_connection_closed();
    void sig_login_failed(int);
    void sig_swich_chatdlg();
    void sig_user_search(std::shared_ptr<SearchInfo>);
    void sig_friend_apply(std::shared_ptr<AddFriendApply>);		// 收到对方的好友申请
    void sig_add_auth_friend(std::shared_ptr<AuthInfo>);		// 收到对方同意的认证后来自对端的回复
    void sig_auth_rsp(std::shared_ptr<AuthRsp>);                // 同意对方申请后服务器给本端的回复
    void sig_text_chat_msg(std::shared_ptr<TextChatMsg> msg);
};

#endif // TCPMGR_H
