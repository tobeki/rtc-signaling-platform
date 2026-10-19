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

// M6.6：装配所需的三个类型只做前向声明。
//   - IMeetingSessionDirectory：CServer 实现的判活目录接口（M6.3），
//     LogicSystem 只保存它的弱引用；
//   - CountingNoWireCollector / MeetingResponseNotificationAdapter：M6.4 的生产实现，
//     由 LogicSystem 以 unique_ptr **独占拥有**。
// 完整类型只在本类的**析构函数定义**所在编译单元（LogicSystem.cpp）可见，
// 因此这里既不必把 adapter/collector 的头文件传染给所有 LogicSystem 使用者，
// 也满足 "unique_ptr 管理不完整类型" 的析构要求。
class IMeetingSessionDirectory;
class CountingNoWireCollector;
class MeetingResponseNotificationAdapter;

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

	// -------------------------------------------------------------------------
	// M6.6：三个正式生命周期入口（§12.1 / §12.3 / §12.5）。
	// -------------------------------------------------------------------------

	// 注册 Session Directory 的**判活**弱引用（不拥有、不新增第二份 registry）。
	//
	// 契约：
	//   - noexcept：内部只做 weak_ptr 赋值与一个 bool 写入，没有分配、没有抛点；
	//   - **一次性**语义：重复注册不会替换已注册的目录（后续调用直接返回）；
	//   - 必须在唯一 worker 启动前调用（M6.6 的 main 在 io_context.run() 之前调用）。
	//     否则 lazily 构造的 adapter 会持有一个空的弱引用 ⇒ 出站永远 no-op（R-3）。
	//   - 不引入线程、队列或 timer。
	void SetMeetingSessionDirectory(
		std::shared_ptr<IMeetingSessionDirectory> directory) noexcept;

	// 停止并排空唯一 executor（K 级步骤：失败必须上抛给关闭协调器，不得吞掉）。
	//   - executor 尚未构造 ⇒ 安全返回（幂等 no-op）；
	//   - 已构造 ⇒ 调用现有 StopAndJoin()（先 RequestStop，再排空，锁外 join）；
	//   - 不创建第二个执行器，不绕过 FIFO 排空，不持队列锁 join。
	void ShutdownWorkExecutor();

	// 清除 Session Directory 的弱引用（B 级步骤）。
	//   - noexcept：只 reset 本对象保存的 weak_ptr；
	//   - 必须在 ShutdownWorkExecutor() **成功之后**调用：drain 期间的
	//     FINALIZE_CLOSE 仍要 lookup；
	//   - **不**会、也无法强制修改 adapter 内部已保存的那份弱引用（它是构造时拷贝的），
	//     此处不做任何"跨对象失效"的承诺；worker 已 join ⇒ 已无消费者在使用它。
	void ClearMeetingSessionDirectory() noexcept;

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

	// =====================================================================
	// M6.6（§12.1 / §12.2.3）：生产出站四件套的所有权与声明顺序。
	//
	// 依赖事实（决定声明顺序，而不是偏好决定）：
	//   collector ← adapter ← dispatcher ← executor 的 worker 线程
	//   即 worker 运行期间，adapter/dispatcher 必须存活；adapter 运行期间，
	//   collector 必须存活。
	//
	// 声明顺序（**必须**如此）：
	//   _outbound_collector → _meeting_adapter → _dispatcher → _work_executor
	// 逆序析构：
	//   _work_executor（worker 先被 join）→ _dispatcher → _meeting_adapter
	//   → _outbound_collector
	// ⇒ "collector ⊇ adapter ⊇ dispatcher ⊇ worker" 的存活条件成立。
	//
	// ⚠ 声明顺序只是**防御性**保证，**不能**代替显式的 ShutdownWorkExecutor()：
	//   只依赖逆序析构会让 worker 一直跑到 ~LogicSystem 开始析构，而那时
	//   CServer / Redis / pool 早已由关闭协调器停掉。显式停止不可省略。
	// =====================================================================

	// 生产出站接受者：只计数、绝不发送任何字节（§7.3 / §7.5）。
	// 必须是**成员**（不是 EnsureWorkQueueStarted 内的局部对象），
	// 否则 call_once 结束时它就析构，adapter 将持有悬垂引用（R-13）。
	std::unique_ptr<CountingNoWireCollector> _outbound_collector;

	// 真实 Response/Notification adapter：非拥有地引用上面的 collector，
	// 并以弱引用判活 Session Directory。
	std::unique_ptr<MeetingResponseNotificationAdapter> _meeting_adapter;

	// 会议执行上下文。由 LogicSystem 执行上下文拥有，仅由单个消费者线程修改，
	// 这也正是使 MeetingService 的“非线程安全，无内部锁”契约安全的原因。
	// M6.6：构造时注入**真实** adapter 指针（第四参数），不再是 nullptr。
	std::unique_ptr<LogicWorkDispatcher> _dispatcher;

	// 唯一的权威 FIFO 以及它的单个消费者。
	// M4 用这个执行器取代了旧的三元组（_msg_que / _mutex / _consume / _worker_thread）。
	// 仍然只有一个队列和一个工作线程：没有第二个业务队列，没有第二个业务工作线程，
	// 没有异步任务执行器。
	std::unique_ptr<SerializedWorkExecutor<LogicWorkDispatcher>> _work_executor;

	// M6.6（§8.4 / §12.5）：Session Directory 的判活弱引用。
	//   - 不拥有 CServer（main 的 shared_ptr 才是所有者）；
	//   - 一次性注册；写入发生在唯一 worker 启动之前（main 在 run() 之前调用）；
	//   - adapter 在构造时拷贝这份弱引用，因此注册必须先于 EnsureWorkQueueStarted()。
	std::weak_ptr<IMeetingSessionDirectory> _meeting_session_directory;

	// 一次性注册标志：重复调用 SetMeetingSessionDirectory() 不会替换已注册目录。
	// 只有 main 线程在 worker 启动前写它，因此用普通 bool（无并发写者）。
	bool _meeting_session_directory_registered = false;

	// 在构建 MeetingRequestContext 时使用的本地服务器 ID。
	std::string _local_server_id;

	 // 保护一次性构建 _dispatcher _work_executor。
    // 延迟（不在构造函数中构建），以确保构造顺序永远不依赖于单例构造顺序。
    // M6.6：四件套（collector / adapter / dispatcher / executor）在**同一个**
    // call_once 内按依赖顺序构造，因此"工作线程启动前依赖已就绪"由结构保证。
	std::once_flag _work_start_once;

	std::map<short, FunCallBack> _fun_callbacks;
};

