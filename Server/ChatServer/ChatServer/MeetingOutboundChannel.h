#pragma once

// ==============================================================================
// M6.1：出站接受者与存活标记接口（只定义数据与边界）。
//
// 本文件只定义抽象接口：
//   - 不含 wire format、协议号、序列化或发送实现；
//   - 只 include <memory> / <string>，并以**不完整类型**引用出站值对象
//     （前向声明 MeetingOutboundMessage，不 include MeetingOutboundMessage.h）；
//   - 不 include CSession.h / CServer.h / LogicSystem.h / Redis / MySQL / gRPC。
// ==============================================================================

#include <memory>
#include <string>

// 前向声明：Collect() 只接收 const 引用，不需要完整类型。
// 这样本头文件可以独立于出站消息值对象被包含。
struct MeetingOutboundMessage;

// ------------------------------------------------------------------------------
// IMeetingLiveSession：「某条当前存活的会话」的**只读存活标记**。
//
// 它刻意**不**提供任何发送方法：
//   - M6 的生产路径不发送字节（§7.5）；
//   - 因此不存在"定义了发送能力却永远不可调用"的死接口；
//   - 且所有方法均为 const ⇒ 与 shared_ptr<const IMeetingLiveSession> 契约一致。
//
// 它的存在价值（可断言、可测试）：
//   - 证明 lookup 返回的是**生命周期受控**的强句柄（§8.3）；
//   - 给 adapter 一个稳定的"目标已解析"证据，用于 resolved / missing 计数；
//   - Phase 3 的发送接口可以基于它派生（§8.2.4）。
// ------------------------------------------------------------------------------
class IMeetingLiveSession {
public:
    virtual ~IMeetingLiveSession() {}

    // 该会话的稳定标识（值拷贝）。**只读**，用于日志与测试断言。
    virtual std::string DescribeSessionId() const = 0;
};

// ------------------------------------------------------------------------------
// IMeetingSessionDirectory：session_id → 存活 Session 的判活目录。
//
// 实现方（M6.3 的 CServer）必须：
//   - 在自己的互斥下，从"持有 shared_ptr 的注册表"中**复制**该 shared_ptr；
//   - 禁止用裸指针构造 shared_ptr（会建立第二个控制块 ⇒ 未定义行为）；
//   - 成功返回非空句柄，失败返回空 shared_ptr。
// ------------------------------------------------------------------------------
class IMeetingSessionDirectory {
public:
    virtual ~IMeetingSessionDirectory() {}

    // 非拥有、线程安全、生命周期受控：句柄存活期间目标必然存活。
    virtual std::shared_ptr<const IMeetingLiveSession>
        FindSessionBySessionId(const std::string& session_id) = 0;
};

// ------------------------------------------------------------------------------
// IMeetingOutboundMessageCollector：typed 出站消息的唯一接受者。
//
// 调用契约（实现方与调用方都必须遵守）：
//   - **不得抛出**：异常不得穿过 Collect() 进入领域/消费者线程；
//   - **不得阻塞**：Collect() 必须是快速、非阻塞操作；
//   - **不得回写领域状态**：Collect() 只观察/记录/计数，不得修改 Meeting 领域。
//
// 该接口接收 typed 值对象，不接收字符串 payload；本轮不实现任何 collector。
// ------------------------------------------------------------------------------
class IMeetingOutboundMessageCollector {
public:
    virtual ~IMeetingOutboundMessageCollector() {}

    virtual void Collect(const MeetingOutboundMessage& message) = 0;
};
