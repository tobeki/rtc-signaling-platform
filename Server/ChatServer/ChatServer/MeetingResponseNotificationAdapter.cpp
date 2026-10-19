// ==============================================================================
// M6.4：Meeting Response / Notification Adapter 实现。
//
// 规范来源：docs/phase2/M6-response-notification-adapter.md
//   §6.3   response target 来自可信 context.session_id
//   §6.4   七项 On* 的签名
//   §7.1.2 OnFinalizeClose 的**逐目标**算法（步骤 0 / 1 / 2a / 2b / 2c）
//   §7.1.3 计数守恒
//   §9     best-effort：失败只能导致"少产出一条消息"
//   §10    MeetingView 安全边界与编码矩阵
//   §13    三层错误分离、ResultCode 不改写、未识别 code 的降级
//
// 本文件刻意**不**包含任何：
//   - 领域 mutation 调用（不持有 MeetingService）；
//   - 网络发送（不 include CSession.h / asio / Boost，不调用 Send）；
//   - 队列、线程、timer、重试、ACK、批量合并。
// ==============================================================================

#include "MeetingResponseNotificationAdapter.h"

#include "LogicWorkItem.h"            // ClientMeetingCommandKind（复用既有枚举）
#include "MeetingOutboundMessage.h"   // typed 出站值对象

#include <cstddef>
#include <string>
#include <vector>

namespace {

// ------------------------------------------------------------------------------
// §13.2 第 4 条：ResultCodeName() 对未识别取值的**唯一**哨兵字符串
// （meeting/MeetingTypes.h:155 的 fallback）。
//
// 刻意不复制 "ResultCode → 字符串" 的第二张映射表：名字永远来自 ResultCodeName()，
// 这里只是识别它自己声明的"未识别"结果。
// ------------------------------------------------------------------------------
const char* const kUnknownResultName = "UNKNOWN_RESULT_CODE";

// ------------------------------------------------------------------------------
// Response 公共字段的规范化结果。
// ------------------------------------------------------------------------------
struct NormalizedResult {
    meeting::ResultCode code;
    std::string name;
    meeting::ResultDisposition disposition;
    bool encoding_failed;
};

// ------------------------------------------------------------------------------
// 规范化领域结果码（§13.2）：
//   (1) 绝不把一个 ResultCode 改成另一个 —— 唯一的例外是 (4) 的显式降级；
//   (3) result_name 只来自 ResultCodeName()；
//   (4) 未识别取值 ⇒ 降级为 INTERNAL_ERROR 并计入 encoding_failed，
//       **不**抛异常、**不**静默产出空响应（response 照常产出，只是码被降级）。
// ------------------------------------------------------------------------------
NormalizedResult NormalizeResult(const meeting::MeetingServiceResultCommon& common) {
    NormalizedResult normalized;
    normalized.code = common.result_code;
    normalized.name = meeting::ResultCodeName(common.result_code);
    normalized.disposition = common.disposition;
    normalized.encoding_failed = (normalized.name == kUnknownResultName);

    if (normalized.encoding_failed) {
        normalized.code = meeting::ResultCode::INTERNAL_ERROR;
        normalized.name = meeting::ResultCodeName(meeting::ResultCode::INTERNAL_ERROR);
        normalized.disposition = meeting::ResultDisposition::ERROR;
    }
    return normalized;
}

// ------------------------------------------------------------------------------
// 六项 Response 的公共头字段（§6.3 / §10.3 / §13.2）。
//
// 出站消息先整体值初始化（所有 enum / bool / presence flag 归零），再逐字段显式
// 赋值：不存在"未初始化的 presence flag 被后续读取"的空间。
// ------------------------------------------------------------------------------
NormalizedResult FillResponseHeader(MeetingResponseMessage& message,
                                    ClientMeetingCommandKind command,
                                    const meeting::MeetingRequestContext& context,
                                    const meeting::MeetingServiceResultCommon& common) {
    const NormalizedResult normalized = NormalizeResult(common);

    message.command = command;
    // correlation 只来自可信 context（与传给 MeetingService 的是同一个值，§6.3）；
    // request_id 不是幂等键，也不参与任何可见性判断。
    message.request_id = context.request_id;
    // response target = 发起该命令的会话；成功与失败路径使用**同一条**来源。
    message.response_target_session_id = context.session_id;

    message.result_code = normalized.code;
    message.result_name = normalized.name;
    message.disposition = normalized.disposition;

    // §10.3：MEETING_NOT_LOCAL 是**唯一**允许携带异地 owner 的结果。
    // 其余结果即便领域意外置位也不编码（不泄露拓扑）。
    message.has_owner_chat_server_id = false;
    if (normalized.code == meeting::ResultCode::MEETING_NOT_LOCAL &&
        common.has_owner_chat_server_id) {
        message.has_owner_chat_server_id = true;
        message.owner_chat_server_id = common.owner_chat_server_id;
    }

    message.has_view = false;
    message.has_participants = false;
    message.has_participant_scope = false;
    return normalized;
}

// ------------------------------------------------------------------------------
// §10.3 编码矩阵的**唯一**实现。
//
// 两条判据必须同时成立，缺一不可：
//   (a) 授权等价类：只有 SUCCESS / IDEMPOTENT 才允许出现 MeetingView
//       （ResultDisposition 由领域 ResultDispositionFor() 计算，这里不复制该表）；
//   (b) 真实填充：领域确实填了这份 view（用 meeting_id 非空作为存在性判据）。
//
// 为什么 (a) 必须显式判：领域在若干 ERROR 路径上也会留下**非空**的 MeetingView
//   （MeetingAggregate::Join / Leave / BeginClose 在函数入口就 BuildMeetingView()，
//    MeetingService 又把这些字段原样透传），例如
//     · HOST_MUST_CLOSE_MEETING（host 调 Leave）
//     · MEETING_STATE_REJECTED（ENDING 下的 Join / Leave）
//   若只看 (b)，这些 ERROR 结果就会把"会议摘要"（主持人、成员数、状态）编码给
//   未授权调用方 —— 直接违反 §10.1 / §10.3。
//
// 为什么 (b) 必须显式判：不得因为"结果结构体存在"就宣称视图有效（§10.2）。
//
// 安全性：这里**不读取任何敏感字段**，整块拷贝都在 has_view == true 之后发生
//   （§10.5 的静态核对因此在本文件里对"主持人 / 成员数 / 会议状态"三个字段
//    的真实**代码**读取为 0 命中：本层只做整块透传，从不逐字段取值）。
//   唯一被读取的 view 字段是 meeting_id，它属于 §10.3 明确允许出现在失败响应中
//   的标识（"该 id 由请求携带，非泄露"）。
// ------------------------------------------------------------------------------
void FillViewIfAuthorized(MeetingResponseMessage& message,
                          const NormalizedResult& normalized,
                          const meeting::MeetingView& domain_view) {
    if (normalized.disposition == meeting::ResultDisposition::ERROR) {
        return;   // §10.3 的 ERROR 行：has_view 必须为 false
    }
    if (domain_view.meeting_id.empty()) {
        return;   // 领域未填充 ⇒ 不编造、不补齐
    }
    message.has_view = true;
    message.view = domain_view;   // 整块透传，不在本层增删字段
}

// ------------------------------------------------------------------------------
// §10.4 / MV6：ListParticipants 的 participants 与 scope 只在**授权通过**的结果上
// 存在；授权失败的等价类与 view 相同（ERROR ⇒ 不编码），此处也不额外填充。
// ------------------------------------------------------------------------------
void FillParticipantsIfAuthorized(MeetingResponseMessage& message,
                                  const NormalizedResult& normalized,
                                  const meeting::ListParticipantsResult& result) {
    if (normalized.disposition == meeting::ResultDisposition::ERROR) {
        return;
    }
    message.has_participants = true;
    message.participants = result.participants;
    message.has_participant_scope = true;
    message.participant_scope = result.scope;   // CURRENT / HISTORICAL 照实透传
}

// ------------------------------------------------------------------------------
// §7.1.2 步骤 2b：通知的 event 事实来源。
//
// FinalizeCloseResult **不**直接暴露 CloseContext，因此 event_id 与
// originating_request_id 的唯一可信来源是 result.events 中的 MEETING_CLOSED 事件
// （MeetingService.cpp:927-941 把 CloseContext.originating_request_id 写入该事件的
// has_request_id / request_id）。这里只读该事件，不伪造、不推断、不改领域：
//   · 事件缺失 / event_id 为空 ⇒ has_event_id = false；
//   · has_request_id 为 false 或 request_id 为空 ⇒ has_correlation_request_id = false。
// ------------------------------------------------------------------------------
struct ClosedEventFacts {
    bool has_event_id;
    meeting::EventId event_id;
    bool has_correlation_request_id;
    meeting::RequestId correlation_request_id;
};

ClosedEventFacts ExtractClosedEventFacts(const std::vector<meeting::MeetingEvent>& events) {
    ClosedEventFacts facts;
    facts.has_event_id = false;
    facts.has_correlation_request_id = false;

    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].event_type != meeting::EventType::MEETING_CLOSED) {
            continue;
        }
        facts.has_event_id = !events[i].event_id.empty();
        facts.event_id = events[i].event_id;
        facts.has_correlation_request_id =
            events[i].has_request_id && !events[i].request_id.empty();
        if (facts.has_correlation_request_id) {
            facts.correlation_request_id = events[i].request_id;
        }
        break;   // 领域保证 MEETING_CLOSED 恰好一次；取到即停
    }
    return facts;
}

}  // namespace

// ------------------------------------------------------------------------------
// 构造 / 析构
// ------------------------------------------------------------------------------

MeetingResponseNotificationAdapter::MeetingResponseNotificationAdapter(
    IMeetingOutboundMessageCollector& collector,
    std::weak_ptr<IMeetingSessionDirectory> session_directory) noexcept
    : collector_(collector),
      session_directory_(session_directory) {
    // 刻意不在这里 lock()/调用任何外部依赖：构造只保存引用与 weak 句柄，
    // 不产生可见副作用，因此可以安全地在 call_once 装配期内完成（§12.2.5）。
}

// ------------------------------------------------------------------------------
// 判活与出站：唯一两个"接触外部依赖"的点。
// ------------------------------------------------------------------------------

bool MeetingResponseNotificationAdapter::AcquireLiveTarget(
    const meeting::SessionId& session_id,
    std::shared_ptr<const IMeetingLiveSession>& out_handle) {
    // 空目标不可能命中任何存活会话；按"目标不存在"处理，保证计数守恒。
    if (session_id.empty()) {
        target_missing_ += 1;
        return false;
    }

    // §8.4：目录可能是"从未注入"或"已析构"⇒ 整次调用 no-op，绝不崩溃、绝不悬垂。
    const std::shared_ptr<IMeetingSessionDirectory> directory = session_directory_.lock();
    if (!directory) {
        directory_unavailable_ += 1;
        return false;
    }

    // 返回的是**强句柄**：在它存活的这段范围内，目标必然存活（§8.3）。
    out_handle = directory->FindSessionBySessionId(session_id);
    if (!out_handle) {
        target_missing_ += 1;
        return false;
    }

    target_resolved_ += 1;
    return true;
}

bool MeetingResponseNotificationAdapter::CollectOnce(
    const MeetingOutboundMessage& message) noexcept {
    // §13.4：collector 也可能违约抛异常（测试替身会故意这样做）；
    // 这里必须吞掉并计数，绝不允许异常逃逸进唯一消费者的 WorkerLoop。
    try {
        collector_.Collect(message);
        return true;
    } catch (...) {
        adapter_exception_ += 1;
        return false;
    }
}

void MeetingResponseNotificationAdapter::EmitResponse(const MeetingResponseMessage& message) {
    // 局部强句柄：只在本目标的处理期间持有，绝不缓存到成员（§12.6）。
    std::shared_ptr<const IMeetingLiveSession> handle;
    if (!AcquireLiveTarget(message.response_target_session_id, handle)) {
        // §13.2(2)：目标已消失**不得**改变 result_code / disposition；
        // 本函数不修改已构造好的 message，调用方的领域 result 也不受影响。
        return;
    }

    MeetingOutboundMessage outbound{};
    outbound.kind = MeetingOutboundKind::RESPONSE;
    outbound.response = message;

    // handle 存活到本次 Collect 返回之后：目标在整个产出期间必然存活。
    if (CollectOnce(outbound)) {
        response_produced_ += 1;
    }
}

// ------------------------------------------------------------------------------
// 六项 Response（§6.3 / §6.4 / §10 / §13）
//
// 每个 On* 的形状一致：
//   值初始化 message → 填公共头（码/名/disposition/owner/target）
//   → 填授权相关字段（view / participants，均内置 §10.3 判据）
//   → 判活 → 恰好一次 Collect → 计数
// 整体包在 try/catch 内，异常只影响本次产出（§9.2(1)(2)）。
// ------------------------------------------------------------------------------

void MeetingResponseNotificationAdapter::OnCreateMeeting(
    const meeting::MeetingRequestContext& context,
    const meeting::CreateMeetingResult& result) noexcept {
    try {
        MeetingResponseMessage message{};
        const NormalizedResult normalized = FillResponseHeader(
            message, ClientMeetingCommandKind::CREATE, context, result.common);
        if (normalized.encoding_failed) {
            encoding_failed_ += 1;
        }
        FillViewIfAuthorized(message, normalized, result.meeting);
        EmitResponse(message);
    } catch (...) {
        adapter_exception_ += 1;
    }
}

void MeetingResponseNotificationAdapter::OnJoinMeeting(
    const meeting::MeetingRequestContext& context,
    const meeting::JoinMeetingResult& result) noexcept {
    try {
        MeetingResponseMessage message{};
        const NormalizedResult normalized = FillResponseHeader(
            message, ClientMeetingCommandKind::JOIN, context, result.common);
        if (normalized.encoding_failed) {
            encoding_failed_ += 1;
        }
        FillViewIfAuthorized(message, normalized, result.meeting);
        EmitResponse(message);
    } catch (...) {
        adapter_exception_ += 1;
    }
}

void MeetingResponseNotificationAdapter::OnLeaveMeeting(
    const meeting::MeetingRequestContext& context,
    const meeting::LeaveMeetingResult& result) noexcept {
    try {
        MeetingResponseMessage message{};
        const NormalizedResult normalized = FillResponseHeader(
            message, ClientMeetingCommandKind::LEAVE, context, result.common);
        if (normalized.encoding_failed) {
            encoding_failed_ += 1;
        }
        FillViewIfAuthorized(message, normalized, result.meeting);
        EmitResponse(message);
    } catch (...) {
        adapter_exception_ += 1;
    }
}

void MeetingResponseNotificationAdapter::OnCloseMeeting(
    const meeting::MeetingRequestContext& context,
    const meeting::CloseMeetingResult& result) noexcept {
    try {
        MeetingResponseMessage message{};
        const NormalizedResult normalized = FillResponseHeader(
            message, ClientMeetingCommandKind::CLOSE, context, result.common);
        if (normalized.encoding_failed) {
            encoding_failed_ += 1;
        }
        FillViewIfAuthorized(message, normalized, result.meeting);
        EmitResponse(message);
    } catch (...) {
        adapter_exception_ += 1;
    }
}

void MeetingResponseNotificationAdapter::OnQueryMeeting(
    const meeting::MeetingRequestContext& context,
    const meeting::QueryMeetingResult& result) noexcept {
    try {
        MeetingResponseMessage message{};
        const NormalizedResult normalized = FillResponseHeader(
            message, ClientMeetingCommandKind::QUERY, context, result.common);
        if (normalized.encoding_failed) {
            encoding_failed_ += 1;
        }
        FillViewIfAuthorized(message, normalized, result.meeting);
        EmitResponse(message);
    } catch (...) {
        adapter_exception_ += 1;
    }
}

void MeetingResponseNotificationAdapter::OnListParticipants(
    const meeting::MeetingRequestContext& context,
    const meeting::ListParticipantsResult& result) noexcept {
    try {
        MeetingResponseMessage message{};
        const NormalizedResult normalized = FillResponseHeader(
            message, ClientMeetingCommandKind::LIST_PARTICIPANTS, context, result.common);
        if (normalized.encoding_failed) {
            encoding_failed_ += 1;
        }
        FillViewIfAuthorized(message, normalized, result.meeting);
        FillParticipantsIfAuthorized(message, normalized, result);
        EmitResponse(message);
    } catch (...) {
        adapter_exception_ += 1;
    }
}

// ------------------------------------------------------------------------------
// FINALIZE_CLOSE：逐目标通知（§7.1.2 权威算法的逐句落地）
//
// 没有 response；不修改领域状态；不等待、不重试、不合并、不回滚。
// ------------------------------------------------------------------------------
void MeetingResponseNotificationAdapter::OnFinalizeClose(
    const meeting::FinalizeCloseResult& result) noexcept {
    try {
        // ---- 步骤 0：没有通知意图 ⇒ 直接返回（0 条消息、0 次 Collect） --------
        // 典型来源：重复 FinalizeClose 的 already_finalized 路径（MeetingService.h:217-219）。
        if (result.notification_session_ids.empty()) {
            return;
        }

        // ---- 步骤 1：目录判活；失效 ⇒ 整批 no-op（0 条消息、0 次 Collect） ----
        const std::shared_ptr<IMeetingSessionDirectory> directory =
            session_directory_.lock();
        if (!directory) {
            directory_unavailable_ += 1;
            return;
        }

        // 事件事实只提取一次（只读；每个目标共享同一份真实来源）。
        const ClosedEventFacts facts = ExtractClosedEventFacts(result.events);

        // ---- 步骤 2：对每一个 target 独立处理 --------------------------------
        for (std::size_t i = 0; i < result.notification_session_ids.size(); ++i) {
            const meeting::SessionId& target = result.notification_session_ids[i];

            // 步骤 2a（含防御分支）：目标为空或已消失 ⇒ 只跳过该目标。
            if (target.empty()) {
                target_missing_ += 1;
                continue;
            }

            // 逐目标局部强句柄：循环体结束即释放，绝不跨目标复用、绝不缓存。
            const std::shared_ptr<const IMeetingLiveSession> handle =
                directory->FindSessionBySessionId(target);
            if (!handle) {
                target_missing_ += 1;
                continue;
            }
            target_resolved_ += 1;

            // ---- 步骤 2b：为**当前 target** 构造一条**全新的**单目标消息 ------
            MeetingNotificationMessage notification{};
            notification.notification_type = meeting::EventType::MEETING_CLOSED;
            notification.meeting_id = result.meeting_id;
            // 必须是当前 target：非空、且绝不是别的 target（§7.2 不变式）。
            notification.notification_target_session_id = target;
            notification.has_event_id = facts.has_event_id;
            notification.event_id = facts.event_id;
            notification.has_correlation_request_id = facts.has_correlation_request_id;
            notification.correlation_request_id = facts.correlation_request_id;

            MeetingOutboundMessage outbound{};
            outbound.kind = MeetingOutboundKind::NOTIFICATION;
            outbound.notification = notification;

            // ---- 步骤 2c：恰好一次 Collect；成功后计数 -----------------------
            if (CollectOnce(outbound)) {
                notification_produced_ += 1;
            }
        }
    } catch (...) {
        // 目录 lookup 抛异常、消息拷贝抛异常等一律在此吞掉并计数；
        // 领域侧的 ENDING → CLOSED 收敛**已经完成**，这里不可能回滚（§13.5）。
        adapter_exception_ += 1;
    }
}

// ------------------------------------------------------------------------------
// 只读计数快照（观测用途；不参与任何业务判定）
// ------------------------------------------------------------------------------

MeetingAdapterCounters MeetingResponseNotificationAdapter::Snapshot() const {
    MeetingAdapterCounters counters;
    counters.response_produced = response_produced_.load();
    counters.notification_produced = notification_produced_.load();
    counters.outbound_produced = counters.response_produced + counters.notification_produced;
    counters.target_resolved = target_resolved_.load();
    counters.target_missing = target_missing_.load();
    counters.directory_unavailable = directory_unavailable_.load();
    counters.encoding_failed = encoding_failed_.load();
    counters.adapter_exception = adapter_exception_.load();
    return counters;
}
