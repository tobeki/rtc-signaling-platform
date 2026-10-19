#pragma once

// ==============================================================================
// M6.4：生产出站接受者 —— 只计数、绝不发送（§7.3 / §7.5）。
//
// 为什么需要它：
//   M6 的 adapter 必须有一个**真实**的出站接受者，才能让"产出了多少条 typed
//   消息"成为**可运行、可断言**的事实；但 M6 又绝不能发送任何字节（wire 交付
//   属 Phase 3）。因此生产实现唯一允许的动作就是"分类 + 计数"。
//
// 硬约束（全部可静态核对）：
//   - 不 include CSession.h / CServer.h / LogicSystem.h / asio / Boost / Redis /
//     MySQL / gRPC；不调用 CSession::Send；不产生、不缓存、不编码任何字节；
//   - 不做可阻塞的同步 I/O，不起线程，不建队列，不记日志文件；
//   - 不根据计数改变任何业务行为：计数只用于诊断与测试断言（§9.2(1)）；
//   - Collect() 为非抛出操作（override 上标注 noexcept，编译期强制）。
//
// 所有权契约（§12.1 / §12.2.3）：
//   CountingNoWireCollector 由 LogicSystem 以 unique_ptr 成员**独占拥有**，
//   且其成员声明必须**先于** MeetingResponseNotificationAdapter
//   （逆序析构 ⇒ adapter 先销毁，collector 后销毁）。
//   adapter 只持有它的**非拥有引用**；本类永不反过来引用 adapter。
//   禁止在 EnsureWorkQueueStarted() 内创建局部 collector 再把引用交给 adapter
//   （R-13：局部对象析构后即为悬垂引用）。
//
// 线程模型：
//   Collect() 只由唯一消费者的 WorkerLoop 调用；计数值为原子量，
//   允许其它线程只读观测（Snapshot()/各 Count()），但不构成全局屏障。
// ==============================================================================

#include <atomic>
#include <cstdint>

#include "MeetingOutboundChannel.h"

// ------------------------------------------------------------------------------
// 只读计数快照。
//
// total 是**被接受**的 typed 消息总数；它绝不表示"已发送"、"已送达"或
// "客户端已收到"（§13.3：L4/L5 在 M6 不可统计、不可验证）。
// ------------------------------------------------------------------------------
struct CountingNoWireCollectorSnapshot {
    std::uint64_t responses = 0;
    std::uint64_t notifications = 0;
    std::uint64_t total = 0;

    // kind 取值不在 MeetingOutboundKind 的两个已定义值内（理论不可达）。
    // 单列出来是为了让"未知 kind 不静默通过"可断言，而不是把它混进两类计数。
    std::uint64_t unknown_kind = 0;
};

// ------------------------------------------------------------------------------
// 生产 collector。
// ------------------------------------------------------------------------------
class CountingNoWireCollector : public IMeetingOutboundMessageCollector {
public:
    CountingNoWireCollector() {}
    ~CountingNoWireCollector() override {}

    // 契约：不抛异常、不阻塞、不回写领域状态。
    // noexcept 是编译期强制：本实现只做原子自增与枚举分支，不可能抛出。
    void Collect(const MeetingOutboundMessage& message) noexcept override;

    CountingNoWireCollectorSnapshot Snapshot() const;

    std::uint64_t ResponseCount() const;
    std::uint64_t NotificationCount() const;
    std::uint64_t TotalCount() const;

private:
    std::atomic<std::uint64_t> responses_{0};
    std::atomic<std::uint64_t> notifications_{0};
    std::atomic<std::uint64_t> unknown_kind_{0};
};
