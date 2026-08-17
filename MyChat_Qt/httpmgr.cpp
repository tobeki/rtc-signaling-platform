#include "httpmgr.h"
#include <QNetworkReply>

HttpMgr::~HttpMgr()
{

}

HttpMgr::HttpMgr()
{
    //连接http请求和完成信号，信号槽机制保证队列消费
    connect(this, &HttpMgr::sig_http_finish, this, &HttpMgr::slot_http_finish);
}

void HttpMgr::PostHttpReq(QUrl url, QJsonObject json, ReqId req_id, Modules mod)
{
    //创建一个HTTP POST请求，并设置请求头和请求体
    // 将json序列化
    QByteArray data = QJsonDocument(json).toJson();
    //通过url构造请求
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    // QByteArray获取长度时会自动进行大小端序转换
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray::number(data.length()));

    //发送请求，并处理响应
    auto self = shared_from_this();
    QNetworkReply* reply = _manager.post(request, data);
    //设置信号和槽等待发送完成,等待reply发送finished信号，然后触发回调函数
    QObject::connect(reply, &QNetworkReply::finished, [reply, self, req_id, mod](){
        //处理错误的情况
        if (reply->error() != QNetworkReply::NoError){
            qDebug() << reply->errorString();

            emit self->sig_http_finish(req_id, "", ErrorCodes::ERR_NETWORK, mod);
            reply->deleteLater();
            return;
        }

        //无错误则读回请求(QByteArray可转为QString)
        QString res = reply->readAll();
        //发送信号通知完成
        emit self->sig_http_finish(req_id, res, ErrorCodes::SUCCESS, mod);
        reply->deleteLater();
        return;
    });
}

void HttpMgr::slot_http_finish(ReqId id, QString res, ErrorCodes err, Modules mod)
{
    if (mod == Modules::REGISTERMOD){
        //发送信号通知指定模块http响应结束
        emit sig_reg_mod_finish(id, res, err);
    }

    if (mod == Modules::RESETMOD){
        //发送信号通知指定模块http响应结束
        emit sig_reset_mod_finish(id, res, err);
    }

    if(mod == Modules::LOGINMOD){
        emit sig_login_mod_finish(id, res, err);
    }
}
