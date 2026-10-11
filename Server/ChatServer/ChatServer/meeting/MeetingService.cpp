// ==============================================================================
// M3 MeetingService 实现。
//
// 全部规则来自已提交的 Phase 1 文档：
//   - Step 1.3 §5        领域 Session 状态与合法转换
//   - Step 1.3 §13       SessionDisconnected fan-out（0 / 1 / N）
//   - Step 1.3 §14       Host Disconnect 不关闭 Meeting
//   - Step 1.3 §17/§18   BeginClose / FinalizeClose 切分与 CLOSED 原子发布
//   - Step 1.3 §23       I1–I12 稳定状态不变量
//   - Step 1.4 §13–§18   六项 API 语义与结果码
//   - Step 1.4 §19/§20   八步求值顺序与错误优先级矩阵
//   - Step 1.4 §21–§26   事件契约、CLOSED 快照语义
//   - Step 1.5 §12/§13/§23  fan-out、两 turn Close、快照发布路径
//   - Step 1.6 §6.5/§11/§16/§19  MeetingService 职责、反向索引、Close 切分
//
// 本文件不引入线程、锁、timer、async task、网络或持久化。所有 mutation 都假定
// 由外部 single-writer serialized boundary 顺序调用。
// ==============================================================================

#include "meeting/MeetingService.h"

#include <utility>

namespace meeting {

// ------------------------------------------------------------------------------
// ResultCode → ResultDisposition（Step 1.4 §9.2）
// ------------------------------------------------------------------------------

ResultDisposition ResultDispositionFor(ResultCode code) {
    if (code == ResultCode::OK) {
        return ResultDisposition::SUCCESS;
    }
    switch (code) {
    case ResultCode::ALREADY_JOINED:
    case ResultCode::ALREADY_LEFT:
    case ResultCode::CLOSE_IN_PROGRESS:
    case ResultCode::ALREADY_CLOSED:
        return ResultDisposition::IDEMPOTENT;
    default:
        return ResultDisposition::ERROR;
    }
}

// ------------------------------------------------------------------------------
// 构造
// ------------------------------------------------------------------------------

MeetingService::MeetingService(const ServerId& local_server_id,
                               MeetingServiceDependencies dependencies)
    : local_server_id_(local_server_id),
      dependencies_(dependencies) {
}

// ------------------------------------------------------------------------------
// 领域 Session 生命周期
// ------------------------------------------------------------------------------

bool MeetingService::RegisterConnectedSession(const SessionId& session_id) {
    if (session_id.empty()) {
        return false;
    }
    if (sessions_.find(session_id) != sessions_.end()) {
        // 重复注册是幂等的 no-op：绝不覆盖已有状态（尤其是 AUTHENTICATED）。
        return false;
    }

    DomainSessionRecord record;
    record.session_id = session_id;
    record.state = DomainSessionState::CONNECTED;
    record.has_user_id = false;
    record.user_id = 0;
    sessions_[session_id] = record;
    return true;
}

bool MeetingService::AuthenticateSession(const SessionId& session_id, UserId user_id) {
    // 非正数不是合法领域身份：拒绝而不是登记一个永远无法与 context 匹配的
    // "已认证但身份为 0" 的 Session。
    if (user_id <= 0) {
        return false;
    }

    std::map<SessionId, DomainSessionRecord>::iterator it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return false;
    }

    DomainSessionRecord& record = it->second;
    // 只有 CONNECTED → AUTHENTICATED 是合法转换。
    // 重复认证、CLOSING/CLOSED 下的认证都不改变状态（Step 1.3 §5.3）。
    if (record.state != DomainSessionState::CONNECTED) {
        return false;
    }

    record.state = DomainSessionState::AUTHENTICATED;
    record.has_user_id = true;
    record.user_id = user_id;
    return true;
}

bool MeetingService::TryGetDomainSessionState(const SessionId& session_id,
                                             DomainSessionState& out_state) const {
    std::map<SessionId, DomainSessionRecord>::const_iterator it =
        sessions_.find(session_id);
    if (it == sessions_.end()) {
        return false;
    }
    out_state = it->second.state;
    return true;
}

// ------------------------------------------------------------------------------
// 八步顺序的前三步
// ------------------------------------------------------------------------------

bool MeetingService::ValidateContext(const MeetingRequestContext& context,
                                     ResultCode& out_code) const {
    if (context.request_id.empty()) {
        out_code = ResultCode::INVALID_ARGUMENT;
        return false;
    }
    if (context.session_id.empty()) {
        out_code = ResultCode::INVALID_ARGUMENT;
        return false;
    }
    if (context.handling_chat_server_id.empty()) {
        out_code = ResultCode::INVALID_ARGUMENT;
        return false;
    }
    // handling 节点必须就是本服务所在节点：否则这条命令不该在这里求值。
    if (context.handling_chat_server_id != local_server_id_) {
        out_code = ResultCode::INVALID_ARGUMENT;
        return false;
    }
    // presence flag 为 true 时值必须非空，否则是 adapter 构造出的自相矛盾 context。
    if (context.has_device_id && context.device_id.empty()) {
        out_code = ResultCode::INVALID_ARGUMENT;
        return false;
    }
    if (context.has_trusted_owner_chat_server_id &&
        context.trusted_owner_chat_server_id.empty()) {
        out_code = ResultCode::INVALID_ARGUMENT;
        return false;
    }

    // 刻意**不**因为 authenticated_user_id == 0 就返回 INVALID_ARGUMENT：
    // 未认证 Session 必须能走到步骤 2 拿到正式的 AUTH_REQUIRED。
    return true;
}

bool MeetingService::CheckDomainSessionUsability(const MeetingRequestContext& context,
                                                 ResultCode& out_code) const {
    std::map<SessionId, DomainSessionRecord>::const_iterator it =
        sessions_.find(context.session_id);

    // 没有领域记录的 Session 视为未认证。
    if (it == sessions_.end()) {
        out_code = ResultCode::AUTH_REQUIRED;
        return false;
    }

    const DomainSessionRecord& record = it->second;
    if (record.state == DomainSessionState::CONNECTED) {
        out_code = ResultCode::AUTH_REQUIRED;
        return false;
    }
    // 已认证但正在关闭 / 已关闭：必须报 SESSION_STATE_REJECTED，
    // 不得误报成 AUTH_REQUIRED（Step 1.4 §19.4）。
    if (record.state == DomainSessionState::CLOSING ||
        record.state == DomainSessionState::CLOSED) {
        out_code = ResultCode::SESSION_STATE_REJECTED;
        return false;
    }

    // 到这里 record.state == AUTHENTICATED。
    // server-built context 声称的身份必须与领域记录一致：这是 service 上下文
    // 自相矛盾，说明 adapter 构造有 bug，而不是客户端越权。不得信任 context
    // 声称的 identity 去覆盖领域事实。
    if (!record.has_user_id || record.user_id != context.authenticated_user_id) {
        out_code = ResultCode::INVALID_ARGUMENT;
        return false;
    }
    return true;
}

bool MeetingService::ValidateMeetingIdArgument(const MeetingId& meeting_id,
                                               ResultCode& out_code) const {
    // 只要求非空。刻意不发明 UUID regex、固定长度或 wire encoding 约束
    // （Step 1.4 §19.1.2）。
    if (meeting_id.empty()) {
        out_code = ResultCode::INVALID_ARGUMENT;
        return false;
    }
    return true;
}

ResultCode MeetingService::ResolveLocality(const MeetingRequestContext& context,
                                            const MeetingId& meeting_id,
                                            ServerId& out_owner) const {
    // 本地权威来源优先：active registry 与 closed snapshot registry 互斥。
    if (active_meetings_.find(meeting_id) != active_meetings_.end()) {
        return ResultCode::OK;
    }
    if (closed_snapshots_.find(meeting_id) != closed_snapshots_.end()) {
        return ResultCode::OK;
    }

    // 本地都不存在时，才允许使用**服务端可信**的 owner 信息。
    // MeetingRequestContext 没有 client_owner_hint 字段，因此未经验证的客户端
    // 提示根本无法进入这条 trusted path（Step 1.4 §8.4）。
    if (context.has_trusted_owner_chat_server_id &&
        context.trusted_owner_chat_server_id != context.handling_chat_server_id) {
        out_owner = context.trusted_owner_chat_server_id;
        return ResultCode::MEETING_NOT_LOCAL;
    }

    // 无可信 owner，或可信 owner 就是本节点但本地不存在。
    return ResultCode::MEETING_NOT_FOUND;
}

// ------------------------------------------------------------------------------
// 结果装配
// ------------------------------------------------------------------------------

MeetingServiceResultCommon MeetingService::MakeCommon(const RequestId& request_id,
                                                      ResultCode code) const {
    MeetingServiceResultCommon common;
    common.request_id = request_id;
    common.result_code = code;
    // 唯一的字符串来源：不维护第二套映射。
    common.result_name = ResultCodeName(code);
    common.disposition = ResultDispositionFor(code);
    common.has_owner_chat_server_id = false;
    return common;
}

MeetingView MeetingService::BuildClosedMeetingView(
    const ClosedMeetingSnapshot& snapshot) const {
    MeetingView view;
    view.meeting_id = snapshot.meeting_id;
    view.owner_chat_server_id = snapshot.owner_chat_server_id;
    view.host_user_id = snapshot.host_user_id;
    view.meeting_state = MeetingState::CLOSED;
    // 终态摘要中的成员数来自冻结边界上的 ACTIVE 数，而不是历史加入数。
    view.active_participant_count = snapshot.final_participant_count;
    view.created_at = snapshot.created_at;
    view.has_closed_at = true;
    view.closed_at = snapshot.closed_at;
    return view;
}

ParticipantView MeetingService::BuildHistoricalParticipantView(
    const ParticipantIdentitySnapshot& identity) const {
    ParticipantView view;
    view.user_id = identity.user_id;
    view.role = identity.role;
    // ACTIVE 在这里只表示"Meeting 关闭边界上该成员仍为 ACTIVE"，
    // 不表示 Presence / Session 在线状态。
    view.participant_state = identity.has_left_at ? ParticipantState::LEFT
                                                  : ParticipantState::ACTIVE;
    view.joined_at = identity.joined_at;
    view.has_left_at = identity.has_left_at;
    view.left_at = identity.has_left_at ? identity.left_at : Timestamp();
    return view;
}

bool MeetingService::IsHistoricalParticipant(const ClosedMeetingSnapshot& snapshot,
                                             UserId user_id) const {
    if (snapshot.host_user_id == user_id) {
        return true;
    }
    for (std::size_t i = 0; i < snapshot.participant_identities.size(); ++i) {
        if (snapshot.participant_identities[i].user_id == user_id) {
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------------------
// 反向索引维护
//
// 不变式：同一 (session_id, meeting_id, user_id) 最多一条 ref（Step 1.6 §11.2）。
// ------------------------------------------------------------------------------

void MeetingService::AddBindingRef(const SessionId& session_id, const BindingRef& ref) {
    std::vector<BindingRef>& refs = reverse_index_[session_id];
    for (std::size_t i = 0; i < refs.size(); ++i) {
        if (refs[i].meeting_id == ref.meeting_id && refs[i].user_id == ref.user_id) {
            // 重复 Join / 重复绑定不得把索引变成 [(M,U), (M,U)]。
            return;
        }
    }
    refs.push_back(ref);
}

void MeetingService::RemoveBindingRefsForMeetingAndUser(const MeetingId& meeting_id,
                                                        UserId user_id) {
    std::map<SessionId, std::vector<BindingRef> >::iterator it = reverse_index_.begin();
    while (it != reverse_index_.end()) {
        std::vector<BindingRef>& refs = it->second;
        std::vector<BindingRef> kept;
        kept.reserve(refs.size());
        for (std::size_t i = 0; i < refs.size(); ++i) {
            if (refs[i].meeting_id == meeting_id && refs[i].user_id == user_id) {
                continue; // 成功 Leave 解除该 User 的全部 Session refs
            }
            kept.push_back(refs[i]);
        }
        refs.swap(kept);
        if (refs.empty()) {
            // 该 session 不再关联任何 Meeting Binding，索引项一并移除。
            std::map<SessionId, std::vector<BindingRef> >::iterator dead = it;
            ++it;
            reverse_index_.erase(dead);
        } else {
            ++it;
        }
    }
}

void MeetingService::RemoveBindingRefsForSession(const SessionId& session_id) {
    reverse_index_.erase(session_id);
}

void MeetingService::RemoveBindingRefsForMeeting(const MeetingId& meeting_id) {
    std::map<SessionId, std::vector<BindingRef> >::iterator it = reverse_index_.begin();
    while (it != reverse_index_.end()) {
        std::vector<BindingRef>& refs = it->second;
        std::vector<BindingRef> kept;
        kept.reserve(refs.size());
        for (std::size_t i = 0; i < refs.size(); ++i) {
            if (refs[i].meeting_id == meeting_id) {
                continue;
            }
            kept.push_back(refs[i]);
        }
        refs.swap(kept);
        if (refs.empty()) {
            std::map<SessionId, std::vector<BindingRef> >::iterator dead = it;
            ++it;
            reverse_index_.erase(dead);
        } else {
            ++it;
        }
    }
}

// ------------------------------------------------------------------------------
// registry 查找
// ------------------------------------------------------------------------------

MeetingAggregate* MeetingService::FindActiveMeeting(const MeetingId& meeting_id) {
    std::map<MeetingId, MeetingAggregate>::iterator it = active_meetings_.find(meeting_id);
    return it == active_meetings_.end() ? 0 : &it->second;
}

const ClosedMeetingSnapshot* MeetingService::FindClosedSnapshot(
    const MeetingId& meeting_id) const {
    std::map<MeetingId, ClosedMeetingSnapshot>::const_iterator it =
        closed_snapshots_.find(meeting_id);
    return it == closed_snapshots_.end() ? 0 : &it->second;
}

// ------------------------------------------------------------------------------
// 受控只读诊断 / 维护
// ------------------------------------------------------------------------------

bool MeetingService::HasActiveMeeting(const MeetingId& meeting_id) const {
    return active_meetings_.find(meeting_id) != active_meetings_.end();
}

bool MeetingService::HasClosedSnapshot(const MeetingId& meeting_id) const {
    return closed_snapshots_.find(meeting_id) != closed_snapshots_.end();
}

std::size_t MeetingService::ReverseBindingCountForSession(
    const SessionId& session_id) const {
    std::map<SessionId, std::vector<BindingRef> >::const_iterator it =
        reverse_index_.find(session_id);
    return it == reverse_index_.end() ? 0 : it->second.size();
}

bool MeetingService::EvictClosedSnapshot(const MeetingId& meeting_id) {
    // 只有表达"retention layer 已淘汰该快照"这一个语义，不实现任何时间策略。
    return closed_snapshots_.erase(meeting_id) > 0;
}

// ------------------------------------------------------------------------------
// API 1：CreateMeeting（Step 1.4 §13）
//
// 顺序：context → session → （无业务参数）→ 生成 ID → 冲突检查 →
//       MeetingAggregate::Create → publish active registry → host 反向索引 →
//       response + MeetingCreated
//
// request_id 只做 correlation：相同 request_id 的两次 Create 会得到两个不同的
// Meeting、两次 OK、两个独立的 MeetingCreated（Step 1.4 §13.5）。
// ------------------------------------------------------------------------------

CreateMeetingResult MeetingService::CreateMeeting(const MeetingRequestContext& context) {
    CreateMeetingResult result;

    ResultCode code = ResultCode::INTERNAL_ERROR;
    if (!ValidateContext(context, code)) {
        result.common = MakeCommon(context.request_id, code);
        return result;
    }
    if (!CheckDomainSessionUsability(context, code)) {
        result.common = MakeCommon(context.request_id, code);
        return result;
    }

    // 本 API 没有业务 payload，因此不存在 meeting_id 参数校验。
    const MeetingId meeting_id = dependencies_.next_meeting_id();

    // 注入来源产出的 ID 必须是新的：既不能覆盖活动 Meeting，也不能覆盖已 CLOSED
    // 的权威快照。这不是可以用幂等掩盖的情况，而是领域不变量被破坏。
    if (meeting_id.empty() ||
        active_meetings_.find(meeting_id) != active_meetings_.end() ||
        closed_snapshots_.find(meeting_id) != closed_snapshots_.end()) {
        result.common = MakeCommon(context.request_id, ResultCode::INTERNAL_ERROR);
        return result;
    }

    CreateMeetingParams params;
    params.meeting_id = meeting_id;
    params.host_user_id = context.authenticated_user_id;
    params.host_session_id = context.session_id;
    params.owner_chat_server_id = context.handling_chat_server_id;
    params.created_at = dependencies_.now();
    params.event_id = dependencies_.next_event_id();
    params.request_id = context.request_id;
    params.has_host_device_id = context.has_device_id;
    params.host_device_id = context.has_device_id ? context.device_id : DeviceId();

    MeetingEvent created_event;
    MeetingAggregate aggregate = MeetingAggregate::Create(params, created_event);

    // 7. publish active registry。之后只通过 registry 引用操作该聚合，
    //    绝不把 MeetingAggregate* 交给外部调用者。
    std::map<MeetingId, MeetingAggregate>::iterator inserted =
        active_meetings_.insert(
            std::make_pair(meeting_id, std::move(aggregate))).first;
    MeetingAggregate& stored = inserted->second;

    // 8. Host 初始 Binding 建立 → 反向索引登记 host session。
    BindingRef ref;
    ref.meeting_id = meeting_id;
    ref.user_id = context.authenticated_user_id;
    AddBindingRef(context.session_id, ref);

    result.common = MakeCommon(context.request_id, ResultCode::OK);
    result.meeting = stored.BuildMeetingView();
    stored.TryGetParticipantView(context.authenticated_user_id, result.host_participant);
    // 恰好一个 MeetingCreated；CreateMeeting 不产生 ParticipantJoined。
    result.events.push_back(created_event);
    return result;
}

// ------------------------------------------------------------------------------
// API 2：JoinMeeting（Step 1.4 §14）
//
// 步骤 5（resource-scoped authorization）对本 API 不适用：Join 本身就是建立
// 成员关系，没有额外的身份门槛。因此流程是 context → session → argument →
// locality/state gate → aggregate.Join → 反向索引 → response。
// ------------------------------------------------------------------------------

JoinMeetingResult MeetingService::JoinMeeting(const MeetingRequestContext& context,
                                              const MeetingId& meeting_id) {
    JoinMeetingResult result;

    ResultCode code = ResultCode::INTERNAL_ERROR;
    if (!ValidateContext(context, code)) {
        result.common = MakeCommon(context.request_id, code);
        return result;
    }
    if (!CheckDomainSessionUsability(context, code)) {
        result.common = MakeCommon(context.request_id, code);
        return result;
    }
    // 参数校验必须发生在任何 registry lookup / locality 判断之前。
    if (!ValidateMeetingIdArgument(meeting_id, code)) {
        result.common = MakeCommon(context.request_id, code);
        return result;
    }

    MeetingAggregate* aggregate = FindActiveMeeting(meeting_id);

    if (aggregate == 0) {
        // 本地没有活动聚合：已 CLOSED 的 Meeting 仍然"存在"，因此不是 NOT_FOUND。
        const ClosedMeetingSnapshot* snapshot = FindClosedSnapshot(meeting_id);
        if (snapshot != 0) {
            result.common = MakeCommon(context.request_id,
                                       ResultCode::MEETING_STATE_REJECTED);
            result.meeting = BuildClosedMeetingView(*snapshot);
            return result;
        }

        ServerId owner;
        const ResultCode locality = ResolveLocality(context, meeting_id, owner);
        result.common = MakeCommon(context.request_id, locality);
        if (locality == ResultCode::MEETING_NOT_LOCAL) {
            result.common.has_owner_chat_server_id = true;
            result.common.owner_chat_server_id = owner;
        }
        return result;
    }

    MutationParams params;
    params.now = dependencies_.now();
    // 只有真正产生事件时聚合才会消费该 event_id；其余路径无副作用。
    params.event_id = dependencies_.next_event_id();
    params.has_device_id = context.has_device_id;
    params.device_id = context.has_device_id ? context.device_id : DeviceId();

    const JoinResult joined = aggregate->Join(context.authenticated_user_id,
                                              context.session_id, context.request_id,
                                              params);

    // 反向索引维护：只有**真正成功**建立/恢复绑定关系的三种 outcome 需要登记，
    // 且插入本身幂等（禁止 (M,U) 重复出现）。
    //
    // 必须以 result_code == OK 为准：被拒绝的路径（例如 ENDING 下的
    // MEETING_STATE_REJECTED）不会改动状态，但 JoinResult::outcome 在这类路径上
    // 仍保留其默认值，仅凭 outcome 判断会错误地往索引里塞 ref。
    const bool binding_established =
        joined.result_code == ResultCode::OK &&
        (joined.outcome == JoinOutcome::NEW_PARTICIPANT ||
         joined.outcome == JoinOutcome::REJOINED_PARTICIPANT ||
         joined.outcome == JoinOutcome::ADDITIONAL_SESSION_BOUND);
    if (binding_established) {
        BindingRef ref;
        ref.meeting_id = meeting_id;
        ref.user_id = context.authenticated_user_id;
        AddBindingRef(context.session_id, ref);
    }

    result.common = MakeCommon(context.request_id, joined.result_code);
    result.outcome = joined.outcome;
    result.meeting = joined.meeting;
    result.participant = joined.participant;
    if (joined.has_event) {
        result.events.push_back(joined.event);
    }
    return result;
}

// ------------------------------------------------------------------------------
// API 3：LeaveMeeting（Step 1.4 §15、Step 1.3 §16）
//
// 关键顺序：身份/历史成员判断先于状态暴露。
// ENDING 下的 Leave 与 CLOSED 下的 Leave 语义不同，而与"是否曾经是成员"无关：
//   - 从未参加（含 ENDING）→ NOT_PARTICIPANT（必须先判，不能因为 ENDING 先泄露
//     MEETING_STATE_REJECTED）；
//   - 曾经参加 + ENDING     → MEETING_STATE_REJECTED；
//   - 曾经参加 + CLOSED     → ALREADY_LEFT（幂等，不改快照、不产生事件）。
// ------------------------------------------------------------------------------

LeaveMeetingResult MeetingService::LeaveMeeting(const MeetingRequestContext& context,
                                                const MeetingId& meeting_id) {
    LeaveMeetingResult result;

    ResultCode code = ResultCode::INTERNAL_ERROR;
    if (!ValidateContext(context, code)) {
        result.common = MakeCommon(context.request_id, code);
        return result;
    }
    if (!CheckDomainSessionUsability(context, code)) {
        result.common = MakeCommon(context.request_id, code);
        return result;
    }
    if (!ValidateMeetingIdArgument(meeting_id, code)) {
        result.common = MakeCommon(context.request_id, code);
        return result;
    }

    MeetingAggregate* aggregate = FindActiveMeeting(meeting_id);

    if (aggregate != 0) {
        // 步骤 5：资源范围内的成员身份判断。TryGetParticipantView 同时覆盖
        // "仍是 ACTIVE" 与 "历史 LEFT" 两种已知成员。
        ParticipantView participant;
        const bool known = aggregate->TryGetParticipantView(
            context.authenticated_user_id, participant);
        if (!known) {
            // 授权失败：不填充 MeetingView，避免向非成员泄露会议摘要
            // （member 数 / host_user_id）。Phase 1 未要求在这些失败路径上
            // 返回视图，Step 1.4 §16.4 亦明确"按可见规则返回或不返回"。
            result.common = MakeCommon(context.request_id, ResultCode::NOT_PARTICIPANT);
            return result;
        }

        // 已知成员 + ENDING：成员关系已冻结，一律拒绝。
        if (aggregate->State() == MeetingState::ENDING) {
            result.common = MakeCommon(context.request_id,
                                       ResultCode::MEETING_STATE_REJECTED);
            result.meeting = aggregate->BuildMeetingView();
            return result;
        }

        // CREATED / ACTIVE：交给聚合完成 Host 判定、幂等判定与真实 mutation。
        MutationParams params;        params.now = dependencies_.now();
        params.event_id = dependencies_.next_event_id();
        params.has_device_id = context.has_device_id;
        params.device_id = context.has_device_id ? context.device_id : DeviceId();

        const LeaveResult left = aggregate->Leave(context.authenticated_user_id,
                                                 context.request_id, params);

        // 只有真实完成逻辑离会时才清理反向索引（该 User 的全部 Session refs）。
        if (left.result_code == ResultCode::OK) {
            RemoveBindingRefsForMeetingAndUser(meeting_id,
                                               context.authenticated_user_id);
        }

        result.common = MakeCommon(context.request_id, left.result_code);
        result.outcome = left.outcome;
        result.meeting = left.meeting;
        if (left.has_event) {
            result.events.push_back(left.event);
        }
        return result;
    }

    const ClosedMeetingSnapshot* snapshot = FindClosedSnapshot(meeting_id);
    if (snapshot != 0) {
        if (IsHistoricalParticipant(*snapshot, context.authenticated_user_id)) {
            result.common = MakeCommon(context.request_id, ResultCode::ALREADY_LEFT);
            result.meeting = BuildClosedMeetingView(*snapshot);
        } else {
            // 非历史成员：不得借此枚举 CLOSED 会议摘要。
            result.common = MakeCommon(context.request_id,
                                       ResultCode::NOT_PARTICIPANT);
        }
        return result;
    }

    ServerId owner;
    const ResultCode locality = ResolveLocality(context, meeting_id, owner);
    result.common = MakeCommon(context.request_id, locality);
    if (locality == ResultCode::MEETING_NOT_LOCAL) {
        result.common.has_owner_chat_server_id = true;
        result.common.owner_chat_server_id = owner;
    }
    return result;
}

// ------------------------------------------------------------------------------
// API 4：CloseMeeting（Step 1.4 §16）
//
// 只完成第一个 serialized turn：BeginClose。
// 绝不在这里调用 FinalizeClose，也绝不产生 MeetingClosed。
// 权限必须先于任何幂等结果：非 Host 在 ENDING / CLOSED 下同样是 PERMISSION_DENIED。
// ------------------------------------------------------------------------------

CloseMeetingResult MeetingService::CloseMeeting(const MeetingRequestContext& context,
                                                const MeetingId& meeting_id) {
    CloseMeetingResult result;

    ResultCode code = ResultCode::INTERNAL_ERROR;
    if (!ValidateContext(context, code)) {
        result.common = MakeCommon(context.request_id, code);
        return result;
    }
    if (!CheckDomainSessionUsability(context, code)) {
        result.common = MakeCommon(context.request_id, code);
        return result;
    }
    if (!ValidateMeetingIdArgument(meeting_id, code)) {
        result.common = MakeCommon(context.request_id, code);
        return result;
    }

    MeetingAggregate* aggregate = FindActiveMeeting(meeting_id);

    if (aggregate != 0) {
        // 步骤 5：Close 只允许 Host。
        if (context.authenticated_user_id != aggregate->HostUserId()) {
            // 授权失败：不填充 MeetingView。
            result.common = MakeCommon(context.request_id, ResultCode::PERMISSION_DENIED);
            return result;
        }

        MutationParams params;
        params.now = dependencies_.now();
        params.event_id = dependencies_.next_event_id();
        params.has_device_id = context.has_device_id;
        params.device_id = context.has_device_id ? context.device_id : DeviceId();

        const BeginCloseResult began = aggregate->BeginClose(
            context.authenticated_user_id, context.request_id, params);

        result.common = MakeCommon(context.request_id, began.result_code);
        result.outcome = began.outcome;
        result.meeting = began.meeting;
        // 首次合法 BeginClose 才安排恰好一个 FinalizeClose。
        result.should_schedule_finalize = began.should_schedule_finalize;
        // BeginClose 不产生事件。
        return result;
    }

    const ClosedMeetingSnapshot* snapshot = FindClosedSnapshot(meeting_id);
    if (snapshot != 0) {
        // CLOSED 下的权限判定只依赖快照中的 host_user_id。
        // 历史普通 Participant 也不能得到 ALREADY_CLOSED。
        if (context.authenticated_user_id != snapshot->host_user_id) {
            result.common = MakeCommon(context.request_id, ResultCode::PERMISSION_DENIED);
        } else {
            result.common = MakeCommon(context.request_id, ResultCode::ALREADY_CLOSED);
            result.meeting = BuildClosedMeetingView(*snapshot);
        }
        return result;
    }

    ServerId owner;
    const ResultCode locality = ResolveLocality(context, meeting_id, owner);
    result.common = MakeCommon(context.request_id, locality);
    if (locality == ResultCode::MEETING_NOT_LOCAL) {
        result.common.has_owner_chat_server_id = true;
        result.common.owner_chat_server_id = owner;
    }
    return result;
}

// ------------------------------------------------------------------------------
// API 5：QueryMeeting（Step 1.4 §17）
//
// 完全只读：不改变 Meeting 状态、成员数、Binding、反向索引、事件或快照。
// 开放 Meeting 的授权基于活动聚合；CLOSED 的授权与数据**只**来自快照。
// ------------------------------------------------------------------------------

QueryMeetingResult MeetingService::QueryMeeting(const MeetingRequestContext& context,
                                                const MeetingId& meeting_id) {
    QueryMeetingResult result;

    ResultCode code = ResultCode::INTERNAL_ERROR;
    if (!ValidateContext(context, code)) {
        result.common = MakeCommon(context.request_id, code);
        return result;
    }
    if (!CheckDomainSessionUsability(context, code)) {
        result.common = MakeCommon(context.request_id, code);
        return result;
    }
    if (!ValidateMeetingIdArgument(meeting_id, code)) {
        result.common = MakeCommon(context.request_id, code);
        return result;
    }

    MeetingAggregate* aggregate = FindActiveMeeting(meeting_id);
    if (aggregate != 0) {
        const UserId caller = context.authenticated_user_id;
        if (caller == aggregate->HostUserId()) {
            result.common = MakeCommon(context.request_id, ResultCode::OK);
            result.meeting = aggregate->BuildMeetingView();
            return result;
        }

        ParticipantView participant;
        if (aggregate->TryGetParticipantView(caller, participant) &&
            participant.participant_state == ParticipantState::ACTIVE) {
            result.common = MakeCommon(context.request_id, ResultCode::OK);
            result.meeting = aggregate->BuildMeetingView();
            return result;
        }

        // 从未参加，或曾经参加但当前为 LEFT：开放 Meeting 下都不再可见。
        // 授权失败时不返回 MeetingView，避免泄露私有会议摘要。
        result.common = MakeCommon(context.request_id, ResultCode::NOT_PARTICIPANT);
        return result;
    }

    const ClosedMeetingSnapshot* snapshot = FindClosedSnapshot(meeting_id);
    if (snapshot != 0) {
        if (IsHistoricalParticipant(*snapshot, context.authenticated_user_id)) {
            result.common = MakeCommon(context.request_id, ResultCode::OK);
            result.meeting = BuildClosedMeetingView(*snapshot);
        } else {
            // 未加入过的用户不得借此枚举 CLOSED 会议摘要（Step 1.4 §26.2）。
            result.common = MakeCommon(context.request_id, ResultCode::NOT_PARTICIPANT);
        }
        return result;
    }

    ServerId owner;
    const ResultCode locality = ResolveLocality(context, meeting_id, owner);
    result.common = MakeCommon(context.request_id, locality);
    if (locality == ResultCode::MEETING_NOT_LOCAL) {
        result.common.has_owner_chat_server_id = true;
        result.common.owner_chat_server_id = owner;
    }
    return result;
}

// ------------------------------------------------------------------------------
// API 6：ListParticipants（Step 1.4 §18）
//
// 授权与 QueryMeeting 完全相同；区别只在投影：
//   开放 Meeting → scope CURRENT，只列 ACTIVE 逻辑成员；
//   CLOSED       → scope HISTORICAL，只从快照的历史身份构造。
// ------------------------------------------------------------------------------

ListParticipantsResult MeetingService::ListParticipants(
    const MeetingRequestContext& context, const MeetingId& meeting_id) {
    ListParticipantsResult result;

    ResultCode code = ResultCode::INTERNAL_ERROR;
    if (!ValidateContext(context, code)) {
        result.common = MakeCommon(context.request_id, code);
        return result;
    }
    if (!CheckDomainSessionUsability(context, code)) {
        result.common = MakeCommon(context.request_id, code);
        return result;
    }
    if (!ValidateMeetingIdArgument(meeting_id, code)) {
        result.common = MakeCommon(context.request_id, code);
        return result;
    }

    MeetingAggregate* aggregate = FindActiveMeeting(meeting_id);
    if (aggregate != 0) {
        const UserId caller = context.authenticated_user_id;
        bool authorized = (caller == aggregate->HostUserId());
        if (!authorized) {
            ParticipantView participant;
            authorized = aggregate->TryGetParticipantView(caller, participant) &&
                         participant.participant_state == ParticipantState::ACTIVE;
        }
        if (!authorized) {
            // 授权失败：不返回 MeetingView，也不返回成员列表。
            result.common = MakeCommon(context.request_id, ResultCode::NOT_PARTICIPANT);
            return result;
        }

        result.common = MakeCommon(context.request_id, ResultCode::OK);
        result.meeting = aggregate->BuildMeetingView();
        // CURRENT 投影只包含 ACTIVE 成员；LEFT 的历史成员不出现在列表里。
        result.participants = aggregate->ListCurrentParticipantViews();
        return result;
    }

    const ClosedMeetingSnapshot* snapshot = FindClosedSnapshot(meeting_id);
    if (snapshot != 0) {
        if (!IsHistoricalParticipant(*snapshot, context.authenticated_user_id)) {
            result.common = MakeCommon(context.request_id, ResultCode::NOT_PARTICIPANT);
            return result;
        }

        result.common = MakeCommon(context.request_id, ResultCode::OK);
        result.meeting = BuildClosedMeetingView(*snapshot);
        result.scope = ParticipantViewScope::HISTORICAL;
        result.participants.reserve(snapshot->participant_identities.size());
        for (std::size_t i = 0; i < snapshot->participant_identities.size(); ++i) {
            result.participants.push_back(
                BuildHistoricalParticipantView(snapshot->participant_identities[i]));
        }
        return result;
    }

    ServerId owner;
    const ResultCode locality = ResolveLocality(context, meeting_id, owner);
    result.common = MakeCommon(context.request_id, locality);
    if (locality == ResultCode::MEETING_NOT_LOCAL) {
        result.common.has_owner_chat_server_id = true;
        result.common.owner_chat_server_id = owner;
    }
    return result;
}

// ------------------------------------------------------------------------------
// FinalizeClose（Step 1.3 §17.2/§18.2、Step 1.4 §16.5、Step 1.5 §23）
//
// 这是第二个 serialized turn，**不是外部 API**：没有客户端 Response Contract，
// 也不为客户端设计 Rejected 结果。因此 active 聚合仍处于 CREATED/ACTIVE 时
// 只返回一个内部 not-finalized 结果，而不是 MEETING_STATE_REJECTED。
//
// 代码顺序必须能直接读出：snapshot 发布先于 active 聚合释放。
// ------------------------------------------------------------------------------

FinalizeCloseResult MeetingService::FinalizeClose(const MeetingId& meeting_id) {
    FinalizeCloseResult result;
    result.meeting_id = meeting_id;

    std::map<MeetingId, MeetingAggregate>::iterator it =
        active_meetings_.find(meeting_id);

    if (it == active_meetings_.end()) {
        // active 已释放：若快照已存在，则是重复 FinalizeClose。
        // 不得第二次产生 MeetingClosed，也不得第二次输出通知意图。
        const ClosedMeetingSnapshot* snapshot = FindClosedSnapshot(meeting_id);
        if (snapshot != 0) {
            result.already_finalized = true;
            result.has_snapshot = true;
            result.snapshot = *snapshot;
        }
        return result;
    }

    MeetingAggregate& aggregate = it->second;

    // 1. 只有 ENDING 才允许收敛；CREATED/ACTIVE 收到 FinalizeClose 属于内部
    //    调度错误，不得关闭该 Meeting。
    if (aggregate.State() != MeetingState::ENDING || !aggregate.HasCloseContext()) {
        return result;
    }

    // 2. obtain frozen CloseContext（值拷贝，不持有聚合内部指针）。
    const CloseContext context = aggregate.FrozenCloseContext();

    // 3. cleanup reverse-index Binding refs for this Meeting。
    //    之后任何 SessionDisconnected 都不会再尝试修改这个即将释放的聚合。
    RemoveBindingRefsForMeeting(meeting_id);

    // 4. build complete ClosedMeetingSnapshot（此时 Participant 历史身份仍然完整）。
    const Timestamp finalized_at = dependencies_.now();
    const ClosedMeetingSnapshot snapshot =
        aggregate.BuildClosedSnapshot(context, finalized_at);

    // 5. publish snapshot into closed registry —— 必须先于第 8 步释放 active 聚合，
    //    否则会出现"活动聚合已释放 + Snapshot 尚不存在"的可观测空窗。
    closed_snapshots_[meeting_id] = snapshot;

    // 6. MeetingClosed exactly once。
    MeetingEvent closed_event;
    closed_event.event_id = dependencies_.next_event_id();
    closed_event.event_type = EventType::MEETING_CLOSED;
    closed_event.meeting_id = meeting_id;
    closed_event.has_actor_user_id = true;
    closed_event.actor_user_id = context.host_user_id;
    // MeetingClosed 不强制填写 participant_user_id。
    closed_event.has_participant_user_id = false;
    closed_event.participant_user_id = 0;
    closed_event.owner_chat_server_id = context.owner_chat_server_id;
    closed_event.occurred_at = finalized_at;
    closed_event.meeting_state_after = MeetingState::CLOSED;
    // 与最初那次 Close command 建立 correlation；它仍然不是幂等键。
    closed_event.has_request_id = true;
    closed_event.request_id = context.originating_request_id;
    closed_event.dedupe_context = std::string("MEETING_CLOSED:") + meeting_id;

    // 7. capture notification target session IDs from the frozen CloseContext。
    //    M3 只输出意图，不执行任何发送。
    result.notification_session_ids = context.notifiable_session_ids;

    // 8. release active aggregate。
    active_meetings_.erase(it);

    result.finalized = true;
    result.has_snapshot = true;
    result.snapshot = snapshot;
    result.events.push_back(closed_event);
    return result;
}

// ------------------------------------------------------------------------------
// SessionDisconnected fan-out（Step 1.3 §13.2、Step 1.5 §12）
//
// 一次 serialized mutation 内完成：CLOSING → 逐 Meeting 独立裁决 →
// 清除反向索引 → CLOSED。Session 只有在全部受影响 Meeting 清理完成后才能
// 进入 CLOSED。
// ------------------------------------------------------------------------------

SessionDisconnectedResult MeetingService::SessionDisconnected(
    const SessionId& session_id) {
    SessionDisconnectedResult result;
    result.final_state = DomainSessionState::CLOSED;

    std::map<SessionId, DomainSessionRecord>::iterator it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        // 未知 Session：没有领域记录可变更，也没有 Binding 可清理。
        // 这仍然是一次幂等调用。
        result.session_known = false;
        return result;
    }

    DomainSessionRecord& record = it->second;
    result.session_known = true;

    if (record.state == DomainSessionState::CLOSED) {
        // CLOSED 是终态（CLOSED → * 非法）：重复投递整体幂等。
        result.already_closed = true;
        return result;
    }

    // 1. AUTHENTICATED（或 CONNECTED）→ CLOSING。
    record.state = DomainSessionState::CLOSING;

    // 2. copy 该 session 当前关联的全部 BOUND Binding refs。
    //    必须先复制：后面的注册表操作不得让遍历失效，且遍历期间不得依赖
    //    中间态（只允许出现在本 mutation 内部，对外不可观测）。
    std::vector<BindingRef> affected;
    std::map<SessionId, std::vector<BindingRef> >::const_iterator refs_it =
        reverse_index_.find(session_id);
    if (refs_it != reverse_index_.end()) {
        affected = refs_it->second;
    }

    // 3. for each affected Meeting（各 Meeting 独立裁决）。
    const Timestamp at = dependencies_.now();
    for (std::size_t i = 0; i < affected.size(); ++i) {
        std::map<MeetingId, MeetingAggregate>::iterator meeting_it =
            active_meetings_.find(affected[i].meeting_id);
        if (meeting_it == active_meetings_.end()) {
            // 该 Meeting 已 CLOSED 并释放：绝不再触碰已释放的聚合。
            continue;
        }

        MutationParams params;
        params.now = at;
        // 注意：该 event_id 会在每次参与本 Meeting 的 disconnect 时由注入来源
        // 消耗一个值，即使本次裁决最终不产生事件（例如 Host 断线或 ENDING）。
        // 事件身份本身始终由注入来源决定，测试通过记录生成序列来断言。
        params.event_id = dependencies_.next_event_id();

        const DisconnectSessionResult disconnected = meeting_it->second.DisconnectSession(
            affected[i].user_id, session_id, params);
        if (disconnected.has_event) {
            result.events.push_back(disconnected.event);
        }
    }
    result.affected_meeting_count = affected.size();

    // 4. 清除该 session 的反向索引。
    RemoveBindingRefsForSession(session_id);

    // 5. 只有全部受影响 Meeting 处理完成后，Domain Session 才 → CLOSED。
    record.state = DomainSessionState::CLOSED;
    result.final_state = DomainSessionState::CLOSED;
    return result;
}

} // namespace meeting
