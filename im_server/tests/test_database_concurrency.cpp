#include "Database.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

int main()
{
    const char* dbPath = "/tmp/im_server_database_concurrency.db";
    std::remove(dbPath);
    std::remove((std::string(dbPath) + "-wal").c_str());
    std::remove((std::string(dbPath) + "-shm").c_str());

    // 旧的 lo * 2^20 + hi 算法会让这两个会话发生碰撞。
    const auto conversationA =
        imsrv::makeConversationId(1, 1048579);

    const auto conversationB =
        imsrv::makeConversationId(2, 3);

    assert(conversationA != conversationB);

    // 同一组用户无论发送方向如何，会话 ID 必须相同。
    assert(
        imsrv::makeConversationId(1, 1048579) ==
        imsrv::makeConversationId(1048579, 1)
    );

    assert(
        imsrv::makeConversationId(2, 3) ==
        imsrv::makeConversationId(3, 2)
    );

    imsrv::Database db;
    assert(db.open(dbPath, 4)); // 多连接才能真实覆盖原来的 MAX(seq)+1 竞争窗口
    db.seedIfEmpty();

    // 好友申请必须在离线状态下持久化，并同时出现在发起方/接收方的待处理列表中。
    assert(db.createFriendRequest(1, 2));
    const auto requesterView = db.pendingFriendRequests(1);
    const auto targetView = db.pendingFriendRequests(2);
    assert(requesterView.size() == 1);
    assert(targetView.size() == 1);
    assert(requesterView.front().requesterId == 1);
    assert(requesterView.front().targetId == 2);
    assert(db.resolveFriendRequest(1, 2, false));
    assert(db.pendingFriendRequests(1).empty());
    assert(db.pendingFriendRequests(2).empty());

    // 被拒绝后允许重新申请；同意操作与双向好友关系写入属于同一事务。
    assert(db.createFriendRequest(1, 2));
    assert(db.resolveFriendRequest(1, 2, true));
    assert(db.isFriend(1, 2));
    assert(db.isFriend(2, 1));
    assert(db.removeFriendBidirectional(1, 2));

    //增加两个会话独立分配seq的测试
    imsrv::StoredMessage firstA;
    firstA.msgId = "collision-a-1";
    firstA.senderId = 1;
    firstA.receiverId = 1048579;
    firstA.content = "conversation A";
    firstA.ts = 1;

    assert(db.saveMessage(firstA, true));
    assert(firstA.seq == 1);

    imsrv::StoredMessage firstB;
    firstB.msgId = "collision-b-1";
    firstB.senderId = 2;
    firstB.receiverId = 3;
    firstB.content = "conversation B";
    firstB.ts = 1;

    assert(db.saveMessage(firstB, true));
    assert(firstB.seq == 1);

    imsrv::StoredMessage secondA;
    secondA.msgId = "collision-a-2";
    secondA.senderId = 1048579;
    secondA.receiverId = 1;
    secondA.content = "conversation A second";
    secondA.ts = 2;

    assert(db.saveMessage(secondA, true));
    assert(secondA.seq == 2);

    imsrv::StoredMessage secondB;
    secondB.msgId = "collision-b-2";
    secondB.senderId = 3;
    secondB.receiverId = 2;
    secondB.content = "conversation B second";
    secondB.ts = 2;

    assert(db.saveMessage(secondB, true));
    assert(secondB.seq == 2);

    // 富媒体元数据必须完整落库并能经漫游查询恢复，否则 Android 历史图片无法下载/校验。
    imsrv::StoredMessage image;
    image.msgId = "media-metadata-1";
    image.senderId = 10;
    image.receiverId = 11;
    image.type = 1;
    image.mediaPath = "/tmp/media-metadata-1.png";
    image.fileId = "file-media-metadata-1";
    image.fileSize = 12345;
    image.contentType = "image/png";
    image.sha256 = "0123456789abcdef";
    image.ts = 3;
    assert(db.saveMessage(image, true));

    const auto mediaRows = db.roamMessages(10, 11, INT64_MAX, 10);
    assert(mediaRows.size() == 1);
    assert(mediaRows.front().fileId == image.fileId);
    assert(mediaRows.front().contentType == image.contentType);
    assert(mediaRows.front().sha256 == image.sha256);

    imsrv::StoredMessage fetchedMedia;
    assert(db.getMessageByFileId(image.fileId, fetchedMedia));
    assert(fetchedMedia.contentType == image.contentType);
    assert(fetchedMedia.sha256 == image.sha256);

    //8线程并发测试
    constexpr int threadCount = 8;
    constexpr int messagesPerThread = 50;
    std::mutex resultMtx;
    std::vector<std::int64_t> assigned;
    std::vector<std::thread> workers;

    for (int t = 0; t < threadCount; ++t) {
        workers.emplace_back([&, t]() {
            for (int i = 0; i < messagesPerThread; ++i) {
                imsrv::StoredMessage m;
                m.msgId = "concurrent-" + std::to_string(t) + "-" + std::to_string(i);
                m.senderId = (i % 2 == 0) ? 1 : 2;
                m.receiverId = (i % 2 == 0) ? 2 : 1;
                m.content = "payload";
                m.ts = i;
                assert(db.saveMessage(m, true));
                std::lock_guard<std::mutex> lock(resultMtx);
                assigned.push_back(m.seq);
            }
        });
    }
    for (auto& worker : workers) worker.join();

    const std::size_t expected = threadCount * messagesPerThread;
    assert(assigned.size() == expected);
    std::set<std::int64_t> unique(assigned.begin(), assigned.end());
    assert(unique.size() == expected);
    assert(*unique.begin() == 1);
    assert(*unique.rbegin() == static_cast<std::int64_t>(expected));

    auto rows = db.roamMessages(1, 2, INT64_MAX, static_cast<int>(expected));
    assert(rows.size() == expected);
    for (std::size_t i = 1; i < rows.size(); ++i) assert(rows[i - 1].seq > rows[i].seq);

    std::cout << "test_database_concurrency PASSED" << std::endl;
    return 0;
}
