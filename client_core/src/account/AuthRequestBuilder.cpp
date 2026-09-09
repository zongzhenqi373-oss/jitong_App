#include "client_core/AuthRequestBuilder.h"

#include "im.pb.h"

namespace im {
namespace account {

namespace {
std::string toStr(const DeviceProofService::Bytes& b)
{
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}
} // namespace

BuiltRequest AuthRequestBuilder::buildTokenLogin(const DeviceProofService::Bytes& appSessionId,
                                                 const std::string& accessToken,
                                                 const std::string& requestId) const
{
    BuiltRequest out;
    const auto proof = m_proof.forTokenLogin(appSessionId, accessToken);
    if (!proof.ok()) return out; // 设备签名失败

    im::proto::TokenLoginRq rq;
    rq.set_access_token(accessToken);
    rq.set_device_id(m_proof.deviceId());
    rq.set_request_id(requestId);
    rq.set_device_signature(toStr(proof.signature));
    out.payload = rq.SerializeAsString();
    out.ok = true;
    return out;
}

BuiltRequest AuthRequestBuilder::buildRefresh(const DeviceProofService::Bytes& appSessionId,
                                              const std::string& refreshToken,
                                              const std::string& requestId) const
{
    BuiltRequest out;
    const auto proof = m_proof.forTokenRefresh(appSessionId, refreshToken, requestId);
    if (!proof.ok()) return out;

    im::proto::RefreshTokenRq rq;
    rq.set_refresh_token(refreshToken);
    rq.set_device_id(m_proof.deviceId());
    rq.set_request_id(requestId);
    rq.set_device_signature(toStr(proof.signature));
    out.payload = rq.SerializeAsString();
    out.ok = true;
    return out;
}

BuiltRequest AuthRequestBuilder::buildLogout(const DeviceProofService::Bytes& appSessionId,
                                             const std::string& refreshToken, bool allDevices) const
{
    BuiltRequest out;
    const auto proof = m_proof.forLogout(appSessionId, refreshToken, allDevices);
    if (!proof.ok()) return out;

    im::proto::LogoutRq rq;
    rq.set_refresh_token(refreshToken);
    rq.set_device_id(m_proof.deviceId());
    rq.set_logout_all_devices(allDevices);
    rq.set_device_signature(toStr(proof.signature));
    out.payload = rq.SerializeAsString();
    out.ok = true;
    return out;
}

} // namespace account
} // namespace im
