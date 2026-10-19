#pragma once

// ==============================================================================
// M6.1：Meeting 出站 typed 值对象（M6 → Phase 3 的正式交付边界）。
//
// 本文件只定义**纯值类型**：
//   - 不定义 wire format、协议号、序列化或发送逻辑；
//   - 不含任何协议号字段，不调用 CSession 的发送接口；
//   - 不 include CSession.h / CServer.h / LogicSystem.h / Redis / MySQL / gRPC。
//
// 类型复用（不得新增第二套 ResultCode / EventType / 命令枚举）：
//   ClientMeetingCommandKind      ← LogicWorkItem.h
//   meeting::*（ID/结果/视图/事件）← meeting/MeetingTypes.h
//   meeting::ParticipantViewScope ← meeting/MeetingService.h
//       （实际定义在 MeetingService.h:96；MeetingTypes.h 中没有同名类型。
//        为避免复制出第二套枚举，这里直接复用其唯一定义。）
//
// 构造约定：以下 struct 不提供默认初始化；生产者必须显式填充所有字段
// （含 has_* 标志），消费者必须按 kind 只读取对应分支。
// ==============================================================================

#include "LogicWorkItem.h"
#include "meeting/MeetingService.h"
#include "meeting/MeetingTypes.h"

#include <string>
#include <vector>

// ------------------------------------------------------------------------------
// 响应
// ------------------------------------------------------------------------------
struct MeetingResponseMessage {
    ClientMeetingCommandKind command;   // 复用既有枚举，不新增编码枚举
    meeting::RequestId request_id;      // 仅用于关联（correlation），不是幂等键
    meeting::SessionId response_target_session_id;  // 来自可信 context.session_id

    meeting::ResultCode result_code;
    std::string result_name;            // 唯一来源 ResultCodeName()
    meeting::ResultDisposition disposition;

    bool has_owner_chat_server_id;
    meeting::ServerId owner_chat_server_id;          // 仅 MEETING_NOT_LOCAL

    bool has_view;
    meeting::MeetingView view;          // 受 §10 安全规则约束

    bool has_participants;
    std::vector<meeting::ParticipantView> participants;   // 仅 ListParticipants
    bool has_participant_scope;
    meeting::ParticipantViewScope participant_scope;
};

// ------------------------------------------------------------------------------
// 通知
//
// notification_target_session_id 表示**单个**通知目标，且必须非空：
//   N 个有效目标 ⇒ N 条消息 ⇒ N 次 Collect（§7.1.3 守恒式）。
// 禁止改成目标 ID 列表，也禁止复用一条消息代表多个目标。
// ------------------------------------------------------------------------------
struct MeetingNotificationMessage {
    meeting::EventType notification_type;   // 当前仅 MEETING_CLOSED
    meeting::MeetingId meeting_id;

    meeting::SessionId notification_target_session_id;

    bool has_event_id;
    meeting::EventId event_id;              // 来自 result.events 的 MEETING_CLOSED
    bool has_correlation_request_id;
    meeting::RequestId correlation_request_id;   // 来自 CloseContext.originating_request_id
};

// ------------------------------------------------------------------------------
// 统一出站容器
//
// 刻意保留"一个 struct + 两个具体消息 + kind"的形态（与 §7.2 一致）：
// 它是纯值容器，不承担多态分发；kind 决定哪个分支有效。
// ------------------------------------------------------------------------------
enum class MeetingOutboundKind {
    RESPONSE = 0,
    NOTIFICATION = 1
};

struct MeetingOutboundMessage {
    MeetingOutboundKind kind;
    MeetingResponseMessage response;         // kind == RESPONSE 时有效
    MeetingNotificationMessage notification; // kind == NOTIFICATION 时有效
};
