// AuthRequestBuilder 单元测试（P5-T07）。
//
// 用 proto 反解校验三类请求的字段号/内容正确，并验证附带的设备签名与服务端 verifyDevice
// 采用的 canonical message 一致（用 DeviceProofService::buildMessage 复算 + 公钥验签）。

#include "client_core/AuthRequestBuilder.h"
#include "transport/DeviceProof.h"
#include "im.pb.h"

#include <openssl/evp.h>
#include <openssl/x509.h>

#include <iostream>
#include <string>
#include <vector>

using namespace im::account;
using Bytes = std::vector<unsigned char>;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& name)
{
    std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << std::endl;
    if (!cond) ++g_failures;
}

class SoftSigner : public IP256Signer {
public:
    SoftSigner() { m_key.generate(); }
    Bytes publicKeyDer() const override { return m_key.publicKeyDer(); }
    Bytes sign(const Bytes& msg) const override { return m_key.sign(msg); }
private:
    im::transport::DeviceProofKey m_key;
};

Bytes toBytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

bool verify(const Bytes& pubDer, const Bytes& msg, const std::string& sig)
{
    if (pubDer.empty() || sig.empty()) return false;
    const unsigned char* p = pubDer.data();
    EVP_PKEY* pkey = d2i_PUBKEY(nullptr, &p, static_cast<long>(pubDer.size()));
    if (!pkey) return false;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = false;
    if (ctx) {
        ok = EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
             EVP_DigestVerifyUpdate(ctx, msg.data(), msg.size()) == 1 &&
             EVP_DigestVerifyFinal(ctx,
                 reinterpret_cast<const unsigned char*>(sig.data()), sig.size()) == 1;
        EVP_MD_CTX_free(ctx);
    }
    EVP_PKEY_free(pkey);
    return ok;
}

const Bytes kSession = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const std::string kDevice = "dev-req-builder";

void testTokenLogin()
{
    std::cout << "[1] TokenLoginRq 字段与签名" << std::endl;
    SoftSigner signer;
    DeviceProofService svc(signer, kDevice);
    AuthRequestBuilder b(svc);

    const std::string access = "ACCESS-TOKEN-1";
    const std::string reqId = "req-1";
    const auto r = b.buildTokenLogin(kSession, access, reqId);
    check(r.ok, "构造成功");

    im::proto::TokenLoginRq rq;
    check(rq.ParseFromString(r.payload), "可被 proto 反解");
    check(rq.access_token() == access, "access_token 字段正确");
    check(rq.device_id() == kDevice, "device_id 字段正确");
    check(rq.request_id() == reqId, "request_id 字段正确");

    const Bytes msg = DeviceProofService::buildMessage(
        "token-login", kSession, kDevice, access, Bytes{});
    check(verify(signer.publicKeyDer(), msg, rq.device_signature()), "设备签名验签通过");
}

void testRefresh()
{
    std::cout << "[2] RefreshTokenRq 字段与签名" << std::endl;
    SoftSigner signer;
    DeviceProofService svc(signer, kDevice);
    AuthRequestBuilder b(svc);

    const std::string refresh = "REFRESH-TOKEN-1";
    const std::string reqId = "rf-req-9";
    const auto r = b.buildRefresh(kSession, refresh, reqId);
    check(r.ok, "构造成功");

    im::proto::RefreshTokenRq rq;
    check(rq.ParseFromString(r.payload), "可被 proto 反解");
    check(rq.refresh_token() == refresh, "refresh_token 字段正确");
    check(rq.device_id() == kDevice, "device_id 字段正确");
    check(rq.request_id() == reqId, "request_id 字段正确");

    const Bytes msg = DeviceProofService::buildMessage(
        "token-refresh", kSession, kDevice, refresh + std::string(1, '\0') + reqId, Bytes{});
    check(verify(signer.publicKeyDer(), msg, rq.device_signature()), "设备签名验签通过");
}

void testLogout()
{
    std::cout << "[3] LogoutRq 字段与签名（all=true/false）" << std::endl;
    SoftSigner signer;
    DeviceProofService svc(signer, kDevice);
    AuthRequestBuilder b(svc);

    const std::string refresh = "REFRESH-TOKEN-1";
    const auto r = b.buildLogout(kSession, refresh, /*allDevices=*/true);
    check(r.ok, "构造成功");

    im::proto::LogoutRq rq;
    check(rq.ParseFromString(r.payload), "可被 proto 反解");
    check(rq.refresh_token() == refresh, "refresh_token 字段正确");
    check(rq.device_id() == kDevice, "device_id 字段正确");
    check(rq.logout_all_devices() == true, "logout_all_devices=true");

    const Bytes msg = DeviceProofService::buildMessage(
        "logout", kSession, kDevice, refresh + std::string(1, '\0') + "1", Bytes{});
    check(verify(signer.publicKeyDer(), msg, rq.device_signature()), "设备签名验签通过");

    const auto r0 = b.buildLogout(kSession, refresh, false);
    im::proto::LogoutRq rq0;
    check(rq0.ParseFromString(r0.payload), "all=false 请求可反解");
    check(rq0.logout_all_devices() == false, "all=false 时字段为 false");
}

} // namespace

int main()
{
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    std::cout << "=== test_auth_request_builder ===" << std::endl;
    testTokenLogin();
    testRefresh();
    testLogout();

    if (g_failures == 0) {
        std::cout << "test_auth_request_builder PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_auth_request_builder FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
