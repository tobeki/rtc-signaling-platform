#pragma once

// ==============================================================================
// M6.4：Meeting Response / Notification Adapter（真实实现）。
//
// 职责（M6 §5 / §6 / §7）：
//   把**已经发生**的领域结果映射为 typed 出站值对象（MeetingOutboundMessage），
//   并把它交给**唯一**出站接受者（IMeetingOutboundMessageCollector）。
//
// 本层刻意只做"映射 + 判活 + 计数"：
//   - 不拥有 MeetingService，不调用任何领域 mutation，不读取领域 registry；
//   - 不拥有出站接受者，只持有一个**非拥有引用**（生命周期由 LogicSystem 保证）；
//   - 不持有 CSession / CServer / socket / 领域 registry 指针；
//   - 不发送任何字节：M6 生产链路的 collector 只计数（§7.5）。
//
// 依赖边界（§21.1 A2）：
//   只 include C++ 标准库 + 领域纯头文件 + MeetingOutboundChannel.h +
//   MeetingOutboundMessage.h（typed 出站值对象，本身零第三方依赖）。
//   不 include CSession.h / CServer.h / LogicSystem.h / asio / Boost / Redis /
//   MySQL / gRPC / jsoncpp。
//
// 生命周期（§12.1 / §12.2 / §12.6）：
//   collector 的生命周期必须 ⊇ adapter 的生命周期；adapter 只保存引用。
//   Session 目录以 std::weak_ptr 判活（不延长 CServer 生命周期）；
//   lookup 得到的 shared_ptr<const IMeetingLiveSession> 只在**单个目标**的处理
//   期间作为局部强句柄存在，**不**被缓存到 adapter。
//
// 异常边界（§13.4）：七个 On* 全部 noexcept，内部 try/catch，异常绝不外逸。
// ==============================================================================

#include <atomic>
#include <cstdint>
#include <memory>

#include "MeetingOutboundChannel.h"
// typed 出站值对象（MeetingResponseMessage / MeetingNotificationMessage /
// MeetingOutboundMessage）。该头只依赖 C++ 标准库与纯领域头文件。
#include "MeetingOutboundMessage.h"
#include "meeting/MeetingService.h"

// ------------------------------------------------------------------------------
// IMeetingResponseNotificationAdapter：六个外部 API 的 response 编码 +
// FINALIZE_CLOSE 的 notification 编码（§6.4，签名逐字对齐）。
//
// 六项 On* 同时接收**可信** MeetingRequestContext（response target 的唯一来源）
// 与对应领域 result；OnFinalizeClose 没有发起请求，因此只接收 result。
// ------------------------------------------------------------------------------
class IMeetingResponseNotificationAdapter {
public:
    virtual ~IMeetingResponseNotificationAdapter() {}

    virtual void OnCreateMeeting(const meeting::MeetingRequestContext& context,
                                 const meeting::CreateMeetingResult& result) = 0;
    virtual void OnJoinMeeting(const meeting::MeetingRequestContext& context,
                               const meeting::JoinMeetingResult& result) = 0;
    virtual void OnLeaveMeeting(const meeting::MeetingRequestContext& context,
                                const meeting::LeaveMeetingResult& result) = 0;
    virtual void OnCloseMeeting(const meeting::MeetingRequestContext& context,
                                const meeting::CloseMeetingResult& result) = 0;
    virtual void OnQueryMeeting(const meeting::MeetingRequestContext& context,
                                const meeting::QueryMeetingResult& result) = 0;
    virtual void OnListParticipants(const meeting::MeetingRequestContext& context,
                                    const meeting::ListParticipantsResult& result) = 0;

    // FINALIZE_CLOSE：没有 response，也没有发起请求；只按**逐目标**规则产出通知。
    virtual void OnFinalizeClose(const meeting::FinalizeCloseResult& result) = 0;
};

// ------------------------------------------------------------------------------
// 只读诊断计数（§13.3 的 L0–L3 + §13.2 第 4 条）。
//
// 用途仅限于**观测与测试断言**：绝不参与任何业务判定，也绝不回写领域状态
// （§9.2(1) / §21.1 A-series "计数不被用于业务判定"）。
//
// 计数语义（两条路径共用；测试用**差值**断言，不依赖绝对值）：
//   target_resolved       lookup 命中并进入产出流程的目标数
//   target_missing        lookup 为空（含空 target 的防御分支）的目标数
//   directory_unavailable weak 判活失败 ⇒ 整次调用 no-op 的次数
//   outbound_produced     collector 实际接受的 typed 消息数（= 两类之和）
//   encoding_failed       §13.2(4)：未识别 ResultCode 降级为 INTERNAL_ERROR 的次数
//   adapter_exception     §13.3 L3：在 adapter 边界被吞掉的异常次数
//
// 守恒（§7.1.3）：一次 OnFinalizeClose 内
//   target_resolved + target_missing == result.notification_session_ids.size()
//   notification_produced == target_resolved（前提：collector 遵守不抛契约；
//   collector 抛异常时该目标计入 adapter_exception，且 notification_produced
//   相应少 1 —— 这是**可观测**的采集端违约，不是静默丢失）。
// ------------------------------------------------------------------------------
struct MeetingAdapterCounters {
    std::uint64_t response_produced = 0;
    std::uint64_t notification_produced = 0;
    std::uint64_t outbound_produced = 0;   // response_produced + notification_produced
    std::uint64_t target_resolved = 0;
    std::uint64_t target_missing = 0;
    std::uint64_t directory_unavailable = 0;
    std::uint64_t encoding_failed = 0;
    std::uint64_t adapter_exception = 0;
};

// ------------------------------------------------------------------------------
// 生产实现。
//
// 构造契约：
//   - collector 非空且生命周期 ⊇ 本对象（§12.1：由 LogicSystem 以 unique_ptr 成员
//     独占拥有，且声明顺序在 adapter 之前 ⇒ 逆序析构时 adapter 先销毁）；
//   - session_directory 可为空/已失效：此时所有出站调用都是 no-op 并计数
//     （§8.4：最坏结果是"消息不产出"，不是崩溃或悬垂）。
//
// 线程模型：
//   - 七项 On* 只由唯一消费者的 WorkerLoop 调用（single-writer），本类因此
//     不加锁、不建线程、不做同步 I/O；
//   - 计数为原子量，允许其它线程只读观测（Snapshot()），但不构成全局屏障。
// ------------------------------------------------------------------------------
class MeetingResponseNotificationAdapter : public IMeetingResponseNotificationAdapter {
public:
    MeetingResponseNotificationAdapter(
        IMeetingOutboundMessageCollector& collector,
        std::weak_ptr<IMeetingSessionDirectory> session_directory) noexcept;
    ~MeetingResponseNotificationAdapter() override {}

    void OnCreateMeeting(const meeting::MeetingRequestContext& context,
                         const meeting::CreateMeetingResult& result) noexcept override;
    void OnJoinMeeting(const meeting::MeetingRequestContext& context,
                       const meeting::JoinMeetingResult& result) noexcept override;
    void OnLeaveMeeting(const meeting::MeetingRequestContext& context,
                        const meeting::LeaveMeetingResult& result) noexcept override;
    void OnCloseMeeting(const meeting::MeetingRequestContext& context,
                        const meeting::CloseMeetingResult& result) noexcept override;
    void OnQueryMeeting(const meeting::MeetingRequestContext& context,
                        const meeting::QueryMeetingResult& result) noexcept override;
    void OnListParticipants(const meeting::MeetingRequestContext& context,
                            const meeting::ListParticipantsResult& result) noexcept override;
    void OnFinalizeClose(const meeting::FinalizeCloseResult& result) noexcept override;

    // 只读计数快照（best-effort：不阻塞、不加锁、不构成全局屏障）。
    MeetingAdapterCounters Snapshot() const;

private:
    // 判活 + 计数：命中时输出**仅在本目标处理期间**有效的局部强句柄。
    // 返回 false 表示"目录不可用"或"目标不存在"，调用方只跳过产出，不改领域结果。
    bool AcquireLiveTarget(const meeting::SessionId& session_id,
                           std::shared_ptr<const IMeetingLiveSession>& out_handle);

    // 唯一出站调用点：**恰好一次** Collect，异常在内部被吞掉并计数。
    // 返回 true 表示 collector 接受成功（不等于"已发送"/"已送达"）。
    bool CollectOnce(const MeetingOutboundMessage& message) noexcept;

    void EmitResponse(const MeetingResponseMessage& message);

    IMeetingOutboundMessageCollector& collector_;              // 非拥有；生命周期 ⊇ 本对象
    std::weak_ptr<IMeetingSessionDirectory> session_directory_; // 判活；不拥有

    std::atomic<std::uint64_t> response_produced_{0};
    std::atomic<std::uint64_t> notification_produced_{0};
    std::atomic<std::uint64_t> target_resolved_{0};
    std::atomic<std::uint64_t> target_missing_{0};
    std::atomic<std::uint64_t> directory_unavailable_{0};
    std::atomic<std::uint64_t> encoding_failed_{0};
    std::atomic<std::uint64_t> adapter_exception_{0};
};
