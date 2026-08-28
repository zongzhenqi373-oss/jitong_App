// im_server 端到端测试：真服务端（进程内）+ client_core 真客户端回环
// 覆盖 M1/M2：注册、登录（加盐哈希校验）、资料下发、在线文本转发、
//            离线消息补发、心跳保活、多端互踢

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Server.h"
#include "client_core/ClientCore.h"
#include "client_core/Protocol.h"
#include "sha256.h"

#include <fstream>

using namespace im;
using namespace im::proto;

namespace {

constexpr std::uint16_t TEST_PORT = 24680;
const char* TEST_DB = "/tmp/im_server_e2e.db";
const char* TEST_UPLOADS = "/tmp/im_server_e2e_uploads";
const std::string TEST_CERT = IM_SERVER_TEST_CERT;
const std::string TEST_KEY = IM_SERVER_TEST_KEY;
const std::string TEST_SERVERNAME = "im.example.com";
const im::ClientConfig testConfig{TEST_SERVERNAME, TEST_CERT};


struct RecordingEvents : IClientEvents {
    std::mutex mtx;
    std::condition_variable cv;

    int registerResult = -1;
    int loginResult = -1;
    int loginUserId = 0;
    UserInfo self;
    bool gotSelf = false;
    std::vector<im::FriendInfo> friends;
    std::vector<std::pair<int, std::string>> chats;
    std::vector<std::pair<int, int>> chatResults;
    std::vector<int> offlineEvents;
    int kicked = -1;
    bool closed = false;

    struct ImageMsg { int fromId; std::string bytes; int w; int h; std::string msgId; };
    std::vector<ImageMsg> images; // M7 起图片不再内联字节，这个回调不会再被触发，保留仅为满足接口

    // 文件/图片卡片收集（HTTP 文件服务：ChatInfoRq type=FILE/IMAGE 统一走 onFileCard）
    struct FileCard {
        int fromId; std::string fileId; std::string name; std::int64_t size; std::string msgId;
        std::string contentType; std::string sha256; bool isImage; int w; int h;
    };
    std::vector<FileCard> fileCards;

    // 漫游收集
    std::vector<RoamMessage> roamConvs;
    bool gotRoamConvs = false;
    struct RoamPage { int peerId; std::vector<RoamMessage> msgs; bool hasMore; std::int64_t minSeq; };
    std::vector<RoamPage> roamPages;

    void notify() { cv.notify_all(); }
    void onRegisterResult(int r) override { std::lock_guard<std::mutex> l(mtx); registerResult = r; notify(); }
    void onLoginResult(int r, int uid) override { std::lock_guard<std::mutex> l(mtx); loginResult = r; loginUserId = uid; notify(); }
    void onSelfInfo(const UserInfo& i) override { std::lock_guard<std::mutex> l(mtx); self = i; gotSelf = true; notify(); }
    void onFriendInfo(const im::FriendInfo& f) override { std::lock_guard<std::mutex> l(mtx); friends.push_back(f); notify(); }
    void onChatMessage(int from, const std::string& m) override { std::lock_guard<std::mutex> l(mtx); chats.emplace_back(from, m); notify(); }
    void onImageMessage(int from, const std::string& bytes, int w, int h, const std::string& msgId) override {
        std::lock_guard<std::mutex> l(mtx); images.push_back({from, bytes, w, h, msgId}); notify();
    }
    void onChatSendResult(int friId, int r) override { std::lock_guard<std::mutex> l(mtx); chatResults.emplace_back(friId, r); notify(); }
    void onAddFriendRequest(int, const std::string&) override {}
    void onAddFriendResult(int, const std::string&) override {}
    void onFriendOffline(int uid) override { std::lock_guard<std::mutex> l(mtx); offlineEvents.push_back(uid); notify(); }
    void onKickedOffline(int reason) override { std::lock_guard<std::mutex> l(mtx); kicked = reason; notify(); }
    void onConnectionClosed() override { std::lock_guard<std::mutex> l(mtx); closed = true; notify(); }
    void onRoamConversations(const std::vector<RoamMessage>& convs) override {
        std::lock_guard<std::mutex> l(mtx); roamConvs = convs; gotRoamConvs = true; notify();
    }
    void onRoamMessages(int peerId, const std::vector<RoamMessage>& msgs, bool hasMore, std::int64_t minSeq) override {
        std::lock_guard<std::mutex> l(mtx); roamPages.push_back({peerId, msgs, hasMore, minSeq}); notify();
    }

    void onFileCard(int fromId, const std::string& fileId, const std::string& name,
                    std::int64_t size, const std::string& msgId, const std::string& contentType,
                    const std::string& sha256, bool isImage, int w, int h) override {
        std::lock_guard<std::mutex> l(mtx);
        fileCards.push_back({fromId, fileId, name, size, msgId, contentType, sha256, isImage, w, h});
        notify();
    }

    template <typename Pred>
    bool waitFor(Pred pred, int timeoutMs = 5000)
    {
        std::unique_lock<std::mutex> l(mtx);
        return cv.wait_for(l, std::chrono::milliseconds(timeoutMs), pred);
    }
};

// 已知种子用户（seedIfEmpty 预置，密码均 123456）
struct SeedUser { const char* nick; const char* tel; };
const SeedUser kZhangsan{"张三", "13800000001"};
const SeedUser kLisi{"李四", "13800000002"};

} // namespace

int main()
{
    std::remove(TEST_DB);
    std::remove((std::string(TEST_DB) + "-wal").c_str());
    std::remove((std::string(TEST_DB) + "-shm").c_str());

    imsrv::Server server(TEST_PORT, 2, 2, TEST_DB, TEST_UPLOADS, TEST_CERT, TEST_KEY,
                          static_cast<std::uint16_t>(TEST_PORT + 1));
    assert(server.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // 等监听就绪

    // ============ M1：注册 + 登录 + 文本 ============

    // 注册新用户（密码会经 sha256 哈希后传输）
    RecordingEvents regEv;
    ClientCore regClient(testConfig);
    regClient.setEventSink(&regEv);
    assert(regClient.connectToServer("127.0.0.1", TEST_PORT));
    regClient.sendRegister("新用户", "13900000001", "mypassword");
    assert(regEv.waitFor([&] { return regEv.registerResult == REGISTER_SUCC; }));
    // 重复注册同昵称 → 应返回昵称已存在
    regClient.sendRegister("新用户", "13900000002", "mypassword");
    assert(regEv.waitFor([&] { return regEv.registerResult == REGISTER_NICK_EXIT; }));
    regClient.disconnect();

    // A（张三）登录
    RecordingEvents ea;
    ClientCore a(testConfig);
    a.setEventSink(&ea);
    a.setHeartbeatIntervalMs(500);
    assert(a.connectToServer("127.0.0.1", TEST_PORT));
    a.sendLogin(kZhangsan.tel, "123456");
    assert(ea.waitFor([&] { return ea.loginResult == LOGIN_SUCCESS; }));
    const int idA = ea.loginUserId;
    assert(ea.waitFor([&] { return ea.gotSelf; }));
    assert(ea.self.nick == "张三");

    // 密码错误应失败
    RecordingEvents badEv;
    ClientCore bad(testConfig);
    bad.setEventSink(&badEv);
    assert(bad.connectToServer("127.0.0.1", TEST_PORT));
    bad.sendLogin(kZhangsan.tel, "wrongpass");
    assert(badEv.waitFor([&] { return badEv.loginResult == LOGIN_PASSERROR; }));
    bad.disconnect();

    // B（李四）登录，好友列表应含在线的张三
    RecordingEvents eb;
    ClientCore b(testConfig);
    b.setEventSink(&eb);
    b.setHeartbeatIntervalMs(500);
    assert(b.connectToServer("127.0.0.1", TEST_PORT));
    b.sendLogin(kLisi.tel, "123456");
    assert(eb.waitFor([&] { return eb.loginResult == LOGIN_SUCCESS; }));
    const int idB = eb.loginUserId;
    assert(eb.waitFor([&] {
        for (const auto& f : eb.friends) if (f.id == idA && f.status == STATUS_ONLINE) return true;
        return false;
    }));

    // A → B 文本在线转发
    a.sendChatMessage(idB, "你好李四");
    assert(eb.waitFor([&] {
        for (const auto& c : eb.chats) if (c.first == idA && c.second == "你好李四") return true;
        return false;
    }));
    assert(ea.waitFor([&] {
        for (const auto& r : ea.chatResults) if (r.first == idB && r.second == CHAT_RESULT_SUCC) return true;
        return false;
    }));

    // ============ M2：心跳 / 离线补发 / 互踢 ============

    // 心跳保活：保持 ~1.5s（≈3 个心跳周期），连接应保持（服务端应答了心跳）
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    assert(a.isConnected());
    assert(b.isConnected());

    // B 断开（模拟下线）→ A 应收到 B 的离线通知
    b.disconnect();
    assert(ea.waitFor([&] {
        for (int uid : ea.offlineEvents) if (uid == idB) return true;
        return false;
    }));

    // A → B 离线消息 → B 重连后补发
    a.sendChatMessage(idB, "这是离线消息");
    RecordingEvents eb2;
    ClientCore b2(testConfig);
    b2.setEventSink(&eb2);
    assert(b2.connectToServer("127.0.0.1", TEST_PORT));
    b2.sendLogin(kLisi.tel, "123456");
    assert(eb2.waitFor([&] { return eb2.loginResult == LOGIN_SUCCESS; }));
    assert(eb2.waitFor([&] {
        for (const auto& c : eb2.chats) if (c.first == idA && c.second == "这是离线消息") return true;
        return false;
    }));

    // 互踢：张三在第二个连接上再登录 → 旧连接 a 收到被踢
    RecordingEvents ea2;
    ClientCore a2(testConfig);
    a2.setEventSink(&ea2);
    assert(a2.connectToServer("127.0.0.1", TEST_PORT));
    a2.sendLogin(kZhangsan.tel, "123456");
    assert(ea2.waitFor([&] { return ea2.loginResult == LOGIN_SUCCESS; }));
    assert(ea.waitFor([&] { return ea.kicked == 0; }));

    // 第三方账号（王五）：用于验证文件上传/下载不能只凭 fileId 越权。
    RecordingEvents ec;
    ClientCore c(testConfig);
    c.setEventSink(&ec);
    assert(c.connectToServer("127.0.0.1", TEST_PORT));
    c.sendLogin("13800000003", "123456");
    assert(ec.waitFor([&] { return ec.loginResult == LOGIN_SUCCESS; }));

    // 旧 TCP 内联图片/分片文件测试保留作迁移历史，但不再参与编译。
#if 0
    // ============ M3：图片消息（在线转发 + 服务端落盘 + 离线补发） ============

    // 构造伪 PNG 图片字节（真实 PNG 魔数开头，>1KB，含 0 字节，验证二进制安全）
    std::string imgBytes;
    for (int i = 0; i < 2048; ++i) imgBytes.push_back(static_cast<char>(i % 256));
    imgBytes[0] = '\x89'; imgBytes[1] = 'P'; imgBytes[2] = 'N'; imgBytes[3] = 'G';
    imgBytes[4] = '\x0D'; imgBytes[5] = '\x0A'; imgBytes[6] = '\x1A'; imgBytes[7] = '\x0A';

    // a2（张三）→ b2（李四）在线发图片
    a2.sendImageMessage(idB, imgBytes, 64, 64);
    assert(eb2.waitFor([&] {
        for (const auto& im : eb2.images)
            if (im.fromId == idA && im.bytes == imgBytes && im.w == 64 && im.h == 64) return true;
        return false;
    }));

    // 服务端应已按魔数嗅探落盘 uploads/img/<sha256>.png
    const std::string expectImg = std::string(TEST_UPLOADS) + "/img/" + im::sha256Hex(imgBytes) + ".png";
    {
        std::ifstream f(expectImg, std::ios::binary | std::ios::ate);
        assert(f && f.tellg() == (std::streamsize)imgBytes.size());
    }

    // 离线图片：b2 断开 → a2 发图 → b2 重连后应补发图片（读盘回传字节）
    b2.disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 等服务端感知断开
    std::string imgBytes2(1024, '\x5A');
    a2.sendImageMessage(idB, imgBytes2, 32, 32);

    RecordingEvents eb3;
    ClientCore b3(testConfig);
    b3.setEventSink(&eb3);
    assert(b3.connectToServer("127.0.0.1", TEST_PORT));
    b3.sendLogin(kLisi.tel, "123456");
    assert(eb3.waitFor([&] { return eb3.loginResult == LOGIN_SUCCESS; }));
    assert(eb3.waitFor([&] {
        for (const auto& im : eb3.images)
            if (im.fromId == idA && im.bytes == imgBytes2 && im.w == 32) return true;
        return false;
    }));

    // 送达回执：b3 上线收讫后，a2 应收到补发消息的 SUCC 回执（"已转存"→"已送达"）
    // （在线图片 SUCC 1 次 + 离线文本/图片补发回执 ≥1 次，这里断言总数 ≥2）
    assert(ea2.waitFor([&] {
        int succ = 0;
        for (const auto& cr : ea2.chatResults)
            if (cr.first == idB && cr.second == CHAT_RESULT_SUCC) ++succ;
        return succ >= 2;
    }));

    // ============ M6：消息漫游（会话列表末条 + 历史分页游标 + 图片读盘） ============
    // 至此 conv(A,B) 有 4 条：seq1 "你好李四"(文本)、seq2 "这是离线消息"(文本)、
    //   seq3 imgBytes(图片)、seq4 imgBytes2(图片)。用 a2（张三 idA）视角漫游。

    // 会话列表：拉回每会话末条；末条为图片，预览不应带字节（withImage=false）
    a2.sendRoamConvRq();
    assert(ea2.waitFor([&] { return ea2.gotRoamConvs; }));
    {
        bool foundConvB = false;
        for (const auto& rm : ea2.roamConvs) {
            if (rm.fromId == idB || rm.toId == idB) {
                foundConvB = true;
                assert(rm.type == 1);              // 末条是图片
                assert(rm.imageBytes.empty());     // 预览不读盘、不带字节（修复 2）
                assert(rm.seq == 4);               // 会话最后一条 seq
            }
        }
        assert(foundConvB);
    }

    // 历史第一页：最新 2 条（seq4、seq3），均图片且带完整字节；hasMore=true，minSeq=3
    a2.sendRoamMsgRq(idB, INT64_MAX, 2);
    assert(ea2.waitFor([&] { return ea2.roamPages.size() >= 1; }));
    std::int64_t page1Min = 0;
    {
        const auto& pg = ea2.roamPages[0];
        assert(pg.peerId == idB);
        assert(pg.msgs.size() == 2);
        assert(pg.hasMore);                        // 满 limit 即认为还有更早的
        // 服务端 seq 倒序返回
        assert(pg.msgs[0].seq == 4 && pg.msgs[1].seq == 3);
        assert(pg.msgs[0].type == 1 && !pg.msgs[0].imageBytes.empty()); // 图片读盘回传完整字节
        assert(pg.msgs[1].type == 1 && !pg.msgs[1].imageBytes.empty());
        assert(pg.minSeq == 3);
        page1Min = pg.minSeq;
    }

    // 历史第二页（上拉）：游标 beforeSeq=3 → seq2、seq1 文本，无重叠；hasMore=true，minSeq=1
    a2.sendRoamMsgRq(idB, page1Min, 2);
    assert(ea2.waitFor([&] { return ea2.roamPages.size() >= 2; }));
    std::int64_t page2Min = 0;
    {
        const auto& pg = ea2.roamPages[1];
        assert(pg.msgs.size() == 2);
        assert(pg.msgs[0].seq == 2 && pg.msgs[1].seq == 1);
        assert(pg.msgs[0].text == "这是离线消息" && pg.msgs[1].text == "你好李四");
        assert(pg.minSeq == 1);
        page2Min = pg.minSeq;
    }

    // 历史到头：游标 beforeSeq=1 → 空批，hasMore=false
    a2.sendRoamMsgRq(idB, page2Min, 2);
    assert(ea2.waitFor([&] { return ea2.roamPages.size() >= 3; }));
    {
        const auto& pg = ea2.roamPages[2];
        assert(pg.msgs.empty());
        assert(!pg.hasMore);
    }

    // ============ M7：文件上传（分片 + 水位线 + Complete 校验 + 卡片转发） ============
    // 造一个 300KB 文件（2 块：256KB + 44KB），a2(张三 idA) 发给 idB
    std::string fileBytes(300 * 1024, '\0');
    for (size_t i = 0; i < fileBytes.size(); ++i) fileBytes[i] = static_cast<char>((i * 31 + 7) % 256);
    const int chunkSz = 256 * 1024;
    const int totalChunks = (static_cast<int>(fileBytes.size()) + chunkSz - 1) / chunkSz; // 2
    const std::string fileSha = im::sha256Hex(fileBytes);
    const std::string fmsgId = "file-e2e-0001";

    a2.sendFileOffer(fmsgId, idB, "report.bin", (std::int64_t)fileBytes.size(), totalChunks, fileSha);
    assert(ea2.waitFor([&] { return ea2.gotOffer && ea2.lastOffer.msgId == fmsgId; }));
    assert(ea2.lastOffer.result == FILE_OFFER_OK);
    assert(ea2.lastOffer.receivedChunks == 0); // 新文件水位线 0

    // 未参与该上传的第三方即使知道 fileId，也不能注入第 0 块。
    c.sendFileChunk(fmsgId, 0, std::string(chunkSz, 'X'));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    for (int i = 0; i < totalChunks; ++i) {
        size_t off = (size_t)i * chunkSz;
        size_t len = std::min((size_t)chunkSz, fileBytes.size() - off);
        a2.sendFileChunk(fmsgId, i, fileBytes.substr(off, len));
    }
    a2.sendFileComplete(fmsgId, fmsgId);
    // 发送方应收到 done 进度
    assert(ea2.waitFor([&] { return ea2.lastProgressStatus == FILE_ST_DONE; }));
    // 接收方 b3(李四) 应收到文件卡片（ChatInfoRq type=FILE，走在线转发）
    assert(eb3.waitFor([&] {
        for (auto& fc : eb3.fileCards) if (fc.fileId == fmsgId && fc.name == "report.bin") return true;
        return false;
    }));

    // 断点续传：新文件只发第 0 块就"中断"，重发 Offer 应返回 N=1
    const std::string fmsgId2 = "file-e2e-0002";
    std::string big(300 * 1024, 'A');
    const std::string big2Sha = im::sha256Hex(big);
    a2.sendFileOffer(fmsgId2, idB, "resume.bin", (std::int64_t)big.size(), 2, big2Sha);
    assert(ea2.waitFor([&] { return ea2.gotOffer && ea2.lastOffer.msgId == fmsgId2; }));
    ea2.lastProgressRecv = -1;
    a2.sendFileChunk(fmsgId2, 0, big.substr(0, 256*1024)); // 只发第 0 块
    // 等服务端落盘该块
    assert(ea2.waitFor([&] { return ea2.lastProgressRecv >= 1; }));
    ea2.gotOffer = false;
    a2.sendFileOffer(fmsgId2, idB, "resume.bin", (std::int64_t)big.size(), 2, big2Sha); // 重发 Offer
    assert(ea2.waitFor([&] { return ea2.gotOffer && ea2.lastOffer.msgId == fmsgId2; }));
    assert(ea2.lastOffer.receivedChunks == 1); // 水位线续传起点=1
    ea2.lastProgressStatus = -1;
    a2.sendFileChunk(fmsgId2, 1, big.substr(256*1024)); // 续发第 1 块
    a2.sendFileComplete(fmsgId2, fmsgId2);
    assert(ea2.waitFor([&] { return ea2.lastProgressStatus == FILE_ST_DONE; }));

    // 下载：b3(李四) 从 file-e2e-0001 的第 0 块开始拉，应收到 2 块，拼接 == fileBytes
    eb3.downloadChunks.clear();
    b3.sendFileDownload(fmsgId, 0);
    assert(eb3.waitFor([&] {
        int bytes = 0; for (auto& c : eb3.downloadChunks) bytes += (int)c.second.size();
        return bytes == (int)fileBytes.size();
    }));
    // 断点续传下载：从第 1 块拉，只应收到第 1 块（44KB）
    eb3.downloadChunks.clear();
    b3.sendFileDownload(fmsgId, 1);
    assert(eb3.waitFor([&] {
        return eb3.downloadChunks.size() == 1 && eb3.downloadChunks[0].first == 1;
    }));

    // 第三方不是文件消息的发送者或接收者：下载必须失败且不能收到任何字节。
    ec.downloadChunks.clear();
    ec.lastProgressStatus = -1;
    c.sendFileDownload(fmsgId, 0);
    assert(ec.waitFor([&] { return ec.lastProgressStatus == FILE_ST_FAILED; }));
    assert(ec.downloadChunks.empty());

    // ============ M7 I1 回归：末块为部分块时重发 Offer 不应死锁 ============
    // 300KB 文件（2 块：256KB 整块 + 44KB 部分块）。发完两块（包含部分末块）后，
    // 在 Complete 之前重发 Offer：应报告 receivedChunks == totalChunks（已全部收到），
    // 而不是回退指向最后一个块下标（这会导致客户端重发末块，破坏 partSize == fileSize 的假设）。
    const std::string fmsgId4 = "file-e2e-0004-partial";
    a2.sendFileOffer(fmsgId4, idB, "partial.bin", (std::int64_t)fileBytes.size(), totalChunks, fileSha);
    ea2.gotOffer = false;
    assert(ea2.waitFor([&] { return ea2.gotOffer && ea2.lastOffer.msgId == fmsgId4; }));
    assert(ea2.lastOffer.receivedChunks == 0);

    for (int i = 0; i < totalChunks; ++i) {
        size_t off = (size_t)i * chunkSz;
        size_t len = std::min((size_t)chunkSz, fileBytes.size() - off);
        a2.sendFileChunk(fmsgId4, i, fileBytes.substr(off, len));
    }
    // 等服务端落盘全部字节（水位线达到 totalChunks），再重发 Offer
    assert(ea2.waitFor([&] { return ea2.lastProgressRecv >= totalChunks; }));

    ea2.gotOffer = false; // 复位共享标量，避免断言基于上一轮遗留状态而假通过
    a2.sendFileOffer(fmsgId4, idB, "partial.bin", (std::int64_t)fileBytes.size(), totalChunks, fileSha); // 重发 Offer（未 Complete）
    assert(ea2.waitFor([&] { return ea2.gotOffer && ea2.lastOffer.msgId == fmsgId4; }));
    assert(ea2.lastOffer.receivedChunks == totalChunks); // 已全部收到，不应回退到 totalChunks-1

    ea2.lastProgressStatus = -1; // 复位，避免命中上一轮遗留的 DONE
    a2.sendFileComplete(fmsgId4, fmsgId4);
    assert(ea2.waitFor([&] { return ea2.lastProgressStatus == FILE_ST_DONE; }));

    // ============ M7 边界：超限 + sha256 校验 ============

    // 超限：声明 101MB → 拒绝
    ea2.gotOffer = false;
    a2.sendFileOffer("file-toolarge", idB, "big.bin", 101LL*1024*1024, 999, "deadbeef");
    assert(ea2.waitFor([&] { return ea2.gotOffer && ea2.lastOffer.msgId == "file-toolarge"; }));
    assert(ea2.lastOffer.result == FILE_OFFER_TOO_LARGE);

    // sha256 不符：发正确字节但 Offer 声明错误 sha → Complete 应 failed
    const std::string fmsgId3 = "file-badsha";
    std::string data3(1024, 'Z');
    ea2.gotOffer = false; ea2.lastProgressStatus = -1;
    a2.sendFileOffer(fmsgId3, idB, "bad.bin", (std::int64_t)data3.size(), 1, "0000000000000000000000000000000000000000000000000000000000000000");
    assert(ea2.waitFor([&] { return ea2.gotOffer && ea2.lastOffer.msgId == fmsgId3; }));
    a2.sendFileChunk(fmsgId3, 0, data3);
    a2.sendFileComplete(fmsgId3, fmsgId3);
    assert(ea2.waitFor([&] { return ea2.lastProgressStatus == FILE_ST_FAILED; }));

#endif

    // ============ M7：图片/文件统一走独立 HTTPS 文件服务 ============
    const std::string mediaInput = "/tmp/im_http_e2e.png";
    const std::string mediaOutput = "/tmp/im_http_e2e_download.png";
    std::string png(4096, '\x5a');
    png[0] = '\x89'; png[1] = 'P'; png[2] = 'N'; png[3] = 'G';
    png[4] = '\x0d'; png[5] = '\x0a'; png[6] = '\x1a'; png[7] = '\x0a';
    { std::ofstream out(mediaInput, std::ios::binary); out.write(png.data(), png.size()); }

    const std::string mediaId = a2.uploadMedia(mediaInput, idB, true);
    assert(!mediaId.empty());
    a2.sendFileMessage(idB, mediaId, "photo.png", static_cast<std::int64_t>(png.size()),
                       "image/png", im::sha256Hex(png), true, 64, 64);
    assert(eb2.waitFor([&] {
        for (const auto& card : eb2.fileCards)
            if (card.fileId == mediaId && card.isImage && card.w == 64) return true;
        return false;
    }));

    // 消息参与者可下载，第三方即使知道 file_id 也不能下载。
    assert(b2.downloadMedia(mediaId, mediaOutput));
    { std::ifstream in(mediaOutput, std::ios::binary);
      std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      assert(got == png); }
    assert(!c.downloadMedia(mediaId, "/tmp/im_http_e2e_forbidden.png"));

    std::remove(mediaInput.c_str());
    std::remove(mediaOutput.c_str());
    a2.disconnect();
    b2.disconnect();
    c.disconnect();
    server.stop();

    std::cout << "test_e2e PASSED" << std::endl;
    return 0;
}
