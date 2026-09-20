// ==============================================================================
// M2 MeetingAggregate 实现。
//
// 这里的规则全部来自已提交的 Phase 1 文档：
//   - Step 1.3 §5/§7/§9/§11  状态值
//   - Step 1.3 §8            CREATED → ACTIVE 的精确触发
//   - Step 1.3 §15/§16       Join 幂等与 Leave 语义
//   - Step 1.3 §17           BeginClose / FinalizeClose 切分
//   - Step 1.4 §10/§11/§12/§13/§14/§15/§16  ResultCode / View / API 语义
//   - Step 1.5 §6–§14        核心时序与关键断言
//
// 本文件不引入任何并发、时间获取或 ID 生成：这些都属于调用方职责。
// ==============================================================================

#include "meeting/MeetingAggregate.h"

namespace meeting {

// ------------------------------------------------------------------------------
// 内部查找辅助
// ------------------------------------------------------------------------------

MeetingAggregate::ParticipantRecord* MeetingAggregate::FindParticipant(UserId user_id) {
    std::map<UserId, ParticipantRecord>::iterator it = participants_.find(user_id);
    return it == participants_.end() ? nullptr : &it->second;
}

const MeetingAggregate::ParticipantRecord* MeetingAggregate::FindParticipant(
    UserId user_id) const {
    std::map<UserId, ParticipantRecord>::const_iterator it = participants_.find(user_id);
    return it == participants_.end() ? nullptr : &it->second;
}

MeetingAggregate::BindingRecord* MeetingAggregate::FindBoundBinding(
    ParticipantRecord& participant, const SessionId& session_id) {
    for (std::size_t i = 0; i < participant.bindings.size(); ++i) {
        BindingRecord& binding = participant.bindings[i];
        if (binding.binding_state == BindingState::BOUND &&
            binding.session_id == session_id) {
            return &binding;
        }
    }
    return nullptr;
}

const MeetingAggregate::BindingRecord* MeetingAggregate::FindBoundBinding(
    const ParticipantRecord& participant, const SessionId& session_id) {
    for (std::size_t i = 0; i < participant.bindings.size(); ++i) {
        const BindingRecord& binding = participant.bindings[i];
        if (binding.binding_state == BindingState::BOUND &&
            binding.session_id == session_id) {
            return &binding;
        }
    }
    return nullptr;
}

void MeetingAggregate::BindSession(ParticipantRecord& participant,
                                  const SessionId& session_id, const Timestamp& now,
                                  bool has_device_id, const DeviceId& device_id) {
    BindingRecord* existing = FindBoundBinding(participant, session_id);
    if (existing != nullptr) {
        // 同一 session 重复绑定不是新关系：只刷新可选的 device 信息。
        // 若调用方未提供 device_id，则不得伪造，也不要抹掉已有信息。
        if (has_device_id) {
            existing->has_device_id = true;
            existing->device_id = device_id;
        }
        return;
    }

    // 复用该 session 的历史 UNBOUND 记录（重新绑定），而不是不断堆叠记录。
    // 这保证"同一 Participant + session 最多一条 BOUND Binding"（I8）。
    for (std::size_t i = 0; i < participant.bindings.size(); ++i) {
        BindingRecord& binding = participant.bindings[i];
        if (binding.binding_state == BindingState::UNBOUND &&
            binding.session_id == session_id) {
            binding.binding_state = BindingState::BOUND;
            binding.bound_at = now;
            binding.has_device_id = has_device_id;
            binding.device_id = has_device_id ? device_id : DeviceId();
            return;
        }
    }

    BindingRecord created;
    created.session_id = session_id;
    created.binding_state = BindingState::BOUND;
    created.bound_at = now;
    created.has_device_id = has_device_id;
    created.device_id = has_device_id ? device_id : DeviceId();
    participant.bindings.push_back(created);
}

namespace {

// 把 UserId 渲染成 dedupe_context 里的稳定片段。UserId 的内部表示是一个实现
// 细节，这里不做零填充或格式化承诺，只是保证同一输入产生同一字符串。
std::string ToDedupeUserId(UserId user_id) {
    return std::to_string(user_id);
}

} // namespace

ParticipantView MeetingAggregate::MakeParticipantView(const ParticipantRecord& p) {
    ParticipantView view;
    view.user_id = p.user_id;
    view.role = p.role;
    view.participant_state = p.participant_state;
    view.joined_at = p.joined_at;
    // ACTIVE 时 left_at 必须为空；只有 LEFT 才携带离会时间。
    view.has_left_at = (p.participant_state == ParticipantState::LEFT);
    view.left_at = view.has_left_at ? p.left_at : Timestamp();
    return view;
}

ParticipantIdentitySnapshot MeetingAggregate::MakeIdentitySnapshot(
    const ParticipantRecord& p) {
    ParticipantIdentitySnapshot snapshot;
    snapshot.user_id = p.user_id;
    snapshot.role = p.role;
    snapshot.joined_at = p.joined_at;
    snapshot.has_left_at = (p.participant_state == ParticipantState::LEFT);
    snapshot.left_at = snapshot.has_left_at ? p.left_at : Timestamp();
    return snapshot;
}

MeetingEvent MeetingAggregate::MakeEvent(EventType type, const EventId& event_id,
                                         const Timestamp& occurred_at, bool has_actor,
                                         UserId actor_user_id, bool has_participant,
                                         UserId participant_user_id,
                                         MeetingState meeting_state_after,
                                         const RequestId& request_id,
                                         const std::string& dedupe_context) const {
    MeetingEvent event;
    event.event_id = event_id;
    event.event_type = type;
    event.meeting_id = meeting_id_;
    event.has_actor_user_id = has_actor;
    event.actor_user_id = has_actor ? actor_user_id : 0;
    event.has_participant_user_id = has_participant;
    event.participant_user_id = has_participant ? participant_user_id : 0;
    event.owner_chat_server_id = owner_chat_server_id_;
    event.occurred_at = occurred_at;
    // 事件产生后的稳定状态，由调用点显式给出。
    event.meeting_state_after = meeting_state_after;
    // M2.1 中聚合产生的**全部**事件都由外部命令触发（Create / Join / Leave），
    // 因此这里恒为 true，绝不存在"该事件没有发起命令"的情况。
    //
    // M3 引入内部触发事件（SessionDisconnected 的 ParticipantLeft fan-out）时，
    // **必须**把本形参扩展为 (bool has_request_id, const RequestId&)，而不是为
    // 内部事件伪造一个客户端 request_id —— 那会让 correlation 语义失真。
    event.has_request_id = true;
    event.request_id = request_id;
    event.dedupe_context = dedupe_context;
    return event;
}

// ------------------------------------------------------------------------------
// 构造：初始聚合
// ------------------------------------------------------------------------------

// 私有状态初始化构造。所有正常创建路径都必须经过 Create() factory，
// 从而必然同时得到一个 MeetingCreated value。
MeetingAggregate::MeetingAggregate(const CreateMeetingParams& params) {
    meeting_id_ = params.meeting_id;
    host_user_id_ = params.host_user_id;
    owner_chat_server_id_ = params.owner_chat_server_id;
    created_at_ = params.created_at;
    meeting_state_ = MeetingState::CREATED;

    // Host 是 Participant，且创建成功时即为 ACTIVE；但 Host 的建立**不构成
    // "加入"，因此 Meeting 停留在 CREATED（Step 1.3 §8.2）。
    ParticipantRecord host;
    host.user_id = params.host_user_id;
    host.role = ParticipantRole::HOST;
    host.participant_state = ParticipantState::ACTIVE;
    host.joined_at = params.created_at;
    host.has_left_at = false;
    participants_[host.user_id] = host;
    participant_order_.push_back(host.user_id);

    // Host 初始 Session Binding 为 BOUND。Host 不占用"非 Host 激活"名额。
    BindSession(participants_[params.host_user_id], params.host_session_id,
                params.created_at, params.has_host_device_id, params.host_device_id);

    active_participant_count_ = 1;
    has_activated_non_host_ = false;
    has_close_context_ = false;
}

MeetingAggregate MeetingAggregate::Create(const CreateMeetingParams& params,
                                          MeetingEvent& created_event) {
    MeetingAggregate aggregate(params);

    // MeetingCreated 的最低语义（Step 1.4 §22.2）：actor 为 Host、meeting_state_after
    // 为 CREATED、不填 participant_user_id。
    // "恰好一次"由 factory 的结构保证：一次合法的 Create domain operation 返回
    // 一个聚合 + 一个事件值，调用方无法忘记生成，也无法重复生成。
    created_event = aggregate.MakeEvent(
        EventType::MEETING_CREATED, params.event_id, params.created_at,
        /*has_actor=*/true, params.host_user_id,
        /*has_participant=*/false, 0, MeetingState::CREATED, params.request_id,
        std::string("MEETING_CREATED:") + params.meeting_id);
    return aggregate;
}

// ------------------------------------------------------------------------------
// 只读投影
// ------------------------------------------------------------------------------

MeetingView MeetingAggregate::BuildMeetingView() const {
    MeetingView view;
    view.meeting_id = meeting_id_;
    view.owner_chat_server_id = owner_chat_server_id_;
    view.host_user_id = host_user_id_;
    view.meeting_state = meeting_state_;
    view.active_participant_count = active_participant_count_;
    view.created_at = created_at_;
    // M2 不进入 CLOSED 生命周期（ENDING → CLOSED 的 registry 发布属于 M3），
    // 因此此处 closed_at 始终为空。
    view.has_closed_at = false;
    view.closed_at = Timestamp();
    return view;
}

bool MeetingAggregate::TryGetParticipantView(UserId user_id,
                                             ParticipantView& out) const {
    const ParticipantRecord* participant = FindParticipant(user_id);
    if (participant == nullptr) {
        return false;
    }
    out = MakeParticipantView(*participant);
    return true;
}

std::vector<ParticipantView> MeetingAggregate::ListCurrentParticipantViews() const {
    std::vector<ParticipantView> views;
    views.reserve(participant_order_.size());
    for (std::size_t i = 0; i < participant_order_.size(); ++i) {
        const ParticipantRecord* participant = FindParticipant(participant_order_[i]);
        // CURRENT 投影只列活动逻辑成员（Step 1.4 §18.4）。
        // LEFT 的历史成员仍保留在 participants_ 中，但不进入该列表。
        if (participant != nullptr &&
            participant->participant_state == ParticipantState::ACTIVE) {
            views.push_back(MakeParticipantView(*participant));
        }
    }
    return views;
}

bool MeetingAggregate::IsSessionBound(UserId user_id,
                                      const SessionId& session_id) const {
    const ParticipantRecord* participant = FindParticipant(user_id);
    if (participant == nullptr) {
        return false;
    }
    return FindBoundBinding(*participant, session_id) != nullptr;
}

std::size_t MeetingAggregate::BoundSessionCount(UserId user_id) const {
    const ParticipantRecord* participant = FindParticipant(user_id);
    if (participant == nullptr) {
        return 0;
    }
    std::size_t count = 0;
    for (std::size_t i = 0; i < participant->bindings.size(); ++i) {
        if (participant->bindings[i].binding_state == BindingState::BOUND) {
            ++count;
        }
    }
    return count;
}

// ------------------------------------------------------------------------------
// Join
//
// 求值顺序遵循 Step 1.5 §7：先做 state gate，再判幂等，最后才 mutate。
// ENDING 一律拒绝（成员关系已冻结）。
// ------------------------------------------------------------------------------

JoinResult MeetingAggregate::Join(UserId user_id, const SessionId& session_id,
                                  const RequestId& request_id,
                                  const MutationParams& params) {
    JoinResult result;
    result.meeting = BuildMeetingView();

    ParticipantView participant_view;
    if (TryGetParticipantView(user_id, participant_view)) {
        result.participant = participant_view;
    }

    // state gate：ENDING 不接受任何新成员关系。CLOSED 的 aggregate lifecycle
    // 由 M3 负责，本聚合不进入该状态。
    if (meeting_state_ == MeetingState::ENDING ||
        meeting_state_ == MeetingState::CLOSED) {
        result.result_code = ResultCode::MEETING_STATE_REJECTED;
        result.disposition = ResultDisposition::ERROR;
        result.has_event = false;
        return result;
    }

    ParticipantRecord* existing = FindParticipant(user_id);

    // ---------------------------------------------------------------------------
    // Case B / Duplicate Join：该 session 已有 BOUND Binding → 完全幂等。
    // 幂等来自**领域状态**，不是 request_id 去重。
    // ---------------------------------------------------------------------------
    if (existing != nullptr &&
        FindBoundBinding(*existing, session_id) != nullptr) {
        result.result_code = ResultCode::ALREADY_JOINED;
        result.disposition = ResultDisposition::IDEMPOTENT;
        result.outcome = JoinOutcome::ALREADY_BOUND;
        result.participant = MakeParticipantView(*existing);
        result.has_event = false;
        return result;
    }

    // ---------------------------------------------------------------------------
    // Case C：Participant 已 ACTIVE，但这是一个新的 session → 附加绑定。
    // 逻辑成员数不变，不产生 ParticipantJoined。
    // ---------------------------------------------------------------------------
    if (existing != nullptr &&
        existing->participant_state == ParticipantState::ACTIVE) {
        BindSession(*existing, session_id, params.now, params.has_device_id,
                    params.device_id);
        result.result_code = ResultCode::OK;
        result.disposition = ResultDisposition::SUCCESS;
        result.outcome = JoinOutcome::ADDITIONAL_SESSION_BOUND;
        result.participant = MakeParticipantView(*existing);
        result.meeting = BuildMeetingView();
        result.has_event = false;
        return result;
    }

    // ---------------------------------------------------------------------------
    // Case A：首次逻辑加入，或历史 LEFT 成员 Re-Join。
    //
    // 两种情况都在同一个 mutation 内完成 Participant 激活与（仅 Case A 首次
    // 非 Host 激活时的）CREATED → ACTIVE 转换。
    // ---------------------------------------------------------------------------
    const bool is_rejoin = (existing != nullptr);
    const bool is_non_host = (user_id != host_user_id_);

    if (is_rejoin) {
        // Re-Join：joined_at 保持"首次逻辑加入"语义，不重置。
        existing->participant_state = ParticipantState::ACTIVE;
        existing->has_left_at = false;
        existing->left_at = Timestamp();
        BindSession(*existing, session_id, params.now, params.has_device_id,
                    params.device_id);
    } else {
        ParticipantRecord created;
        created.user_id = user_id;
        // 只有 host_user_id_ 才是 Host；其余一律是普通成员。
        created.role = (user_id == host_user_id_) ? ParticipantRole::HOST
                                                 : ParticipantRole::PARTICIPANT;
        created.participant_state = ParticipantState::ACTIVE;
        created.joined_at = params.now;
        created.has_left_at = false;
        BindSession(created, session_id, params.now, params.has_device_id,
                    params.device_id);
        participants_[user_id] = created;
        participant_order_.push_back(user_id);
        existing = &participants_[user_id];
    }

    ++active_participant_count_;

    // CREATED → ACTIVE 只由"第一个非 Host 成员的真实逻辑激活"触发，
    // 且不可回退（Step 1.3 §8.1、§8.5）。
    if (is_non_host && !has_activated_non_host_) {
        has_activated_non_host_ = true;
        if (meeting_state_ == MeetingState::CREATED) {
            meeting_state_ = MeetingState::ACTIVE;
        }
    }

    result.result_code = ResultCode::OK;
    result.disposition = ResultDisposition::SUCCESS;
    result.outcome = is_rejoin ? JoinOutcome::REJOINED_PARTICIPANT
                               : JoinOutcome::NEW_PARTICIPANT;
    result.participant = MakeParticipantView(*existing);
    result.meeting = BuildMeetingView();
    // NEW_PARTICIPANT 与 REJOINED_PARTICIPANT 各自恰好产生一次
    // ParticipantJoined，且使用调用方提供的新 event_id。
    result.has_event = true;
    // ParticipantJoined 的 meeting_state_after 在 Phase 1 中**固定为 ACTIVE**
    // （Step 1.4 §23.2）：该事件只能由 NEW_PARTICIPANT / REJOINED_PARTICIPANT 产生，
    // 而首个非 Host 激活与 CREATED → ACTIVE 在同一个 mutation 内完成，后续 Join
    // 与 Re-Join 也必然发生在已 ACTIVE 的 Meeting 上。
    //
    // 这里刻意使用**当前真实状态**而不是硬编码常量：两者在契约上恒等，用真实状态
    // 可以让"若将来状态机被改坏"直接反映到事件里，而不是被常量掩盖。
    //
    // dedupe_context 按 Phase 1 定义携带"逻辑动作 + meeting_id + user_id"这一最小
    // 去重上下文；它不是幂等键，也不规定去重算法。
    result.event = MakeEvent(EventType::PARTICIPANT_JOINED, params.event_id, params.now,
                             true, user_id, true, user_id, meeting_state_, request_id,
                             std::string("PARTICIPANT_JOINED:") + meeting_id_ + ":" +
                                 ToDedupeUserId(user_id));
    return result;
}

// ------------------------------------------------------------------------------
// Leave
//
// Leave 是 **User 级**离会：解除该 User 在本 Meeting 下全部 Binding。
// Host 对开放会议不得用 Leave 退出，必须走 Close。
// ------------------------------------------------------------------------------

LeaveResult MeetingAggregate::Leave(UserId caller_user_id,
                                    const RequestId& request_id,
                                    const MutationParams& params) {
    LeaveResult result;
    result.meeting = BuildMeetingView();

    // state gate：ENDING 下成员关系已冻结，外部 Leave 一律拒绝（含 Host）。
    // 内部 close cleanup 不是 Leave，不在此路径。
    if (meeting_state_ == MeetingState::ENDING ||
        meeting_state_ == MeetingState::CLOSED) {
        result.result_code = ResultCode::MEETING_STATE_REJECTED;
        result.disposition = ResultDisposition::ERROR;
        result.has_event = false;
        return result;
    }

    ParticipantRecord* participant = FindParticipant(caller_user_id);

    // 从未成为该 Meeting 成员。
    if (participant == nullptr) {
        result.result_code = ResultCode::NOT_PARTICIPANT;
        result.disposition = ResultDisposition::ERROR;
        result.has_event = false;
        return result;
    }

    // Host 在 CREATED/ACTIVE 下必须用 Close，不能 Leave。
    // 不得解绑、不得减成员数、不得产生事件。
    if (participant->role == ParticipantRole::HOST) {
        result.result_code = ResultCode::HOST_MUST_CLOSE_MEETING;
        result.disposition = ResultDisposition::ERROR;
        result.has_event = false;
        return result;
    }

    // duplicate Leave：已知历史成员当前已 LEFT。无 mutation、无事件。
    if (participant->participant_state == ParticipantState::LEFT) {
        result.result_code = ResultCode::ALREADY_LEFT;
        result.disposition = ResultDisposition::IDEMPOTENT;
        result.has_event = false;
        return result;
    }

    // 成功 Leave。
    participant->participant_state = ParticipantState::LEFT;
    participant->has_left_at = true;
    participant->left_at = params.now;
    for (std::size_t i = 0; i < participant->bindings.size(); ++i) {
        participant->bindings[i].binding_state = BindingState::UNBOUND;
    }
    --active_participant_count_;

    result.result_code = ResultCode::OK;
    result.disposition = ResultDisposition::SUCCESS;
    result.outcome = LeaveOutcome::LEFT;
    result.meeting = BuildMeetingView();
    result.has_event = true;
    // ParticipantLeft 的 meeting_state_after 是**mutation 完成后的当前 Meeting 状态**
    // （Step 1.4 §21.3）：成员离会不得使 Meeting 状态回退，因此这里用真实状态而
    // 不是硬编码 CREATED/ACTIVE。
    result.event = MakeEvent(EventType::PARTICIPANT_LEFT, params.event_id, params.now,
                             true, caller_user_id, true, caller_user_id, meeting_state_,
                             request_id,
                             std::string("PARTICIPANT_LEFT:") + meeting_id_ + ":" +
                                 ToDedupeUserId(caller_user_id));
    return result;
}

// ------------------------------------------------------------------------------
// BeginClose
//
// 求值顺序必须是 **permission before idempotency/state exposure**：
// 非 Host 在 CREATED/ACTIVE/ENDING 下都返回 PERMISSION_DENIED，绝不能让他
// 通过 CLOSE_IN_PROGRESS 推断出 Host-only 的内部状态。
// ------------------------------------------------------------------------------

BeginCloseResult MeetingAggregate::BeginClose(UserId caller_user_id,
                                              const RequestId& request_id,
                                              const MutationParams& params) {
    BeginCloseResult result;
    result.meeting = BuildMeetingView();
    result.should_schedule_finalize = false;
    result.has_close_context = false;

    // 1) 权限优先。
    if (caller_user_id != host_user_id_) {
        result.result_code = ResultCode::PERMISSION_DENIED;
        result.disposition = ResultDisposition::ERROR;
        return result;
    }

    // 2) Host 且已 ENDING：幂等，不冻结第二个 context、不排第二个 FinalizeClose。
    if (meeting_state_ == MeetingState::ENDING) {
        result.result_code = ResultCode::CLOSE_IN_PROGRESS;
        result.disposition = ResultDisposition::IDEMPOTENT;
        return result;
    }

    // 3) Host 且 CLOSED：终态。注意此处不改 closed_at 呈现（M3 才进入 CLOSED）。
    if (meeting_state_ == MeetingState::CLOSED) {
        result.result_code = ResultCode::ALREADY_CLOSED;
        result.disposition = ResultDisposition::IDEMPOTENT;
        result.meeting = BuildMeetingView();
        return result;
    }

    // 4) Host 首次在 CREATED/ACTIVE 下 Close：冻结上下文并进入 ENDING。
    //    这里**不执行** FinalizeClose；响应仍反映 ENDING。
    meeting_state_ = MeetingState::ENDING;

    close_context_ = CloseContext();
    close_context_.meeting_id = meeting_id_;
    close_context_.host_user_id = host_user_id_;
    close_context_.owner_chat_server_id = owner_chat_server_id_;
    close_context_.originating_request_id = request_id;
    close_context_.close_started_at = params.now;
    close_context_.frozen_active_participant_count = active_participant_count_;

    // 冻结**完整**历史成员身份（含已 LEFT 的历史成员），并只收集仍 BOUND 的
    // session 作为可通知目标。
    for (std::size_t i = 0; i < participant_order_.size(); ++i) {
        const ParticipantRecord* participant = FindParticipant(participant_order_[i]);
        if (participant == nullptr) {
            continue;
        }
        close_context_.participant_identities.push_back(
            MakeIdentitySnapshot(*participant));
        for (std::size_t j = 0; j < participant->bindings.size(); ++j) {
            if (participant->bindings[j].binding_state == BindingState::BOUND) {
                close_context_.notifiable_session_ids.push_back(
                    participant->bindings[j].session_id);
            }
        }
    }
    has_close_context_ = true;

    result.result_code = ResultCode::OK;
    result.disposition = ResultDisposition::SUCCESS;
    result.outcome = CloseOutcome::CLOSE_STARTED;
    result.meeting = BuildMeetingView();
    // 恰好安排一个 FinalizeClose（M3 据此入队）；本方法不执行也不产生
    // MeetingClosed。
    result.should_schedule_finalize = true;
    result.has_close_context = true;
    result.close_context = close_context_;
    return result;
}

// ------------------------------------------------------------------------------
// Snapshot material
//
// 只根据**冻结尾边界**构造 ClosedMeetingSnapshot；不读取当前活动状态、
// 不发布 snapshot、不改变本聚合。
// ------------------------------------------------------------------------------

ClosedMeetingSnapshot MeetingAggregate::BuildClosedSnapshot(
    const CloseContext& context, const Timestamp& closed_at) const {
    ClosedMeetingSnapshot snapshot;
    snapshot.meeting_id = context.meeting_id;
    snapshot.host_user_id = context.host_user_id;
    snapshot.owner_chat_server_id = context.owner_chat_server_id;
    snapshot.meeting_state = MeetingState::CLOSED;
    // created_at 保持原值。
    snapshot.created_at = created_at_;
    // closed_at 由调用方提供（M2 不在聚合内取时间）。
    snapshot.closed_at = closed_at;
    snapshot.participant_identities = context.participant_identities;
    // final_participant_count = 冻结边界上的 ACTIVE 成员数。
    // 它**不等于** participant_identities.size()：后者含已 LEFT 的历史成员。
    snapshot.final_participant_count = context.frozen_active_participant_count;
    return snapshot;
}

} // namespace meeting
