// ==============================================================================
// M6.4：CountingNoWireCollector 实现。
//
// 唯一动作：按 kind 分类计数。**没有**第二件事可做：
//   - 不发送（M6 没有 wire 交付能力，§7.5）；
//   - 不缓存消息本体（否则它就变成了第二条出站队列，违反 §11.4 / §12.1）；
//   - 不写文件、不打印、不做同步 I/O。
// ==============================================================================

#include "CountingNoWireCollector.h"

#include "MeetingOutboundMessage.h"

void CountingNoWireCollector::Collect(const MeetingOutboundMessage& message) noexcept {
    // 只读 kind，不读取消息内容：即使某个分支含敏感 MeetingView，
    // 本类也不会把它扩散到任何地方（不缓存、不打印、不编码）。
    switch (message.kind) {
    case MeetingOutboundKind::RESPONSE:
        responses_ += 1;
        return;
    case MeetingOutboundKind::NOTIFICATION:
        notifications_ += 1;
        return;
    default:
        // 未知 kind 不静默丢弃：单独计数，便于测试断言"不认识的输入被看见"。
        unknown_kind_ += 1;
        return;
    }
}

CountingNoWireCollectorSnapshot CountingNoWireCollector::Snapshot() const {
    CountingNoWireCollectorSnapshot snapshot;
    snapshot.responses = responses_.load();
    snapshot.notifications = notifications_.load();
    snapshot.unknown_kind = unknown_kind_.load();
    snapshot.total = snapshot.responses + snapshot.notifications + snapshot.unknown_kind;
    return snapshot;
}

std::uint64_t CountingNoWireCollector::ResponseCount() const {
    return responses_.load();
}

std::uint64_t CountingNoWireCollector::NotificationCount() const {
    return notifications_.load();
}

std::uint64_t CountingNoWireCollector::TotalCount() const {
    return responses_.load() + notifications_.load() + unknown_kind_.load();
}
