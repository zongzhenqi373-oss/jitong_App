// 认证请求构造（P5-T07）。
//
// 把 TokenLoginRq / RefreshTokenRq / LogoutRq 的构造收敛到一处：填充字段（字段号对齐
// protocol/im.proto）、附带由 DeviceProofService 生成的设备签名，输出可直接发送的序列化字节。
// 与网络/连接解耦，便于单测（用 proto 反解校验字段）。
//
// 对齐 im.proto：
//   TokenLoginRq   { access_token=1, device_id=3, request_id=4, device_signature=5 }
//   RefreshTokenRq { refresh_token=1, device_id=2, request_id=3, device_signature=4 }
//   LogoutRq       { refresh_token=1, device_id=2, logout_all_devices=3, device_signature=4 }

#ifndef CLIENT_CORE_AUTH_REQUEST_BUILDER_H
#define CLIENT_CORE_AUTH_REQUEST_BUILDER_H

#include "client_core/DeviceProofService.h"

#include <string>

namespace im {
namespace account {

struct BuiltRequest {
    bool ok = false;
    std::string payload; // 序列化后的 protobuf 字节
};

class AuthRequestBuilder {
public:
    explicit AuthRequestBuilder(const DeviceProofService& proofSvc) : m_proof(proofSvc) {}

    /**
     * TokenLoginRq：用 access_token 登录。request_id 用于幂等/关联。
     * 设备签名 operation=token-login，credential=access_token（不绑公钥）。
     */
    BuiltRequest buildTokenLogin(const DeviceProofService::Bytes& appSessionId,
                                 const std::string& accessToken, const std::string& requestId) const;

    /**
     * RefreshTokenRq：刷新 token。request_id 为 TokenManager 分配（Single-Flight 复用）。
     * 设备签名 operation=token-refresh，credential=refresh_token + '\0' + request_id。
     */
    BuiltRequest buildRefresh(const DeviceProofService::Bytes& appSessionId,
                              const std::string& refreshToken, const std::string& requestId) const;

    /**
     * LogoutRq：登出。allDevices 决定是否登出全部设备。
     * 设备签名 operation=logout，credential=refresh_token + '\0' + (all?"1":"0")。
     */
    BuiltRequest buildLogout(const DeviceProofService::Bytes& appSessionId,
                             const std::string& refreshToken, bool allDevices) const;

private:
    const DeviceProofService& m_proof;
};

} // namespace account
} // namespace im

#endif // CLIENT_CORE_AUTH_REQUEST_BUILDER_H
