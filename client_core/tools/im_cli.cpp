// im_cli：client_core 的命令行演示客户端
// 用途：Mac 本机不依赖 Qt/Android，直接连 im_server 真实互聊（演示/联调/兜底第二端）
//
// 用法: im_cli [ip] [port] [tls_server_name] [ca_file]        默认 127.0.0.1 24563 im.example.com <im_server 开发证书>
// tls_server_name/ca_file 用于连接非本机部署的真实服务器时，匹配对方证书的实际域名/受信任 CA
// 命令:
//   register <昵称> <手机号> <密码>     注册
//   login <手机号> <密码>               登录（种子用户: 张三 13800000001 / 李四 13800000002 / 王五 13800000003，密码均 123456）
//   send <好友id> <内容>                发文本
//   sendimg <好友id> <图片路径>          发图片（走 HTTP 文件服务上传）
//   sendfile <好友id> <文件路径>         发文件（走 HTTP 文件服务上传）
//   quit                              下线退出
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "client_core/ClientCore.h"
#include "client_core/Protocol.h"

namespace {

class CliEvents : public im::IClientEvents {
public:
    im::ClientCore* core = nullptr; // 下载文件/图片卡片时用，main() 里回填

    void onRegisterResult(int result) override
    {
        std::cout << "\n[注册结果] " << (result == im::proto::REGISTER_SUCC ? "成功"
                  : result == im::proto::REGISTER_NICK_EXIT ? "失败：昵称已存在"
                  : "失败：手机号已存在") << std::endl;
    }
    void onLoginResult(int result, int userId) override
    {
        if (result == im::proto::LOGIN_SUCCESS)
            std::cout << "\n[登录成功] 我的 id=" << userId << std::endl;
        else
            std::cout << "\n[登录失败] " << (result == im::proto::LOGIN_NOTEXIT ? "用户不存在" : "密码错误") << std::endl;
    }
    void onSelfInfo(const im::UserInfo& info) override
    {
        std::cout << "[我的资料] " << info.nick << "（" << info.feeling << "）" << std::endl;
    }
    void onFriendInfo(const im::FriendInfo& info) override
    {
        std::cout << "[好友] id=" << info.id << " " << info.nick
                  << (info.status == im::proto::STATUS_ONLINE ? "（在线）" : "（离线）") << std::endl;
    }
    void onChatMessage(int fromId, const std::string& msg) override
    {
        std::cout << "\n[收到消息] 来自 " << fromId << ": " << msg << std::endl;
    }
    void onImageMessage(int fromId, const std::string& bytes, int w, int h, const std::string&) override
    {
        // M7 起图片走 onFileCard 统一下发，字节不再内联，这个回调不会再被触发
        (void)fromId; (void)bytes; (void)w; (void)h;
    }
    void onFileCard(int fromId, const std::string& fileId, const std::string& name,
                    std::int64_t size, const std::string& msgId, const std::string& contentType,
                    const std::string&, bool isImage, int w, int h) override
    {
        (void)msgId; (void)contentType;
        const std::string ext = isImage ? ".jpg" : "";
        const std::string path = "/tmp/im_cli_recv_" + std::to_string(fromId) + "_" + fileId + ext;
        if (core && core->downloadMedia(fileId, path)) {
            std::cout << "\n[收到" << (isImage ? "图片" : "文件") << "] 来自 " << fromId
                      << " " << name << " (" << size << " 字节"
                      << (isImage ? (", " + std::to_string(w) + "x" + std::to_string(h)) : "")
                      << ") 已下载到 " << path << std::endl;
        } else {
            std::cout << "\n[收到" << (isImage ? "图片" : "文件") << "卡片] 来自 " << fromId
                      << " " << name << " (" << size << " 字节) file_id=" << fileId
                      << "（下载失败或 core 未就绪）" << std::endl;
        }
    }
    void onChatSendResult(int friId, int result) override
    {
        std::cout << "[发送回执] 给 " << friId << " "
                  << (result == im::proto::CHAT_RESULT_SUCC ? "已送达" : "对方离线，已转存") << std::endl;
    }
    void onAddFriendRequest(int fromId, const std::string& fromNick) override
    {
        std::cout << "\n[好友申请] " << fromNick << "(id=" << fromId << ") 请求加你" << std::endl;
    }
    void onAddFriendResult(int result, const std::string& destNick) override
    {
        std::cout << "\n[好友申请结果] " << destNick << " result=" << result << std::endl;
    }
    void onFriendOffline(int userId) override
    {
        std::cout << "[好友下线] id=" << userId << std::endl;
    }
    void onKickedOffline(int) override
    {
        std::cout << "\n[被踢下线] 你的账号已在其他设备登录" << std::endl;
    }
    void onConnectionClosed() override
    {
        std::cout << "[连接已断开]" << std::endl;
    }
};

void printHelp()
{
    std::cout << "命令: register <昵称> <手机号> <密码> | login <手机号> <密码> | "
                 "send <好友id> <内容> | sendimg <好友id> <图片路径> | sendfile <好友id> <文件路径> | quit" << std::endl;
}

} // namespace

int main(int argc, char* argv[])
{
    const std::string ip = argc > 1 ? argv[1] : "127.0.0.1";
    const std::uint16_t port = argc > 2 ? static_cast<std::uint16_t>(std::atoi(argv[2])) : im::proto::TCP_PORT;
    // tls_server_name/ca_file 都可以在命令行覆盖：连本机开发服务器时用默认值即可；
    // 连真实部署的服务器时，需要传对方证书实际签发的域名 + 信任的 CA，不能一直用开发证书糊弄过去
    const std::string tlsServerName = argc > 3 ? argv[3] : "im.example.com";
    const std::string caFile = argc > 4 ? argv[4] : IM_CLI_DEFAULT_CA;

    im::ClientCore core({tlsServerName, caFile});
    CliEvents events;
    events.core = &core;
    core.setEventSink(&events);

    if (!core.connectToServer(ip, port)) {
        std::cerr << "连接服务端失败 " << ip << ":" << port << std::endl;
        return 1;
    }
    std::cout << "已连接 " << ip << ":" << port << std::endl;
    printHelp();

    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "login") {
            std::string tel, pass;
            iss >> tel >> pass;
            core.sendLogin(tel, pass);
        } else if (cmd == "register") {
            std::string nick, tel, pass;
            iss >> nick >> tel >> pass;
            core.sendRegister(nick, tel, pass);
        } else if (cmd == "send") {
            int friId = 0;
            iss >> friId;
            std::string msg;
            std::getline(iss, msg);
            if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
            if (friId > 0 && !msg.empty()) core.sendChatMessage(friId, msg);
        } else if (cmd == "sendimg") {
            int friId = 0;
            std::string path;
            iss >> friId >> path;
            const std::string fileId = core.uploadMedia(path, friId, /*isImage=*/true);
            if (fileId.empty()) { std::cout << "上传失败 " << path << std::endl; continue; }
            std::error_code ec;
            const auto size = static_cast<std::int64_t>(std::filesystem::file_size(path, ec));
            core.sendFileMessage(friId, fileId, std::filesystem::path(path).filename().string(),
                                 size, "image/jpeg", "", /*isImage=*/true, 0, 0);
            std::cout << "[发送图片] " << path << " (" << size << " 字节) file_id=" << fileId << std::endl;
        } else if (cmd == "sendfile") {
            int friId = 0;
            std::string path;
            iss >> friId >> path;
            const std::string fileId = core.uploadMedia(path, friId, /*isImage=*/false);
            if (fileId.empty()) { std::cout << "上传失败 " << path << std::endl; continue; }
            std::error_code ec;
            const auto size = static_cast<std::int64_t>(std::filesystem::file_size(path, ec));
            core.sendFileMessage(friId, fileId, std::filesystem::path(path).filename().string(),
                                 size, "application/octet-stream", "", /*isImage=*/false, 0, 0);
            std::cout << "[发送文件] " << path << " (" << size << " 字节) file_id=" << fileId << std::endl;
        } else if (cmd == "quit" || cmd == "exit") {
            core.sendOfflineNotify();
            std::this_thread::sleep_for(std::chrono::milliseconds(300)); // 等下线包发出
            break;
        } else if (!cmd.empty()) {
            printHelp();
        }
    }

    core.disconnect();
    return 0;
}
