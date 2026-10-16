#pragma once
#include "Singleton.h"
#include "LogicWorkDispatcher.h"
#include "SessionDisconnectBridge.h"
#include <memory>
#include <map>
#include <mutex>
#include <thread>
#include <string>
#include "CSession.h"
#include "const.h"
#include <json/json.h>
#include <json/value.h>
#include <json/reader.h>
#include <unordered_map>
#include <string>
#include "data.h"

class CServer;
typedef  std::function<void(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data)> FunCallBack;

// M5：LogicSystem 同时是断线事件的投递目标。
// 网络层（CSession）只通过 ISessionDisconnectSink 这一抽象把稳定 SessionId 交进来，
// 由 LogicSystem 负责把它变成 SESSION_DISCONNECTED work item 并投入唯一 FIFO。
// 因此"网络线程不得直接改 Meeting"是结构约束，而不是调用方纪律。
class LogicSystem :public Singleton<LogicSystem>, public ILegacyMessageRouter, public ISessionDisconnectSink
{
	friend class Singleton<LogicSystem>;
public:
	~LogicSystem();

	// 遗留外部条目：签名未更改，调用者未更改（CSession）。
    // 消息被封装为 EXTERNAL_MESSAGE 工作项并发布到单一权威的 FIFO。
	void PostMsgToQue(std::shared_ptr < LogicNode> msg);

	// 	ILegacyMessageRouter：生产环境的传统分发路径。  
	// 逐字保持原始的 msg_id -> FunCallBack 查找语义  
	// （缺失的 id 会被忽略，回调接收相同的三个参数），但现在由队列消费者在队列互斥之外调用。
	void RouteExternalMessage(const std::shared_ptr<LogicNode>& msg);

	// 已键入的内部入队条目（单个 FIFO，与传统消息和会议命令具有相同的排序域）。
	// M5：本函数同时是 ISessionDisconnectSink 的唯一实现，也就是断线事件进入
	// 唯一 FIFO 的唯一入口。参数只允许是稳定 SessionId 的值拷贝：
	// 不持有 CSession / socket / 连接对象，也**不**直接调用 MeetingService。
	// 返回 false 表示队列已在停止路径上（M4 §11.2：stop 之后的外部 work 一律丢弃）。
	bool PostSessionDisconnectedWork(const meeting::SessionId& session_id) override;

	// M5.1：真实 CSession 生命周期进入唯一 FIFO 的两个 typed 入口。
	//
	// 二者与 PostSessionDisconnectedWork 完全同构：只把稳定值（SessionId / UserId）交给
	// 唯一的串行化执行器，绝不直接触碰 MeetingService，也不新建第二条队列或第二个 worker。
	//   - Register    ：TCP accept 成功之后调用（CServer::HandleAccept 的成功分支）；
	//   - Authenticate：legacy 登录真正成功之后调用（LogicSystem::LoginHandler 尾部）。
	// 返回 false 表示队列已停止接收（shutdown 路径），调用方只记录、不重试。
	bool PostSessionRegisterWork(const meeting::SessionId& session_id);
	bool PostSessionAuthenticateWork(const meeting::SessionId& session_id,
		meeting::UserId user_id);

	bool PostMeetingCommandWork(const meeting::MeetingRequestContext& context,
		ClientMeetingCommandKind command, const meeting::MeetingId& meeting_id);

	// 针对单写入者/单消费者不变量的只读可观察性。
	const std::string& LocalServerId() const { return _local_server_id; }

	// 只读面向会议的诊断。仅有意基于值：没有访问器可以提供可变的 MeetingService、
	// 注册表或队列，否则单写边界可能会被网络线程绕过。会议的修改只能通过发布工作项来实现。
	bool HasActiveMeeting(const meeting::MeetingId& meeting_id) const;
	bool HasClosedSnapshot(const meeting::MeetingId& meeting_id) const;
private:
	LogicSystem();
	void RegisterCallBacks();
	void EnsureWorkQueueStarted();
	void LoginHandler(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	void SearchInfo(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	void AddFriendApply(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	void AuthFriendApply(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	void DealChatTextMsg(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);

	bool isPureDigit(const std::string& str);
	void GetUserByUid(std::string uid_str, Json::Value& rtvalue);
	void GetUserByName(std::string name, Json::Value& rtvalue);
	bool GetBaseInfo(std::string base_key, int uid, std::shared_ptr<UserInfo>& userinfo);
	bool GetFriendApplyInfo(int to_uid, std::vector<std::shared_ptr<ApplyInfo>>& list);
	bool GetFriendList(int self_id, std::vector<std::shared_ptr<UserInfo>>& user_list);

	// 唯一的权威 FIFO 以及它的单个消费者。
	// M4 用这个执行器取代了旧的三元组（_msg_que / _mutex / _consume / _worker_thread）。
	// 仍然只有一个队列和一个工作线程：没有第二个业务队列，没有第二个业务工作线程，
	// 没有异步任务执行器。
	std::unique_ptr<SerializedWorkExecutor<LogicWorkDispatcher>> _work_executor;

	// 会议执行上下文。由 LogicSystem 执行上下文拥有，仅由单个消费者线程修改，
	// 这也正是使 MeetingService 的“非线程安全，无内部锁”契约安全的原因。
	std::unique_ptr<LogicWorkDispatcher> _dispatcher;

	// 在构建 MeetingRequestContext 时使用的本地服务器 ID。
	std::string _local_server_id;

	 // 保护一次性构建 _dispatcher _work_executor。
    // 延迟（不在构造函数中构建），以确保构造顺序永远不依赖于单例构造顺序。
	std::once_flag _work_start_once;

	std::map<short, FunCallBack> _fun_callbacks;
};

