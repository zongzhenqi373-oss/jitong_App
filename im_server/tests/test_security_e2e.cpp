// 当前阶段可执行的真实安全链路：TLS 成功后，未完成 AppClientHello 的旧客户端
// 直接发送业务帧必须被服务端 fail-close。完整登录/消息 e2e 待 P4 C++ 安全通道完成。
#include "client_core/ClientCore.h"
#include "core/Server.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>

namespace {
constexpr std::uint16_t PORT = 24690;
constexpr const char* DB = "/tmp/im_server_security_e2e.db";

struct Events final : im::IClientEvents {
    std::mutex mutex;
    std::condition_variable cv;
    bool closed = false;
    int loginResult = -1;

    void onRegisterResult(int) override {}
    void onLoginResult(int result, int) override { std::lock_guard<std::mutex> lock(mutex); loginResult = result; cv.notify_all(); }
    void onSelfInfo(const im::UserInfo&) override {}
    void onFriendInfo(const im::FriendInfo&) override {}
    void onChatMessage(int, const std::string&) override {}
    void onImageMessage(int, const std::string&, int, int, const std::string&) override {}
    void onChatSendResult(int, int) override {}
    void onAddFriendRequest(int, const std::string&) override {}
    void onAddFriendResult(int, const std::string&) override {}
    void onFriendOffline(int) override {}
    void onKickedOffline(int) override {}
    void onConnectionClosed() override { std::lock_guard<std::mutex> lock(mutex); closed = true; cv.notify_all(); }
    void onRoamConversations(const std::vector<im::RoamMessage>&) override {}
    void onRoamMessages(int, const std::vector<im::RoamMessage>&, bool, std::int64_t) override {}
    void onFileCard(int, const std::string&, const std::string&, std::int64_t,
                    const std::string&, const std::string&, const std::string&,
                    bool, int, int) override {}
};
}

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

    Events events;
    im::ClientCore client({"im.example.com", IM_SERVER_TEST_CERT});
    client.setEventSink(&events);
    assert(client.connectToServer("127.0.0.1", PORT)); // 真实 TLS 1.3
    client.sendLogin("13800000001", "123456");       // 故意跳过应用层握手

    {
        std::unique_lock<std::mutex> lock(events.mutex);
        assert(events.cv.wait_for(lock, std::chrono::seconds(5), [&] { return events.closed; }));
        assert(events.loginResult == -1); // 明文业务帧绝不能到达 AuthHandler
    }

    client.disconnect();
    server.stop();
    return 0;
}
