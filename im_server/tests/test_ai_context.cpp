#include "ai/AiContextBuilder.h"
#include "client_core/Protocol.h"
#include "im.pb.h"

#include <cassert>
#include <iostream>

namespace {

imsrv::StoredMessage textMessage(int from, int to, std::int64_t seq,
                                 const std::string& text) {
    imsrv::StoredMessage message;
    message.senderId = from;
    message.receiverId = to;
    message.seq = seq;
    message.type = static_cast<int>(im::proto::TEXT);
    message.content = text;
    return message;
}

} // namespace

int main() {
    {
        const auto redacted = imsrv::ai::redactSensitiveText(
            "电话13812345678，邮箱test.user@example.com，身份证11010519900101123X，"
            "密钥sk-exampleSecret123");
        assert(redacted.find("13812345678") == std::string::npos);
        assert(redacted.find("test.user@example.com") == std::string::npos);
        assert(redacted.find("11010519900101123X") == std::string::npos);
        assert(redacted.find("sk-exampleSecret123") == std::string::npos);
        assert(redacted.find("[手机号已隐藏]") != std::string::npos);
        assert(redacted.find("[邮箱已隐藏]") != std::string::npos);
        assert(redacted.find("[身份证号已隐藏]") != std::string::npos);
        assert(redacted.find("[密钥已隐藏]") != std::string::npos);
    }

    {
        // roamMessages 返回 seq 倒序；其中混入媒体和其他会话数据。
        std::vector<imsrv::StoredMessage> rows;
        rows.push_back(textMessage(2, 1, 5, "发到 a@b.com"));
        auto image = textMessage(1, 2, 4, "不应进入上下文");
        image.type = static_cast<int>(im::proto::IMAGE);
        rows.push_back(image);
        rows.push_back(textMessage(99, 1, 3, "其他会话不能进入"));
        rows.push_back(textMessage(1, 2, 2, "我晚一点到"));
        rows.push_back(textMessage(2, 1, 1, "几点见？"));

        const imsrv::ai::AiContextPolicy policy{3, 100, 200};
        const auto request = imsrv::ai::buildReplyContext(
            rows, 1, 2, "request-1", "自然", 3, policy);
        assert(request.turns.size() == 3);
        assert(request.turns[0].text == "几点见？");
        assert(request.turns[0].role == imsrv::ai::AiConversationTurn::Role::Peer);
        assert(request.turns[1].text == "我晚一点到");
        assert(request.turns[1].role == imsrv::ai::AiConversationTurn::Role::Me);
        assert(request.turns[2].text.find("a@b.com") == std::string::npos);
    }

    {
        std::vector<imsrv::StoredMessage> rows{
            textMessage(2, 1, 2, "这是最新的一条很长消息"),
            textMessage(1, 2, 1, "更早的消息")
        };
        const imsrv::ai::AiContextPolicy policy{20, 12, 12};
        const auto request = imsrv::ai::buildReplyContext(
            rows, 1, 2, "request-2", "简洁", 9, policy);
        assert(request.turns.size() == 1);
        assert(request.turns[0].text.size() <= 12);
        assert(request.maxSuggestions == 3);
    }

    std::cout << "AI context tests passed\n";
    return 0;
}
