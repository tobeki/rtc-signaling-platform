#include "CServer.h"
#include <iostream>
#include "AsioIOServicePool.h"
#include "UserMgr.h"
#include "LogicSystem.h"

CServer::CServer(boost::asio::io_context& io_context, short port) :_io_context(io_context), _port(port),
 _acceptor(io_context, tcp::endpoint(tcp::v4(), port)) {
	std::cout << "Server start success, listen on port : " << _port << std::endl;
	StartAccept();
}

CServer::~CServer() {
	std::cout << "Server destruct listen on port : " << _port << std::endl;
}

void CServer::StartAccept() {
	auto& io_context = AsioIOServicePool::GetInstance()->GetIOService();
	std::shared_ptr<CSession> new_session = std::make_shared<CSession>(io_context, this);
	_acceptor.async_accept(new_session->GetSocket(), std::bind(&CServer::HandleAccept, this, new_session, std::placeholders::_1));
}

void CServer::HandleAccept(std::shared_ptr<CSession> new_session, const boost::system::error_code& error) {
	if (!error) {
		// M5.1：Domain Session 的登记必须发生在 TCP accept **成功之后**。
		// CSession 对象是在 StartAccept() 里、async_accept 之前就创建好的；
		// 若在构造函数里登记，accept 失败时就会留下一个永远不会有断线事件的孤儿
		// Domain Session。这里只处理成功分支，失败分支没有任何投递点。
		const std::string session_id = new_session->GetSessionId();

		{
			std::lock_guard<std::mutex> lock(_mutex);
			_sessions.insert(make_pair(session_id, new_session));
		}

		// 投递刻意放在 _mutex 之外：队列锁与 CServer::_mutex 绝不嵌套，
		// 与 M5 的断线通知保持同一把锁纪律。
		//
		// 而且必须早于 Start()：session 的读回调跑在 AsioIOServicePool 的线程上，
		// 与当前 accept 线程是并发的；一旦先 Start()，该会话的第一条 legacy 消息
		// 就可能先于 REGISTER 进入 FIFO，CONNECTED 状态也就不再可信。
		if (!LogicSystem::GetInstance()->PostSessionRegisterWork(session_id)) {
			// 执行器已进入停止路径（shutdown）：只如实记录，不重试，
			// 也不改变 accept 本身的结果（连接依然被接受）。
			std::cout << "register work rejected because serialized executor is stopping, session: "
				<< session_id << std::endl;
		}

		new_session->Start();
	}
	else {
		std::cout << "session accept failed, error is " << error.what() << std::endl;
	}
	StartAccept();
}

//根据session 的id删除session，并移除用户和session的关联
void CServer::ClearSession(std::string session_id) {
	
	if (_sessions.find(session_id) != _sessions.end()) {
		auto uid = _sessions[session_id]->GetUserId();

		//移除用户和session的关联
		UserMgr::GetInstance()->RmvUserSession(uid);
	}

	{
		std::lock_guard<std::mutex> lock(_mutex);
		_sessions.erase(session_id);
	}
}
