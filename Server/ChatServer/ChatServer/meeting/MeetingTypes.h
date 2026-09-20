#pragma once

// ==============================================================================
// Meeting 领域公共值类型（Phase 2 M1）。
//
// 本文件只定义"值类型与契约类型"：ID 别名、四个状态机的枚举、结果契约、
// 请求上下文、对外视图、领域事件与 CLOSED 终态快照。
//
// 明确不包含：任何业务 mutation（Join/Leave/Close/FinalizeClose/
// SessionDisconnected/GenerateMeetingId/PublishEvent 属于 M2/M3）、registry、
// service、持久化、传输适配。
//
// 依赖边界：仅依赖 C++ 标准库。不得 include CSession/CServer/LogicSystem/
// RedisMgr/MysqlMgr/MysqlDao/ChatGrpcClient，也不得引入 gRPC、protobuf、
// hiredis、jsoncpp 或 Boost。
// ==============================================================================

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace meeting {

// ------------------------------------------------------------------------------
// 标识类型。
//
// 以下只是 C++ 内部表示，不是 Protocol V2 的 wire format 承诺：未来协议可以选择
// 完全不同的编码方式（例如二进制或固定长度），本文件不对此作出约束。
// ------------------------------------------------------------------------------
using UserId = int;

using RequestId = std::string;
using SessionId = std::string;
using MeetingId = std::string;
using ServerId = std::string;
using DeviceId = std::string;
using EventId = std::string;

// 领域时间点的内部表示。M1 不提供格式化、解析、时区换算或序列化能力。
using Timestamp = std::chrono::system_clock::time_point;

// ------------------------------------------------------------------------------
// 领域 Session 状态（Step 1.3 §5.1）。
//
// 注意：这里刻意没有 JOINED。"是否在会议中"由 Participant/Binding 表达，
// 不是 Session 状态的一部分。
// ------------------------------------------------------------------------------
enum class DomainSessionState {
    CONNECTED = 0,     // TCP 已建立，尚未完成认证
    AUTHENTICATED = 1, // 已通过认证且 user_id 已绑定
    CLOSING = 2,       // 终止流程已启动，不再接受新的业务命令
    CLOSED = 3         // 领域与注册表清理完成，终态
};

// ------------------------------------------------------------------------------
// Meeting 状态（Step 1.3 §7.1）。冻结取值只有这四个，不得扩展。
// ------------------------------------------------------------------------------
enum class MeetingState {
    CREATED = 0, // 已创建且有 Host，尚无第一名非 Host 激活成员
    ACTIVE = 1,  // 已有非 Host 成员成功激活
    ENDING = 2,  // Host 已发起关闭，正在冻结上下文与清理
    CLOSED = 3   // 终态，由 ClosedMeetingSnapshot 承接查询
};

// ------------------------------------------------------------------------------
// Participant 成员关系状态与角色（Step 1.3 §9.1 / Step 1.2 §9.1）。
//
// state 与 role 刻意分开：它们语义正交，合并成一个 enum 会让状态机判断变得
// 容易出错。
// ------------------------------------------------------------------------------
enum class ParticipantState {
    ACTIVE = 0, // 当前是逻辑活动成员
    LEFT = 1    // 当前不属于活动成员集合；记录保留以供幂等判断与历史鉴权
};

enum class ParticipantRole {
    HOST = 0,        // 创建者/控制角色
    PARTICIPANT = 1  // 普通成员
};

// ------------------------------------------------------------------------------
// ParticipantSessionBinding 关系状态（Step 1.3 §11.1）。只有两个值。
// ------------------------------------------------------------------------------
enum class BindingState {
    BOUND = 0,   // 存在绑定关系
    UNBOUND = 1  // 绑定关系已解除
};

// ------------------------------------------------------------------------------
// 结果分类（Step 1.4 §9.2）。只有三个值。
//
// 它刻意不使用 HTTP 概念，也不使用 bool success：bool 无法区分
// "成功" 与 "幂等但无业务变化"。
// ------------------------------------------------------------------------------
enum class ResultDisposition {
    SUCCESS = 0,
    IDEMPOTENT = 1,
    ERROR = 2
};

// ------------------------------------------------------------------------------
// ResultCode（Step 1.4 §10.1，硬冻结）。
//
// 数值与编号来自 Phase 1 契约，禁止改号、禁止重新编号、禁止新增。
// 这些数值是 Meeting Domain API Contract 的一部分，既不是 TCP message ID，
// 也不是 HTTP/gRPC status。
// ------------------------------------------------------------------------------
enum class ResultCode : int {
    OK = 0,

    ALREADY_JOINED = 100,
    ALREADY_LEFT = 101,
    CLOSE_IN_PROGRESS = 102,
    ALREADY_CLOSED = 103,

    AUTH_REQUIRED = 1000,
    SESSION_STATE_REJECTED = 1001,
    INVALID_ARGUMENT = 1002,

    PERMISSION_DENIED = 1100,
    NOT_PARTICIPANT = 1101,
    HOST_MUST_CLOSE_MEETING = 1102,

    MEETING_NOT_FOUND = 1200,
    MEETING_NOT_LOCAL = 1201,
    MEETING_STATE_REJECTED = 1202,

    INTERNAL_ERROR = 9000
};

// 契约要求每个 Response 都带 result_name（Step 1.4 §9.1），因此这里提供唯一的
// 纯查表函数，让冻结的契约可以被表达。它不是业务逻辑：无状态、无副作用，
// 也不参与任何状态判断。
//
// 客户端业务逻辑必须依赖 result_code / result_name，不得依赖 diagnostic_message。
inline const char* ResultCodeName(ResultCode code) {
    switch (code) {
    case ResultCode::OK:                    return "OK";
    case ResultCode::ALREADY_JOINED:        return "ALREADY_JOINED";
    case ResultCode::ALREADY_LEFT:          return "ALREADY_LEFT";
    case ResultCode::CLOSE_IN_PROGRESS:     return "CLOSE_IN_PROGRESS";
    case ResultCode::ALREADY_CLOSED:        return "ALREADY_CLOSED";
    case ResultCode::AUTH_REQUIRED:         return "AUTH_REQUIRED";
    case ResultCode::SESSION_STATE_REJECTED:return "SESSION_STATE_REJECTED";
    case ResultCode::INVALID_ARGUMENT:      return "INVALID_ARGUMENT";
    case ResultCode::PERMISSION_DENIED:     return "PERMISSION_DENIED";
    case ResultCode::NOT_PARTICIPANT:       return "NOT_PARTICIPANT";
    case ResultCode::HOST_MUST_CLOSE_MEETING:return "HOST_MUST_CLOSE_MEETING";
    case ResultCode::MEETING_NOT_FOUND:     return "MEETING_NOT_FOUND";
    case ResultCode::MEETING_NOT_LOCAL:     return "MEETING_NOT_LOCAL";
    case ResultCode::MEETING_STATE_REJECTED:return "MEETING_STATE_REJECTED";
    case ResultCode::INTERNAL_ERROR:        return "INTERNAL_ERROR";
    }
    return "UNKNOWN_RESULT_CODE";
}

// ------------------------------------------------------------------------------
// 操作 outcome（Step 1.4 §14.2 / §15.2 / §16.3）。
//
// 值集合严格按契约，不为了"enum 看起来完整"而增加成员：
// 幂等与错误结果由 ResultCode + ResultDisposition 表达，不另建一套错误枚举。
// ------------------------------------------------------------------------------
enum class JoinOutcome {
    NEW_PARTICIPANT = 0,          // 首次逻辑加入
    REJOINED_PARTICIPANT = 1,     // 历史 LEFT 成员重新激活
    ADDITIONAL_SESSION_BOUND = 2, // 已 ACTIVE 成员新增一条 Session Binding
    ALREADY_BOUND = 3             // 该 Session 已有有效 Binding，无任何变化
};

enum class LeaveOutcome {
    LEFT = 0 // 本次实际完成逻辑离会
};

enum class CloseOutcome {
    CLOSE_STARTED = 0 // 本次请求合法启动关闭流程，聚合已进入 ENDING
};

// ------------------------------------------------------------------------------
// MeetingRequestContext（Step 1.4 §5）。
//
// 由 transport adapter 构造，不由客户端 payload 填充：这些身份字段是服务端
// 事实，客户端不得覆盖。
//
// 刻意不包含 socket、CSession*、shared_ptr<CSession> 或 LogicNode*；
// 领域层不持有传输层对象。
// ------------------------------------------------------------------------------
struct MeetingRequestContext {
    RequestId request_id;              // 仅用于关联，不是幂等键
    UserId authenticated_user_id = 0;  // 业务身份的唯一来源
    SessionId session_id;              // 当前服务端 Session，不得由客户端指定
    ServerId handling_chat_server_id;  // 实际处理该请求的节点

    // 可选字段一律用 presence flag + value 表达。只有 has_xxx 为 true 时，
    // 对应值才有效；默认构造出的空值不得被解释为"已知的空标识"。
    bool has_device_id = false;
    DeviceId device_id; // 当前代码尚无 Device 概念时不得伪造

    bool has_trusted_owner_chat_server_id = false;
    ServerId trusted_owner_chat_server_id; // 只能由服务端可信来源填充
};

// ------------------------------------------------------------------------------
// MeetingView（Step 1.4 §11）。
//
// 只表达对外可见的会议摘要，不含 Session、Binding、socket、Redis key 或任何
// 内部指针。
// ------------------------------------------------------------------------------
struct MeetingView {
    MeetingId meeting_id;
    ServerId owner_chat_server_id;
    UserId host_user_id = 0;
    MeetingState meeting_state = MeetingState::CREATED;
    std::size_t active_participant_count = 0;
    Timestamp created_at;

    // CREATED/ACTIVE/ENDING 下必须为 false：此时 closed_at 为空。
    // CLOSED 下必须为 true。
    bool has_closed_at = false;
    Timestamp closed_at;
};

// ------------------------------------------------------------------------------
// ParticipantView（Step 1.4 §12）。
//
// 刻意不暴露 binding_id、session_binding_ids、device 列表或 socket：
// 成员列表呈现的是逻辑 Participant，不按设备/Session 展开。
// ------------------------------------------------------------------------------
struct ParticipantView {
    UserId user_id = 0;
    ParticipantRole role = ParticipantRole::PARTICIPANT;
    ParticipantState participant_state = ParticipantState::ACTIVE;

    // joined_at 保持"首次逻辑加入"语义，不因 Re-Join 重置。
    Timestamp joined_at;

    // ACTIVE 时必须为 false；LEFT 时必须为 true。
    bool has_left_at = false;
    Timestamp left_at;
};

// ------------------------------------------------------------------------------
// 终态快照中的历史成员身份（Step 1.2 §13.2）。
//
// 只保留鉴权与审计所需的最小身份与时间边界，不保存活动 Session 或 Binding。
// ------------------------------------------------------------------------------
struct ParticipantIdentitySnapshot {
    UserId user_id = 0;
    ParticipantRole role = ParticipantRole::PARTICIPANT;
    Timestamp joined_at;

    // 关闭时该成员已离会则为 true。
    bool has_left_at = false;
    Timestamp left_at;
};

// ------------------------------------------------------------------------------
// ClosedMeetingSnapshot（Step 1.2 §13.2）。
//
// 关闭后的不可变/受限修改值对象，独立于活动 Meeting 聚合：活动聚合释放后，
// 仍需要它来支持按权限查询历史成员。
//
// 它不保留活动 CSession 指针、不保留有效会议 Session，也不使 Meeting 重新激活；
// 不做 Redis/MySQL 持久化。
//
// snapshot_version 在 Phase 1 中属于"可选建议字段"，当前实现无并发保护或观测
// 需求，因此不加入。
// ------------------------------------------------------------------------------
struct ClosedMeetingSnapshot {
    MeetingId meeting_id;
    UserId host_user_id = 0;
    ServerId owner_chat_server_id;

    // 终态记录，固定为 CLOSED。
    MeetingState meeting_state = MeetingState::CLOSED;

    Timestamp created_at;
    // 快照只存在于 CLOSED 之后，因此 closed_at 必然有效，不使用 presence flag。
    Timestamp closed_at;

    std::vector<ParticipantIdentitySnapshot> participant_identities;
    std::size_t final_participant_count = 0;
};

// ------------------------------------------------------------------------------
// 领域事件（Step 1.2 §12）。
//
// 只有四类事件，用 EventType + 单一 value struct 表达：不建立继承体系、
// 虚基类、event bus、dispatcher 或 MQ 抽象。
//
// 这里只是 value type，不包含任何发布逻辑，也不代表已接入可靠投递、
// ACK、重试或死信队列。
// ------------------------------------------------------------------------------
enum class EventType {
    MEETING_CREATED = 0,
    PARTICIPANT_JOINED = 1,
    PARTICIPANT_LEFT = 2,
    MEETING_CLOSED = 3
};

struct MeetingEvent {
    EventId event_id;       // 仅保证在本地事件作用域内可识别
    EventType event_type = EventType::MEETING_CREATED;
    MeetingId meeting_id;

    // 系统清理可能没有具体发起用户，因此 actor 用 presence flag 表达。
    // 未置 presence 时这里的值没有含义；给一个确定初值只是为了保持类型可
    // 安全默认构造，不代表"用户 0"。
    bool has_actor_user_id = false;
    UserId actor_user_id = 0;

    // 仅在事件确实指向某个受影响成员时填写，同上由 presence flag 决定有效性。
    bool has_participant_user_id = false;
    UserId participant_user_id = 0;

    ServerId owner_chat_server_id;
    Timestamp occurred_at;

    // 最小去重上下文；本文件不规定事件顺序或去重算法。
    std::string dedupe_context;
};

} // namespace meeting
