#pragma once
#include <boost/asio.hpp>
#include "CSession.h"
#include <memory.h>
#include <map>
#include <mutex>
#include <string>
// M6.3：IMeetingSessionDirectory（判活目录接口）。它只含 <memory>/<string>，
// 因此让 CServer 实现它不给本类引入任何新的运行时依赖。
#include "MeetingOutboundChannel.h"

using boost::asio::ip::tcp;

// M6.3（§8.2 / §8.5）：CServer 是 _sessions 这个**唯一真实 Session 注册表**的
// 拥有者，同时也是 IMeetingSessionDirectory 的实现者。
//
// 既有继承 std::enable_shared_from_this<CServer> 保持不变：本轮**不**调用
// shared_from_this()，把 ChatServer.cpp 里的 CServer 改成 make_shared 属于 M6.6。
class CServer :public std::enable_shared_from_this<CServer>,
	public IMeetingSessionDirectory
{
public:
	CServer(boost::asio::io_context& io_context, short port);
	~CServer();
	void ClearSession(std::string);

	// M6.3（§8.2.2 / §8.3）：从**既有** _sessions 注册表复制一份共享所有权句柄。
	//
	// 契约：
	//   - 在 _mutex 下 find，并在同一临界区内**复制** shared_ptr ⇒ 引用计数 +1；
	//   - 命中 ⇒ 非空 shared_ptr<const IMeetingLiveSession>，句柄存活期间目标必然存活；
	//   - 未命中 ⇒ 空 shared_ptr（不抛、不插入新条目）；
	//   - 绝不用裸指针 / .get() 构造 shared_ptr（会产生第二个控制块 ⇒ 未定义行为）；
	//   - 不新增第二份 registry、不缓存 CSession 对象、不返回裸指针。
	std::shared_ptr<const IMeetingLiveSession>
		FindSessionBySessionId(const std::string& session_id) override;

private:
	void HandleAccept(std::shared_ptr<CSession>, const boost::system::error_code& error);
	void StartAccept();
	boost::asio::io_context& _io_context;
	short _port;
	tcp::acceptor _acceptor;
	std::map<std::string, std::shared_ptr<CSession>> _sessions;
	std::mutex _mutex;
};

