// 协议层单元测试：pb 序列化往返 / 协议号编解码 / UTF-8 安全截断

#include <cassert>
#include <cstring>
#include <iostream>
#include "client_core/Protocol.h"
#include "im.pb.h"
#include "sha256.h"

using namespace im::proto;

int main()
{
    // 1. 协议号小端编解码往返 + 显式字节序验证（与主机序无关）
    char buf[4];
    encodeType32(DEF_PROT_CHAT_INFO_RQ, buf);
    assert(decodeType32(buf) == DEF_PROT_CHAT_INFO_RQ);
    encodeType32(1005, buf); // 1005 = 0x03ED
    assert(static_cast<unsigned char>(buf[0]) == 0xED); // 低字节在前（小端）
    assert(static_cast<unsigned char>(buf[1]) == 0x03);

    // 2. pb 序列化/解析往返（含 UTF-8 中文）
    RegisterRq rq;
    rq.set_nick("张三");
    rq.set_tel("13800000000");
    rq.set_pass("123456");
    const std::string wire = rq.SerializeAsString();
    RegisterRq rq2;
    assert(rq2.ParseFromString(wire));
    assert(rq2.nick() == "张三" && rq2.tel() == "13800000000" && rq2.pass() == "123456");

    // 3. 默认值语义（proto3：未设置字段解码为默认值，对应原 struct 零初始化）
    RegisterRs rs;
    rs.set_result(REGISTER_SUCC);
    RegisterRs rs2;
    assert(rs2.ParseFromString(rs.SerializeAsString()));
    assert(rs2.result() == REGISTER_SUCC);
    LoginRs empty;
    LoginRs empty2;
    assert(empty2.ParseFromString(empty.SerializeAsString()));
    assert(empty2.userid() == 0 && empty2.result() == LOGIN_SUCCESS);

    // 4. 长聊天消息不再受 8KB struct 限制（软上限由应用层控制）
    ChatInfoRq chat;
    chat.set_myid(1);
    chat.set_friid(2);
    chat.set_msg(std::string(100, 'a'));
    ChatInfoRq chat2;
    assert(chat2.ParseFromString(chat.SerializeAsString()));
    assert(chat2.msg().size() == 100);
    // 线上体积 ≈ 实际内容（原 struct 固定 8204 字节）
    assert(chat.SerializeAsString().size() < 120);

    // 5. utf8Truncate：ASCII / UTF-8 中文边界 / 不超不截
    assert(utf8Truncate("hello", 10) == "hello");
    assert(utf8Truncate("hello", 3) == "hel");
    const std::string zh = "张三丰"; // 9 字节
    assert(utf8Truncate(zh, 3) == "张");
    assert(utf8Truncate(zh, 4) == "张");   // 4 字节落在"三"中间，回退
    assert(utf8Truncate(zh, 2) == "");     // 不足一个完整字符
    assert(utf8Truncate(zh, 100) == zh);

    // 6. 协议号范围校验逻辑（dispatch 边界）
    assert(DEF_PROT_FRIEND_OFFLINE - DEF_BASE < DEF_PROT_COUNT);
    assert(DEF_PROT_AI_REPLY_RQ - DEF_BASE < DEF_PROT_COUNT);
    assert(DEF_PROT_AI_REPLY_RS - DEF_BASE < DEF_PROT_COUNT);
    assert(DEF_PROT_AI_CANCEL_RQ - DEF_BASE < DEF_PROT_COUNT);
    assert(DEF_PROT_REGISTER_RQ == DEF_BASE);

    // 7. SHA-256 正确性（对齐 QQNT 密码哈希，用标准测试向量校验）
    assert(im::sha256Hex("") ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(im::sha256Hex("abc") ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 加盐二次哈希（服务端存储逻辑）确定性：同输入同输出
    assert(im::sha256Hex("salt" + im::sha256Hex("pass")) ==
           im::sha256Hex("salt" + im::sha256Hex("pass")));

    std::cout << "test_protocol PASSED" << std::endl;
    return 0;
}
