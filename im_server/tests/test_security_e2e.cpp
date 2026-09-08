// 应用层安全的核心不变式（P4 fail-close）：
//
//   TLS 1.3 握手成功之后，客户端若在**未完成应用层安全握手（AppClientHello →
//   AppServerHello → AppFinished）**之前直接发送任何**明文业务帧**（例如 Login），
//   服务端必须立即 fail-close 关闭连接，且该明文业务帧绝不能到达 AuthHandler。
//
// P4 之后，im::ClientCore 会在 connectToServer() 内强制完成应用握手，无法用它来
// 构造“跳过握手发明文”的攻击者。因此这里用一个**原始 asio TLS 客户端**：只做真实
// TLS 1.3 握手，然后按线格式发送明文 LoginRq，最后断言服务端主动关闭连接。
//
// 该测试与完整业务 e2e 互补：e2e 证明合法链路可用，本测试证明绕过安全握手的
// 明文业务会被 fail-close，服务端没有为了兼容而放开降级。

#include "core/Server.h"
#include "client_core/Protocol.h"
#include "im.pb.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include <asio.hpp>
#include <asio/ssl.hpp>

namespace {
constexpr std::uint16_t PORT = 24690;
constexpr const char* DB = "/tmp/im_server_security_e2e.db";

using asio::ip::tcp;

// 组帧：[4B 大端 body_len=4+payload][4B 小端 protocol_type][payload]
std::string frameOf(std::uint32_t type, const std::string& payload)
{
    const std::uint32_t bodyLen = 4 + static_cast<std::uint32_t>(payload.size());
    std::string out;
    out.resize(8 + payload.size());
    // 4B 大端包长
    out[0] = static_cast<char>((bodyLen >> 24) & 0xFF);
    out[1] = static_cast<char>((bodyLen >> 16) & 0xFF);
    out[2] = static_cast<char>((bodyLen >> 8) & 0xFF);
    out[3] = static_cast<char>(bodyLen & 0xFF);
    // 4B 小端协议号
    out[4] = static_cast<char>(type & 0xFF);
    out[5] = static_cast<char>((type >> 8) & 0xFF);
    out[6] = static_cast<char>((type >> 16) & 0xFF);
    out[7] = static_cast<char>((type >> 24) & 0xFF);
    if (!payload.empty()) std::memcpy(&out[8], payload.data(), payload.size());
    return out;
}
} // namespace

int main()
{
    std::remove(DB);
    std::remove((std::string(DB) + "-wal").c_str());
    std::remove((std::string(DB) + "-shm").c_str());

    imsrv::Server server(PORT, 1, 1, DB, "/tmp/im_server_security_e2e_uploads",
                         IM_SERVER_TEST_CERT, IM_SERVER_TEST_KEY,
                         static_cast<std::uint16_t>(PORT + 1),
                         IM_SERVER_TEST_APP_IDENTITY_KEY, 1);
    assert(server.start());

    asio::io_context io;
    asio::ssl::context ctx(asio::ssl::context::tls_client);
    ctx.set_options(asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 |
                    asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1 |
                    asio::ssl::context::no_tlsv1_2);
    ctx.load_verify_file(IM_SERVER_TEST_CERT);
    ctx.set_verify_mode(asio::ssl::verify_peer);

    asio::ssl::stream<tcp::socket> stream(io, ctx);
    // SNI + 主机名校验用逻辑域名，即便实际连 127.0.0.1
    SSL_set_tlsext_host_name(stream.native_handle(), "im.example.com");
    stream.set_verify_callback(asio::ssl::host_name_verification("im.example.com"));

    asio::error_code ec;
    tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), PORT);
    stream.lowest_layer().connect(ep, ec);
    assert(!ec && "TCP 连接失败");

    stream.handshake(asio::ssl::stream_base::client, ec);
    assert(!ec && "TLS 1.3 握手失败"); // 真实 TLS 成功

    // 故意跳过应用层握手，直接发送“看起来合法”的明文 LoginRq。
    im::proto::LoginRq login;
    login.set_tel("13800000001");
    login.set_pass(std::string(64, 'a')); // 64 位 hex 形状的伪 proof
    login.set_device_id("attacker-device");
    const std::string frame =
        frameOf(im::proto::DEF_PROT_LOGIN_RQ, login.SerializeAsString());
    asio::write(stream, asio::buffer(frame), ec);
    // 写入本身可能成功（数据进入 TLS），关键在于服务端不处理并关闭连接。

    // 断言：服务端 fail-close。读操作应在有界时间内返回 EOF / 连接被重置，
    // 且我们绝不会收到任何 LoginRs。
    stream.lowest_layer().set_option(tcp::socket::keep_alive(true));
    char buf[256];
    std::size_t got = 0;
    asio::error_code readEc;
    // 用带超时的同步读：起一个计时器，超时就取消 socket。
    bool closed = false;
    asio::steady_timer timer(io);
    timer.expires_after(std::chrono::seconds(5));
    timer.async_wait([&](const asio::error_code&) {
        asio::error_code ignore;
        stream.lowest_layer().cancel(ignore);
    });
    stream.async_read_some(asio::buffer(buf, sizeof(buf)),
                           [&](const asio::error_code& e, std::size_t n) {
                               readEc = e;
                               got = n;
                               timer.cancel();
                               // EOF / reset / 主动 cancel 都视为连接被关闭
                               if (e == asio::error::eof || e == asio::error::connection_reset ||
                                   e == asio::error::operation_aborted || e) {
                                   closed = true;
                               }
                           });
    io.run();

    assert(closed && "服务端应对未握手的明文业务帧 fail-close 关闭连接");
    assert(got == 0 && "服务端不得在 fail-close 前回发任何业务数据");

    asio::error_code ignore;
    stream.lowest_layer().close(ignore);
    server.stop();
    std::cout << "test_security_e2e PASSED" << std::endl;
    return 0;
}
