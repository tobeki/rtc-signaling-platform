#pragma once

// ==============================================================================
// M3 MeetingService：多 Meeting 管理与 in-memory registry。
//
// 本文件是 Domain-first 阶段的**最后一个**编码任务：M1 MeetingTypes +
// M2 MeetingAggregate + M3 MeetingService 组成一个可以完全脱离 TCP /
// LogicSystem / CSession / Redis / MySQL / gRPC 而独立运行全部核心领域语义的
// Meeting Core。
//
// MeetingService 拥有（Step 1.6 §6.5 / §11.1）：
//   active meeting registry            meeting_id → MeetingAggregate
//   closed snapshot registry           meeting_id → ClosedMeetingSnapshot
//   Domain Session records             session_id → DomainSessionRecord
//   session → binding reverse index    session_id → [ (meeting_id, user_id) ]
//   六项 API 应用服务（Step 1.4 §13–§18）
//   SessionDisconnected fan-out（Step 1.3 §13）
//   FinalizeClose direct domain operation（Step 1.4 §16.5）
//   local domain event output
//
// 所有权与线程模型（Step 1.6 §6.4 / §12）：
//   - MeetingService 是**普通可实例化对象**，禁止 Singleton；
//   - MeetingService **自己不加锁，也不是 thread-safe object**。所有 mutation
//     必须由外部 single-writer serialized boundary（M4 的统一 FIFO 队列）顺序
//     调用。因此本文件刻意不出现 std::mutex / recursive_mutex / shared_mutex，
//     也不创建线程、timer、async task 或 background worker。
//
// 依赖边界（不得退化）：
//   MeetingService → MeetingAggregate → MeetingTypes → C++ 标准库
// 不链接 gRPC、protobuf、hiredis、MySQL、JsonCpp、Boost、Threads、asio。
// std::function 属标准库，用于注入确定性的 ID/时间来源。
//
// 本文件**不**包含：LogicSystem 队列、CSession/CServer 适配、TCP message ID、
// Protocol V2、通知发送（M6）、任何持久化。
// ==============================================================================

#include "meeting/MeetingAggregate.h"
#include "meeting/MeetingTypes.h"

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace meeting {

// ------------------------------------------------------------------------------
// 确定性值来源（依赖注入）。
//
// Phase 1 的正式 CreateMeeting 请求**没有** meeting_id 业务字段，客户端不得
// 指定会议 ID；而单元测试又必须可重复。因此 ID 与时间一律由外部注入：
//
//   - 单元测试注入固定序列（"meeting-1" / "event-1" / epoch+N 秒）；
//   - M4 之后的真实集成注入真实 ID 与 clock 来源。
//
// MeetingService 内部**不得**使用 boost::uuids、system_clock::now()、全局序列号
// 或 rand()。
// ------------------------------------------------------------------------------
struct MeetingServiceDependencies {
    std::function<MeetingId()> next_meeting_id;
    std::function<EventId()> next_event_id;
    std::function<Timestamp()> now;
};

// ------------------------------------------------------------------------------
// 领域 Session 记录（Step 1.3 §5）。
//
// 只保留纯领域状态与可选的身份绑定，绝不保存 CSession*、shared_ptr<CSession>、
// socket 或 io_context：领域层不持有传输层对象（Step 1.3 §13.5）。
//
// CLOSED 之后记录作为**最小 tombstone** 保留（Step 1.3 §5.2：CLOSED → * 非法），
// 用途有三：
//   1. 重复 SessionDisconnected 可以明确幂等；
//   2. 已排入 serialized queue 的 stale command 在 disconnect 之后仍能得到
//      SESSION_STATE_REJECTED，而不是 AUTH_REQUIRED；
//   3. 不把已 CLOSED 的 Session 错报成"未认证"。
//
// tombstone 不保留 socket、网络对象或 Meeting Binding。其保留期/回收策略为
// **Deferred**：M3 不实现 TTL、timer 或清理线程。
// ------------------------------------------------------------------------------
struct DomainSessionRecord {
    SessionId session_id;
    DomainSessionState state = DomainSessionState::CONNECTED;

    // 仅在 AUTHENTICATED 之后有效；CONNECTED 阶段不得伪造身份。
    bool has_user_id = false;
    UserId user_id = 0;
};

// ------------------------------------------------------------------------------
// ListParticipants 的 participant_view_scope（Step 1.4 §18.4）。
//
// 这是**领域 API 枚举**，不是 wire encoding，也不承诺任何数值编码。
// ------------------------------------------------------------------------------
enum class ParticipantViewScope {
    CURRENT = 0,    // CREATED / ACTIVE / ENDING：只列活动逻辑成员
    HISTORICAL = 1  // CLOSED：读取 ClosedMeetingSnapshot 中的历史身份
};

// ------------------------------------------------------------------------------
// 反向索引元素（Step 1.3 §13.2、Step 1.6 §11.2）。
//
// 只保存稳定 ID，绝不保存 Participant* / Binding* / MeetingAggregate* / CSession*。
// 该索引与 UserMgr 无关，也不依赖任何外部会话管理器。
// ------------------------------------------------------------------------------
struct BindingRef {
    MeetingId meeting_id;
    UserId user_id = 0;
};

// ------------------------------------------------------------------------------
// 六项 API 的公共结果字段（Step 1.4 §9.1）。
//
// result_name 必须来自 ResultCodeName()，不得维护第二套字符串映射。
// has_owner_chat_server_id 主要用于 MEETING_NOT_LOCAL：只有该结果才携带一个
// 可信的异地 owner，供调用方重定向。
// ------------------------------------------------------------------------------
struct MeetingServiceResultCommon {
    RequestId request_id;

    ResultCode result_code = ResultCode::INTERNAL_ERROR;
    std::string result_name;
    ResultDisposition disposition = ResultDisposition::ERROR;

    bool has_owner_chat_server_id = false;
    ServerId owner_chat_server_id;
};

// ResultCode → ResultDisposition 的唯一映射（Step 1.4 §9.2）。
//
// 六个 API 共用它，避免各自复制一套容易漂移的判断：
//   OK                                   → SUCCESS
//   ALREADY_JOINED / ALREADY_LEFT /
//   CLOSE_IN_PROGRESS / ALREADY_CLOSED   → IDEMPOTENT
//   其余                                  → ERROR
ResultDisposition ResultDispositionFor(ResultCode code);

// ------------------------------------------------------------------------------
// 六项 API 的结果类型（Step 1.4 §13–§18）。
//
// 刻意使用具名 struct 组合既有值类型，不使用 void* / std::any / variant，
// 也不使用 C++17。
// ------------------------------------------------------------------------------
struct CreateMeetingResult {
    MeetingServiceResultCommon common;

    MeetingView meeting;
    ParticipantView host_participant;
    std::vector<MeetingEvent> events;
};

struct JoinMeetingResult {
    MeetingServiceResultCommon common;

    JoinOutcome outcome = JoinOutcome::NEW_PARTICIPANT;
    MeetingView meeting;
    ParticipantView participant;
    std::vector<MeetingEvent> events;
};

struct LeaveMeetingResult {
    MeetingServiceResultCommon common;

    LeaveOutcome outcome = LeaveOutcome::LEFT;
    MeetingView meeting;
    std::vector<MeetingEvent> events;
};

struct CloseMeetingResult {
    MeetingServiceResultCommon common;

    CloseOutcome outcome = CloseOutcome::CLOSE_STARTED;
    MeetingView meeting;

    // 只有首次合法 BeginClose 为 true：调用方据此向同一 serialized boundary
    // 提交恰好一个 FinalizeClose work item。CloseMeeting 自己**绝不**执行
    // FinalizeClose（Step 1.4 §16.5.2）。
    bool should_schedule_finalize = false;

    // BeginClose 不产生任何领域事件，因此这里在为成功路径时也恒为空。
    std::vector<MeetingEvent> events;
};

struct QueryMeetingResult {
    MeetingServiceResultCommon common;
    MeetingView meeting;
};

struct ListParticipantsResult {
    MeetingServiceResultCommon common;

    MeetingView meeting;
    ParticipantViewScope scope = ParticipantViewScope::CURRENT;
    std::vector<ParticipantView> participants;
};

// ------------------------------------------------------------------------------
// FinalizeClose 的内部结果（Step 1.4 §16.5.6：不是外部 API，无客户端 Response）。
// ------------------------------------------------------------------------------
struct FinalizeCloseResult {
    // 本次调用真实完成了 ENDING → CLOSED 收敛。
    bool finalized = false;

    // active 聚合已释放、ClosedSnapshot 已存在：重复 FinalizeClose。
    bool already_finalized = false;

    MeetingId meeting_id;

    // finalized / already_finalized 时有效。
    bool has_snapshot = false;
    ClosedMeetingSnapshot snapshot;

    // 只有首次 finalize 产生恰好一个 MeetingClosed；重复 finalize 为空。
    std::vector<MeetingEvent> events;

    // 只有首次 finalize 输出一次通知意图（来源：CloseContext.notifiable_session_ids）。
    // M3 不执行任何发送；M6 才做 session_id → CSession → best-effort send。
    std::vector<SessionId> notification_session_ids;
};

// ------------------------------------------------------------------------------
// SessionDisconnected 的内部结果（Step 1.3 §13.2）。
// 同样不是客户端 Response。
// ------------------------------------------------------------------------------
struct SessionDisconnectedResult {
    // 该 session 是否存在于领域 Session registry。
    bool session_known = false;

    // 调用前该 session 已处于 CLOSED：整体幂等，无任何变更与事件。
    bool already_closed = false;

    // 本次 fan-out 覆盖到的 Meeting 数量（0 / 1 / N）。
    std::size_t affected_meeting_count = 0;

    // 本 mutation 结束后的 Domain Session 状态（正常路径恒为 CLOSED）。
    DomainSessionState final_state = DomainSessionState::CLOSED;

    std::vector<MeetingEvent> events;
};

// ------------------------------------------------------------------------------
// MeetingService
// ------------------------------------------------------------------------------
class MeetingService {
public:
    // 非 Singleton：每次构造都得到一份互不共享的内存 registry。
    MeetingService(const ServerId& local_server_id,
                   MeetingServiceDependencies dependencies);

    // ---------------------------------------------------------------------------
    // 领域 Session 生命周期（纯领域状态转换，不是 TCP adapter）
    //
    //   new session              → CONNECTED
    //   auth success             → AUTHENTICATED
    //   SessionDisconnected      → CLOSING → fan-out cleanup → CLOSED
    //
    // M5/M6 才把真实 CSession 生命周期接到这些入口上。
    // ---------------------------------------------------------------------------

    // 注册一条新的 CONNECTED 领域 Session。session_id 为空或已存在时返回 false
    // （后者幂等，不覆盖已有状态）。
    bool RegisterConnectedSession(const SessionId& session_id);

    // CONNECTED → AUTHENTICATED，并绑定 user_id。
    // 只有已注册且当前为 CONNECTED、且 user_id 为合法正数时才成功。
    // 重复认证、未知 session、已 CLOSING/CLOSED 均返回 false 且不改变状态。
    bool AuthenticateSession(const SessionId& session_id, UserId user_id);

    // 读取领域 Session 状态（只读）。session 不存在时返回 false。
    bool TryGetDomainSessionState(const SessionId& session_id,
                                  DomainSessionState& out_state) const;

    // ---------------------------------------------------------------------------
    // 六项正式 API（Step 1.4 §13–§18）
    //
    // 每一项都遵循 §19.1 的八步求值顺序，且都只接受由 transport adapter 构造的
    // MeetingRequestContext；客户端 payload 不得提供身份字段。
    //
    // MeetingView 的填充规则（本地实现裁决）：只有在**授权通过**（或幂等命中，
    // 而幂等命中本身已意味着授权通过）时才填充 MeetingView；授权失败
    // （PERMISSION_DENIED / NOT_PARTICIPANT）时保持其为默认空值。Phase 1 未要求
    // 在这些失败路径上返回视图，Step 1.4 §16.4 明确"按可见规则返回或不返回"，
    // 因此这里选择不向非成员泄露会议摘要（成员数 / host_user_id）。
    // ---------------------------------------------------------------------------

    // API 1：CreateMeeting。没有 meeting_id 业务参数，ID 由注入来源产生。
    // 重复 request_id 会创建**不同**的 Meeting（request_id 不是幂等键）。
    CreateMeetingResult CreateMeeting(const MeetingRequestContext& context);

    // API 2：JoinMeeting
    JoinMeetingResult JoinMeeting(const MeetingRequestContext& context,
                                  const MeetingId& meeting_id);

    // API 3：LeaveMeeting（User 级逻辑离会）
    LeaveMeetingResult LeaveMeeting(const MeetingRequestContext& context,
                                    const MeetingId& meeting_id);

    // API 4：CloseMeeting（只完成 BeginClose 这一 turn）
    CloseMeetingResult CloseMeeting(const MeetingRequestContext& context,
                                    const MeetingId& meeting_id);

    // API 5：QueryMeeting（只读）
    QueryMeetingResult QueryMeeting(const MeetingRequestContext& context,
                                    const MeetingId& meeting_id);

    // API 6：ListParticipants（只读，CURRENT / HISTORICAL）
    ListParticipantsResult ListParticipants(const MeetingRequestContext& context,
                                            const MeetingId& meeting_id);

    // ---------------------------------------------------------------------------
    // 内部 domain operations（不是外部 API，不产生客户端 Response）
    // ---------------------------------------------------------------------------

    // 第二个 serialized turn：ENDING → CLOSED 的收敛。
    //
    // 逻辑顺序（Step 1.3 §18.2、Step 1.5 §23）：
    //   resolve ENDING aggregate → obtain frozen CloseContext
    //   → cleanup reverse-index refs for this Meeting
    //   → build complete ClosedMeetingSnapshot
    //   → **publish snapshot 先于 release active aggregate**
    //   → produce MeetingClosed exactly once
    //   → capture notification target session ids
    //   → release active aggregate
    //
    // 重复调用（active 已释放、snapshot 已存在）返回 already_finalized = true，
    // 不再产生事件，也不再返回通知意图。
    FinalizeCloseResult FinalizeClose(const MeetingId& meeting_id);

    // SessionDisconnected fan-out（Step 1.3 §13.2、Step 1.5 §12）。
    //
    // 全部动作在**一次** serialized mutation 内完成：
    //   Domain Session → CLOSING
    //   → 复制反向索引中受影响的 refs
    //   → 对每个受影响 Meeting 独立裁决其精确 Binding
    //   → 清除该 session 的反向索引
    //   → 只有全部 Meeting 处理完成后 Domain Session 才 → CLOSED
    //
    // 重复调用整体幂等。
    SessionDisconnectedResult SessionDisconnected(const SessionId& session_id);

    // ---------------------------------------------------------------------------
    // 受控只读诊断 / 维护入口
    //
    // 刻意**不**提供 GetActiveRegistry() / GetReverseIndex() 之类会暴露内部
    // 容器的接口（Step 1.6 §11.1）。
    // ---------------------------------------------------------------------------

    const ServerId& LocalServerId() const { return local_server_id_; }

    bool HasActiveMeeting(const MeetingId& meeting_id) const;
    bool HasClosedSnapshot(const MeetingId& meeting_id) const;
    std::size_t ReverseBindingCountForSession(const SessionId& session_id) const;

    // ClosedSnapshot 淘汰钩子：唯一目的是表达"快照已被 retention layer 淘汰"
    // → 后续 Query / List 返回 MEETING_NOT_FOUND。
    //
    // 它**不是**客户端 Meeting API，也不实现任何自动策略：M3 不引入 TTL、
    // timer 或 background cleaner。
    bool EvictClosedSnapshot(const MeetingId& meeting_id);

private:
    // --- 八步求值顺序的前三步（六个 API 共用） ---------------------------------

    // 步骤 1：Request Context 结构校验。
    // 注意：authenticated_user_id == 0 在这里**不**报 INVALID_ARGUMENT，否则
    // 未认证 Session 永远得不到正式的 AUTH_REQUIRED。
    bool ValidateContext(const MeetingRequestContext& context,
                         ResultCode& out_code) const;

    // 步骤 2：Domain Session 可用性。
    //   missing / CONNECTED                → AUTH_REQUIRED
    //   CLOSING / CLOSED                   → SESSION_STATE_REJECTED
    //   AUTHENTICATED 但 user_id 与 context 不一致 → INVALID_ARGUMENT
    bool CheckDomainSessionUsability(const MeetingRequestContext& context,
                                     ResultCode& out_code) const;

    // 步骤 3：API 参数校验（必须先于任何 registry lookup）。
    bool ValidateMeetingIdArgument(const MeetingId& meeting_id,
                                   ResultCode& out_code) const;

    // 步骤 4：locality 解析。
    // 本地 active / closed 均不存在时，才看可信的 trusted_owner_chat_server_id：
    // 有且指向其他节点 → MEETING_NOT_LOCAL（并回填 owner）；否则 MEETING_NOT_FOUND。
    ResultCode ResolveLocality(const MeetingRequestContext& context,
                               const MeetingId& meeting_id,
                               ServerId& out_owner) const;

    // --- 结果装配 --------------------------------------------------------------

    MeetingServiceResultCommon MakeCommon(const RequestId& request_id,
                                          ResultCode code) const;

    // CLOSED 视图只能从 ClosedMeetingSnapshot 构造（Step 1.4 §26.1）。
    MeetingView BuildClosedMeetingView(const ClosedMeetingSnapshot& snapshot) const;

    // 历史身份投影：ACTIVE 只表示"Meeting 关闭边界上仍为 ACTIVE"，与在线状态无关。
    ParticipantView BuildHistoricalParticipantView(
        const ParticipantIdentitySnapshot& identity) const;

    bool IsHistoricalParticipant(const ClosedMeetingSnapshot& snapshot,
                                 UserId user_id) const;

    // --- 反向索引维护 ----------------------------------------------------------

    // 幂等插入：同一 (session_id, meeting_id, user_id) 最多一条。
    void AddBindingRef(const SessionId& session_id, const BindingRef& ref);

    // Leave 成功：删除该 (meeting_id, user_id) 对应的**全部** Session refs。
    void RemoveBindingRefsForMeetingAndUser(const MeetingId& meeting_id, UserId user_id);

    // SessionDisconnected：删除该 session 的全部 refs。
    void RemoveBindingRefsForSession(const SessionId& session_id);

    // FinalizeClose：删除该 Meeting 的全部剩余 refs。
    void RemoveBindingRefsForMeeting(const MeetingId& meeting_id);

    // --- 内部 register 查找 ----------------------------------------------------

    // 返回 nullptr 表示该 MeetingId 不在 active registry 中。
    MeetingAggregate* FindActiveMeeting(const MeetingId& meeting_id);
    const ClosedMeetingSnapshot* FindClosedSnapshot(const MeetingId& meeting_id) const;

    // --- 状态 ------------------------------------------------------------------

    ServerId local_server_id_;
    MeetingServiceDependencies dependencies_;

    std::map<MeetingId, MeetingAggregate> active_meetings_;
    std::map<MeetingId, ClosedMeetingSnapshot> closed_snapshots_;
    std::map<SessionId, DomainSessionRecord> sessions_;
    std::map<SessionId, std::vector<BindingRef> > reverse_index_;
};

} // namespace meeting
