// SQLite 本地存储单元测试：CRUD 往返 + UTF-8 中文 + upsert 覆盖 + limit

#include <cassert>
#include <cstdio>
#include <iostream>
#include "client_core/SqliteStorage.h"

using namespace im;

int main()
{
    const std::string dbPath = "/tmp/client_core_test_storage.db";
    std::remove(dbPath.c_str());

    SqliteStorage st;
    assert(st.open(dbPath));

    // 1. 保存自己资料 + upsert 覆盖
    UserInfo self;
    self.id = 42; self.iconId = 3; self.nick = "测试用户"; self.feeling = "努力实现财富自由";
    assert(st.saveSelfInfo(self));
    self.feeling = "新签名";
    assert(st.saveSelfInfo(self)); // 覆盖不报错

    // 2. 保存好友 + 读取
    FriendInfo f1; f1.id = 7; f1.iconId = 1; f1.status = 0; f1.nick = "张三"; f1.feeling = "我是张三";
    FriendInfo f2; f2.id = 8; f2.iconId = 2; f2.status = 1; f2.nick = "李四"; f2.feeling = "我是李四";
    assert(st.saveFriend(f1));
    assert(st.saveFriend(f2));

    // upsert：状态从离线改在线
    f2.status = 0;
    assert(st.saveFriend(f2));

    auto friends = st.loadFriends();
    assert(friends.size() == 2);
    bool foundF2 = false;
    for (const auto& f : friends) {
        if (f.id == 8) { foundF2 = true; assert(f.status == 0 && f.nick == "李四"); }
        if (f.id == 7) { assert(f.nick == "张三" && f.feeling == "我是张三"); }
    }
    assert(foundF2);

    // 3. 聊天记录：双向 + 中文 + 时间排序
    assert(st.saveChatMessage(42, 7, true,  "你好张三", 1000));
    assert(st.saveChatMessage(42, 7, false, "你好啊",   1001));
    assert(st.saveChatMessage(42, 7, true,  "在吗？",   1002));
    assert(st.saveChatMessage(42, 8, true,  "李四你好", 1003)); // 另一个会话，不应混入

    auto all = st.loadChatHistory(42, 7, 0);
    assert(all.size() == 3);
    assert(all[0].content == "你好张三" && all[0].outgoing);
    assert(all[1].content == "你好啊" && !all[1].outgoing);
    assert(all[2].content == "在吗？" && all[2].ts == 1002);

    // limit：取最近 2 条，仍按时间升序
    auto recent = st.loadChatHistory(42, 7, 2);
    assert(recent.size() == 2);
    assert(recent[0].content == "你好啊");
    assert(recent[1].content == "在吗？");

    // 4. 关闭重开，数据仍在（持久化验证）
    st.close();
    assert(st.open(dbPath));
    auto persisted = st.loadChatHistory(42, 7, 0);
    assert(persisted.size() == 3);
    st.close();

    std::remove(dbPath.c_str());
    std::cout << "test_storage PASSED" << std::endl;
    return 0;
}
