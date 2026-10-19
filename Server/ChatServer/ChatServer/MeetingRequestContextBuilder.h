#pragma once

// ==============================================================================
// M6.2：MeetingRequestContext 的可信构建器。
//
// 纯函数式、无副作用：
//   - 只依赖 meeting/MeetingTypes.h 与 C++ 标准库；
//   - 不读取任何会话对象的内部字段（身份只来自调用方传入的已认证值）；
//   - 不生成/补偿 request ID，不推断 handling server，不伪造可选身份；
//   - 无缓存、无全局状态、无锁、无线程、无第三方依赖。
//
// 调用约定（服务端可信值）：
//   session_id            ← 服务端稳定的会话标识值
//   authenticated_user_id ← 已认证成功路径上的局部 uid
//   request_id            ← 调用方提供；空值原样保留，由领域返回 INVALID_ARGUMENT
//   local_server_id       ← 当前节点标识；不得在此"修正"成别的值
// ==============================================================================

#include "meeting/MeetingTypes.h"

class MeetingRequestContextBuilder {
public:
    meeting::MeetingRequestContext Build(
        const meeting::SessionId& session_id,
        meeting::UserId authenticated_user_id,
        const meeting::RequestId& request_id,
        const meeting::ServerId& local_server_id) const {
        meeting::MeetingRequestContext context;

        // 四个身份字段一律原样透传：builder 不推断、不覆盖、不补偿。
        context.request_id = request_id;
        context.authenticated_user_id = authenticated_user_id;
        context.session_id = session_id;
        context.handling_chat_server_id = local_server_id;

        // 可选身份字段在 M6 阶段没有可信来源，恒为 false；
        // presence flag 为 false 时，对应空值不得被解释为有效标识。
        context.has_device_id = false;
        context.has_trusted_owner_chat_server_id = false;

        return context;
    }
};
