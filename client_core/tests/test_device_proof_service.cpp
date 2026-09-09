// DeviceProofService 单元测试（P5-T03）。
//
// 验证：
//   1) 四类操作构造的 canonical message 与"独立复算"（按 field 编码规则手算）逐字节一致；
//   2) 只有 password-login 在 message 内绑定公钥、且结果 DeviceProof 携带公钥，其余三类不带；
//   3) 生成的签名能被对应 P-256 公钥本地验签（DER SPKI + SHA256withECDSA）。
//
// 签名器用 transport::DeviceProofKey（软件 P-256）适配 IP256Signer。

#include "client_core/DeviceProofService.h"
#include "transport/DeviceProof.h"

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

// 用 transport::DeviceProofKey 适配 IP256Signer
class SoftSigner : public IP256Signer {
public:
    SoftSigner() { m_key.generate(); }
    Bytes publicKeyDer() const override { return m_key.publicKeyDer(); }
    Bytes sign(const Bytes& msg) const override { return m_key.sign(msg); }
private:
    im::transport::DeviceProofKey m_key;
};

// 独立复算 canonical message（不依赖被测代码），用于交叉验证。
void u32be(Bytes& out, std::uint32_t v)
{
    out.push_back((v >> 24) & 0xFF);
    out.push_back((v >> 16) & 0xFF);
    out.push_back((v >> 8) & 0xFF);
    out.push_back(v & 0xFF);
}
void fld(Bytes& out, const std::string& s)
{
    u32be(out, static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}
void fld(Bytes& out, const Bytes& b)
{
    u32be(out, static_cast<std::uint32_t>(b.size()));
    out.insert(out.end(), b.begin(), b.end());
}
Bytes independentMessage(const std::string& op, const Bytes& sess, const std::string& dev,
                         const std::string& cred, const Bytes& pub)
{
    const std::string label = "jitong-device-proof-v1";
    Bytes out(label.begin(), label.end());
    fld(out, op);
    fld(out, sess);
    fld(out, dev);
    fld(out, cred);
    fld(out, pub);
    return out;
}

bool verify(const Bytes& pubDer, const Bytes& msg, const Bytes& sig)
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
             EVP_DigestVerifyFinal(ctx, sig.data(), sig.size()) == 1;
        EVP_MD_CTX_free(ctx);
    }
    EVP_PKEY_free(pkey);
    return ok;
}

const Bytes kSession = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                        0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
const std::string kDevice = "device-unit-test";

void testPasswordLogin()
{
    std::cout << "[1] password-login：绑定公钥 + 字节一致 + 验签" << std::endl;
    SoftSigner signer;
    DeviceProofService svc(signer, kDevice);
    const std::string tel = "13800000001";
    const std::string passHex(64, 'a');
    const auto proof = svc.forPasswordLogin(kSession, tel, passHex);

    check(proof.ok(), "有签名");
    check(!proof.publicKeyDer.empty(), "password-login 携带公钥");

    const Bytes expectMsg = independentMessage(
        "password-login", kSession, kDevice, tel + std::string(1, '\0') + passHex,
        signer.publicKeyDer());
    // buildMessage 与独立复算一致
    const Bytes builtMsg = DeviceProofService::buildMessage(
        "password-login", kSession, kDevice, tel + std::string(1, '\0') + passHex,
        signer.publicKeyDer());
    check(builtMsg == expectMsg, "canonical message 与独立复算逐字节一致");
    check(verify(signer.publicKeyDer(), expectMsg, proof.signature), "签名可被公钥验证");
}

void testTokenLogin()
{
    std::cout << "[2] token-login：不绑公钥" << std::endl;
    SoftSigner signer;
    DeviceProofService svc(signer, kDevice);
    const std::string access = "access-token-value";
    const auto proof = svc.forTokenLogin(kSession, access);

    check(proof.ok(), "有签名");
    check(proof.publicKeyDer.empty(), "token-login 不携带公钥");

    const Bytes expectMsg = independentMessage("token-login", kSession, kDevice, access, Bytes{});
    check(verify(signer.publicKeyDer(), expectMsg, proof.signature), "签名可被公钥验证（空公钥字段）");
}

void testTokenRefresh()
{
    std::cout << "[3] token-refresh：refresh + '\\0' + request_id" << std::endl;
    SoftSigner signer;
    DeviceProofService svc(signer, kDevice);
    const std::string refresh = "refresh-token-value";
    const std::string reqId = "rf-req-123";
    const auto proof = svc.forTokenRefresh(kSession, refresh, reqId);

    check(proof.ok() && proof.publicKeyDer.empty(), "有签名且不带公钥");
    const Bytes expectMsg = independentMessage(
        "token-refresh", kSession, kDevice, refresh + std::string(1, '\0') + reqId, Bytes{});
    check(verify(signer.publicKeyDer(), expectMsg, proof.signature), "签名可被验证");
}

void testLogout()
{
    std::cout << "[4] logout：refresh + '\\0' + all 标志" << std::endl;
    SoftSigner signer;
    DeviceProofService svc(signer, kDevice);
    const std::string refresh = "refresh-token-value";

    const auto proofAll = svc.forLogout(kSession, refresh, true);
    const Bytes msgAll = independentMessage(
        "logout", kSession, kDevice, refresh + std::string(1, '\0') + "1", Bytes{});
    check(verify(signer.publicKeyDer(), msgAll, proofAll.signature), "logout all=1 验签通过");

    const auto proofOne = svc.forLogout(kSession, refresh, false);
    const Bytes msgOne = independentMessage(
        "logout", kSession, kDevice, refresh + std::string(1, '\0') + "0", Bytes{});
    check(verify(signer.publicKeyDer(), msgOne, proofOne.signature), "logout all=0 验签通过");

    // 交叉：all=1 的签名不应通过 all=0 的 message（绑定确实包含该标志）
    check(!verify(signer.publicKeyDer(), msgOne, proofAll.signature),
          "all=1 签名不能验证 all=0 message（绑定生效）");
}

} // namespace

int main()
{
    std::cout << "=== test_device_proof_service ===" << std::endl;
    testPasswordLogin();
    testTokenLogin();
    testTokenRefresh();
    testLogout();

    if (g_failures == 0) {
        std::cout << "test_device_proof_service PASSED" << std::endl;
        return 0;
    }
    std::cout << "test_device_proof_service FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
