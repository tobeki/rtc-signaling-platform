#pragma once

// ==============================================================================
// M4 LogicWorkItem：统一串行化 Work Item 的值类型（Phase 1 Step 1.6 §12）。
//
// 本文件**只定义值类型**，不包含队列、线程、锁或分发逻辑：
//   - 不 include CSession.h / LogicSystem.h / Redis / MySQL / gRPC；
//   - 不产生任何副作用；
//   - 可被单测直接构造（Layer B 顺序测试的前提）。
//
// 设计裁决（Step 1.6 §12.4「方案 A」）：
//   显式 enum kind + 各 kind 所需的稳定 ID/value 字段。
//
// 刻意**不**采用：
//   - polymorphic work item 基类（需要 shared_ptr 多态 + 虚函数，调试需 RTTI）；
//   - std::function<void()> opaque task queue（逻辑藏在 lambda capture 里，
//     无法断言"队列里到底排了什么"，也无法断言顺序）。
//
// 关键约束（Decision 4 / Step 1.6 §12.5）：
//   internal work item（SESSION_DISCONNECTED / FINALIZE_CLOSE）只保存**稳定 ID**，
//   绝不持有 CSession / shared_ptr<CSession> / CServer* / MeetingAggregate* / socket。
//   只有 EXTERNAL_MESSAGE 允许继续持有既有的 std::shared_ptr<LogicNode>，
//   以保持旧 IM 消息的生命周期语义不变。
// ==============================================================================

#include "meeting/MeetingTypes.h"

#include <memory>
#include <string>
#include <vector>

class LogicNode; // 定义在 CSession.h；此处只需不完整类型，避免把 asio/boost 拖进本头文件

// ------------------------------------------------------------------------------
// Work kind。
//
// 四种正式 kind 对应 Step 1.6 §12.2 规划的 EXTERNAL_MESSAGE / SESSION_DISCONNECTED /
// FINALIZE_CLOSE，外加 MEETING_COMMAND：
//
//   MEETING_COMMAND 不是新的网络协议类型，也不是 TCP message。
//   它只是把 M3 已经存在的 transport-neutral Meeting API 表达为一个**真实的
//   typed work item**，使得在没有 TCP Meeting message ID 的 M4 阶段，
//   Meeting 命令能够与 legacy 消息、断线、finalize 共享同一个 FIFO，
//   并且这个 FIFO 的顺序可以被确定性测试。
//
// INTERNAL_SESSION_REGISTER / INTERNAL_SESSION_AUTHENTICATE 是**最小的**
// 领域 Session 生命周期登记入口：Layer B 测试必须在同一个 single-writer 执行
// 路径上准备有效的 AUTHENTICATED Session，而不能在 worker 运行时从测试线程
// 直接改 MeetingService。它们不代表 M4 已接入真实 CSession 的认证路径。
// ------------------------------------------------------------------------------
enum class LogicWorkKind {
    EXTERNAL_MESSAGE = 0,               // legacy IM 消息（可持有 LogicNode）
    MEETING_COMMAND = 1,                // transport-neutral Meeting 命令
    SESSION_DISCONNECTED = 2,           // 网络断线内部工作（只存 SessionId）
    FINALIZE_CLOSE = 3,                 // 第二个 serialized turn（只存 MeetingId）
    INTERNAL_SESSION_REGISTER = 4,      // domain Session → CONNECTED
    INTERNAL_SESSION_AUTHENTICATE = 5   // domain Session → AUTHENTICATED
};

inline const char* LogicWorkKindName(LogicWorkKind kind) {
    switch (kind) {
    case LogicWorkKind::EXTERNAL_MESSAGE:            return "EXTERNAL_MESSAGE";
    case LogicWorkKind::MEETING_COMMAND:             return "MEETING_COMMAND";
    case LogicWorkKind::SESSION_DISCONNECTED:        return "SESSION_DISCONNECTED";
    case LogicWorkKind::FINALIZE_CLOSE:              return "FINALIZE_CLOSE";
    case LogicWorkKind::INTERNAL_SESSION_REGISTER:   return "INTERNAL_SESSION_REGISTER";
    case LogicWorkKind::INTERNAL_SESSION_AUTHENTICATE:return "INTERNAL_SESSION_AUTHENTICATE";
    }
    return "UNKNOWN_WORK_KIND";
}

// ------------------------------------------------------------------------------
// Meeting 命令种类（领域 API 层，不是 wire encoding）。
//
// 与 Phase 1 冻结的六项 API 一一对应；不承诺任何数值编码，也不得被当作
// TCP/gRPC message ID 使用（协议扩展属于 Phase 3）。
// ------------------------------------------------------------------------------
enum class ClientMeetingCommandKind {
    CREATE = 0,
    JOIN = 1,
    LEAVE = 2,
    CLOSE = 3,
    QUERY = 4,
    LIST_PARTICIPANTS = 5
};

inline const char* ClientMeetingCommandKindName(ClientMeetingCommandKind kind) {
    switch (kind) {
    case ClientMeetingCommandKind::CREATE:            return "CREATE";
    case ClientMeetingCommandKind::JOIN:              return "JOIN";
    case ClientMeetingCommandKind::LEAVE:             return "LEAVE";
    case ClientMeetingCommandKind::CLOSE:             return "CLOSE";
    case ClientMeetingCommandKind::QUERY:             return "QUERY";
    case ClientMeetingCommandKind::LIST_PARTICIPANTS: return "LIST_PARTICIPANTS";
    }
    return "UNKNOWN_MEETING_COMMAND";
}

// 该命令是否需要 meeting_id：CREATE 由服务端生成 ID，其余五项必须携带。
inline bool ClientMeetingCommandRequiresMeetingId(ClientMeetingCommandKind kind) {
    return kind != ClientMeetingCommandKind::CREATE;
}

// ------------------------------------------------------------------------------
// 统一 Work Item。
//
// 使用"一个 struct + 若干未使用字段"换取：显式 kind 可见、可断言、C++14 可实现、
// 不需要 shared_ptr 多态。冗余字段是方案 A 已知且被接受的代价。
//
// 字段有效性（只由 kind 决定，不存在隐式约定）：
//   EXTERNAL_MESSAGE              : external_logic_node
//   MEETING_COMMAND               : meeting_context, meeting_command, has_meeting_id, meeting_id
//   SESSION_DISCONNECTED          : session_id
//   FINALIZE_CLOSE                : meeting_id
//   INTERNAL_SESSION_REGISTER     : session_id
//   INTERNAL_SESSION_AUTHENTICATE : session_id, user_id
// ------------------------------------------------------------------------------
struct LogicWorkItem {
    LogicWorkKind kind = LogicWorkKind::EXTERNAL_MESSAGE;

    // --- EXTERNAL_MESSAGE：legacy 路径专用 ------------------------------------
    // 保持既有 LogicNode 语义（含它持有的 shared_ptr<CSession>）。
    std::shared_ptr<LogicNode> external_logic_node;

    // --- MEETING_COMMAND ------------------------------------------------------
    // context 必须由服务端 transport adapter 构造，客户端 payload 不得提供身份字段。
    meeting::MeetingRequestContext meeting_context;
    ClientMeetingCommandKind meeting_command = ClientMeetingCommandKind::CREATE;
    bool has_meeting_id = false;
    meeting::MeetingId meeting_id;

    // --- SESSION_DISCONNECTED / INTERNAL_SESSION_* ----------------------------
    meeting::SessionId session_id;

    // --- INTERNAL_SESSION_AUTHENTICATE ---------------------------------------
    meeting::UserId user_id = 0;

    // --------------------------------------------------------------------------
    // 便于调试与测试的只读描述：直接输出队列里真实排了什么。
    // --------------------------------------------------------------------------
    std::string Describe() const {
        std::string text = LogicWorkKindName(kind);
        if (kind == LogicWorkKind::MEETING_COMMAND) {
            text += "(";
            text += ClientMeetingCommandKindName(meeting_command);
            if (has_meeting_id) {
                text += " meeting_id=";
                text += meeting_id;
            }
            text += ")";
        } else if (kind == LogicWorkKind::FINALIZE_CLOSE) {
            text += "(meeting_id=" + meeting_id + ")";
        } else if (kind == LogicWorkKind::SESSION_DISCONNECTED ||
                   kind == LogicWorkKind::INTERNAL_SESSION_REGISTER) {
            text += "(session_id=" + session_id + ")";
        } else if (kind == LogicWorkKind::INTERNAL_SESSION_AUTHENTICATE) {
            text += "(session_id=" + session_id + " user_id=" + std::to_string(user_id) + ")";
        }
        return text;
    }
};

// ------------------------------------------------------------------------------
// 构造 helper：避免调用方手写字段组合，也避免"忘记设置 kind"这类错误。
// ------------------------------------------------------------------------------
inline LogicWorkItem MakeExternalMessageWork(std::shared_ptr<LogicNode> node) {
    LogicWorkItem item;
    item.kind = LogicWorkKind::EXTERNAL_MESSAGE;
    item.external_logic_node = node;
    return item;
}

inline LogicWorkItem MakeMeetingCommandWork(const meeting::MeetingRequestContext& context,
                                            ClientMeetingCommandKind command,
                                            const meeting::MeetingId& meeting_id) {
    LogicWorkItem item;
    item.kind = LogicWorkKind::MEETING_COMMAND;
    item.meeting_context = context;
    item.meeting_command = command;
    item.has_meeting_id = ClientMeetingCommandRequiresMeetingId(command);
    item.meeting_id = meeting_id;
    return item;
}

inline LogicWorkItem MakeSessionDisconnectedWork(const meeting::SessionId& session_id) {
    LogicWorkItem item;
    item.kind = LogicWorkKind::SESSION_DISCONNECTED;
    item.session_id = session_id;
    return item;
}

inline LogicWorkItem MakeFinalizeCloseWork(const meeting::MeetingId& meeting_id) {
    LogicWorkItem item;
    item.kind = LogicWorkKind::FINALIZE_CLOSE;
    item.meeting_id = meeting_id;
    return item;
}

inline LogicWorkItem MakeSessionRegisterWork(const meeting::SessionId& session_id) {
    LogicWorkItem item;
    item.kind = LogicWorkKind::INTERNAL_SESSION_REGISTER;
    item.session_id = session_id;
    return item;
}

inline LogicWorkItem MakeSessionAuthenticateWork(const meeting::SessionId& session_id,
                                                 meeting::UserId user_id) {
    LogicWorkItem item;
    item.kind = LogicWorkKind::INTERNAL_SESSION_AUTHENTICATE;
    item.session_id = session_id;
    item.user_id = user_id;
    return item;
}

// ------------------------------------------------------------------------------
// 分发观测记录：用于"断言真实执行顺序"，而不是靠日志或 sleep 目测。
//
// 生产路径不强制使用；存在 value 观测点即可，不引入网络或响应编码。
// ------------------------------------------------------------------------------
struct WorkDomainEvent {
    meeting::EventType type = meeting::EventType::MEETING_CREATED;
    meeting::MeetingId meeting_id;
    meeting::EventId event_id;
    meeting::MeetingState meeting_state_after = meeting::MeetingState::CREATED;
};

// 一次 MEETING_COMMAND 分发的领域结果摘要（只读 value，不是 Response 编码）。
struct MeetingCommandOutcome {
    ClientMeetingCommandKind command = ClientMeetingCommandKind::CREATE;
    meeting::ResultCode result_code = meeting::ResultCode::INTERNAL_ERROR;
    std::string result_name;
    meeting::ResultDisposition disposition = meeting::ResultDisposition::ERROR;
    bool should_schedule_finalize = false;
    bool has_meeting_id = false;
    meeting::MeetingId meeting_id;

    // 响应侧的最小状态观测：用于断言"首次 Close 的响应是 ENDING"，
    // 而不是靠日志目测。它只承载 MeetingView 已有的值，不新增领域语义。
    meeting::MeetingState meeting_state = meeting::MeetingState::CREATED;
    bool has_closed_at = false;

    // M5.1：MeetingView.active_participant_count 的只读投影。
    //
    // 存在的理由是"生命周期接线必须可证"：MEETING_COMMAND 的响应里本来就带着参与者
    // 计数，但观测结构此前没有暴露它，于是"最后一个 Session 断线导致 participant_count - 1"
    // 这类断言只能靠事件次数间接推断。这里只做**值拷贝**，不新增也不改变任何领域规则。
    std::size_t active_participant_count = 0;

    std::vector<meeting::EventType> event_types;
};

// 分发观测 sink。
//
// 这是**纯观测**：实现方不得在其中修改领域状态、发送响应或触发通知。
// 生产 LogicSystem 不注入任何 sink（所有回调为空实现），因此不产生额外开销，
// 也不会出现"测试专用执行器"与生产执行器不一致的问题。
class IWorkObservationSink {
public:
    virtual ~IWorkObservationSink() {}

    // 按真实 **dequeue/dispatch** 次序回调：sequence 从 1 单调递增。
    // 这是"顺序由真实 enqueue 次序决定，而不是由 kind 优先级决定"的直接证据。
    virtual void RecordWorkDispatched(const LogicWorkItem& item,
                                      unsigned long long sequence,
                                      const std::vector<WorkDomainEvent>& events) = 0;

    // legacy EXTERNAL_MESSAGE 是否被路由到 legacy 分发器，以及路由到的 msg_id。
    virtual void RecordLegacyMessageRouted(const meeting::SessionId& session_id,
                                           short msg_id,
                                           std::size_t payload_size) = 0;

    // SessionDisconnected fan-out 结果（0 / 1 / N）。
    virtual void RecordSessionDisconnected(const meeting::SessionId& session_id,
                                          std::size_t affected_meeting_count) = 0;

    // FinalizeClose 结果；notification_session_ids 只是**意图**，M4 不发送任何东西。
    virtual void RecordFinalizeClose(const meeting::MeetingId& meeting_id,
                                     bool finalized,
                                     bool already_finalized,
                                     const std::vector<meeting::SessionId>& notification_session_ids) = 0;

    // 六项 Meeting 命令的结果摘要。
    virtual void RecordMeetingCommand(const MeetingCommandOutcome& outcome) = 0;

    // 内部 work（FINALIZE_CLOSE）被追加到同一 FIFO 的那一刻，以及**那一刻的队列内容**。
    //
    // 这是"顺序由真实 enqueue 次序决定"的确定性证据：在一个可能被多个生产者并发
    // 投递的 FIFO 上，"新追加的 item 排在第几"只在该次入队的临界区里是确定的，
    // 事后再看快照只能得到某一次可能的交错。因此在入队点记录才是可断言的。
    virtual void RecordInternalWorkEnqueued(const LogicWorkItem& item,
                                            const std::vector<LogicWorkItem>& queue_after) = 0;
};
