# 第五轮 P6 C++ SQLCipher 数据底座代码审查报告

> 审查日期：2026-09-09
> 审查对象：`outputs/kernel-round5-acceptance.md` 及当前工作区 P6 实现
> 审查方式：验收声明对照、C++/JNI/Kotlin 静态审查、现有 CTest 复跑
> 范围边界：本报告只给出问题与修复建议，不修改实现代码。

## 1. 总结结论

**当前不建议把第五轮判定为最终完成，也不建议直接进入 P7。**

底层结构方向是正确的：SQLCipher 打开顺序、错误 key fail-close、版本化 Schema、Writer queue、
ReadPool、账号物理隔离、Room 影子迁移和 Android JNI 入口已经形成。但是独立审查发现：

- 4 项 P0 阻断问题；
- 7 项 P1 高优先级问题；
- 3 项 P2 完善项。

其中最关键的不是代码风格，而是验收报告声称的三条不变量实际上尚未成立：

1. ReadPool 并没有保证“一次查询独占一条连接”；池满时会把正在使用的连接再次借出。
2. JNI 的 finish/state/self-test 直接操作 Writer connection，绕过了唯一 Writer 线程。
3. Room 迁移没有导出缩略图字段和 conversation 的 unread/摘要数据，不能证明数据完整迁移。

因此建议将验收结论由“P6 完成”调整为“**主体实现完成，代码审查未通过，待 F01–F11 修复并补回归后收口**”。

---

## 2. 阻断问题（P0）

### F01：ReadPool 在连接全部忙时并发复用同一个 `sqlite3*`

**位置**：

- `client_core/src/storage/ReadPool.cpp:62-93`
- 核心错误在 `ReadPool.cpp:69-82`

**现象**：

`withRead()` 搜索不到空闲 slot 时，没有等待或返回 Busy，而是选择 `m_next` 指向的连接，并再次把
`m_busy[slot]` 设为 true。也就是说，2 条连接被占用时，第 3、4 个线程仍可能同时使用其中一条
`sqlite3*`。

这与 `ReadPool.h:8` 声明的“一个查询在作用域内独占一条连接”直接冲突。即使 SQLCipher 编译为
serialized 模式，也不能把同一连接当成多个独立 lease；statement、错误码、关闭和事务状态都可能
互相影响。

**为什么现有测试没发现**：

`test_read_pool.cpp:112-138` 只统计了有多少线程同时进入回调，没有记录每个 `sqlite3*` 是否被两个
线程同时持有。当前实现反而更容易让 `maxConcurrent >= 2`，所以错误实现会通过测试。TSAN 也无法
把 SQLite 库内部的逻辑并发误用稳定识别为 C++ data race。

**修复建议**：

- 增加 condition_variable；无空闲连接时有界等待，或明确返回 `Busy/Timeout/Closed`。
- 使用 RAII lease，确保正常、异常和提前返回时都归还 slot。
- 增加测试：用 `unordered_map<sqlite3*, atomic<int>>` 记录同一连接的最大并发持有数，必须为 1。
- 增加 pool close 与 active lease 并发测试；close 应停止借出并等待所有 lease 归还后再关闭连接。

### F02：JNI 绕过单 Writer 队列，直接并发使用 Writer connection

**位置**：

- `jitong_android/app/src/main/cpp/im_db_jni.cpp:419-425`
- `jitong_android/app/src/main/cpp/im_db_jni.cpp:437-446`
- `jitong_android/app/src/main/cpp/im_db_jni.cpp:471-498`
- `client_core/include/client_core/storage/NativeDatabase.h:76-77`

**现象**：

`nativeFinishMigration()`、`nativeGetMigrationState()` 和 `nativeRunDatabaseSelfTest()` 通过
`writerHandle()` 直接执行 SQL。与此同时，`nativeSubmitMigrationBatch()` 可能正在 Writer thread 上
执行事务。

这违反了“只有 Writer thread 可使用 Writer connection”的核心不变量。`finish()` 还会写
`migration_meta`，并不只是查询，所以可能与导入事务交叉。

**风险**：

- 同一 connection 跨线程并发调用；
- finish 在最后一个 batch 尚未完成时计算到中间状态；
- completed/state 元数据与 batch/checkpoint 顺序不确定；
- close 与 self-test/状态查询并发时使用已经关闭的句柄。

**修复建议**：

- 删除或严格私有化 `writerHandle()`，禁止 JNI 取得 Writer connection。
- 所有写操作经 `DbCommandQueue`；finish 应作为一个完整 Writer command 提交。
- 状态查询、自检和 summary 走 ReadPool；如果要求 read-after-write，先在 Writer queue 提交 barrier，
  再读取，或将整个操作放 Writer queue 串行执行。
- 增加“batch 执行中并发 finish/state/close”测试。

### F03：迁移 JNI 等待超时后存在迟到回调栈 UAF

**位置**：`jitong_android/app/src/main/cpp/im_db_jni.cpp:358-397`

**现象**：

迁移请求的 `fn` 和 `onDone` 通过 `[&]` 捕获 JNI 栈上的 `msgs`、`cp`、`innerErr`、`res`、mutex、
condition_variable 和 `done`。JNI 只等待 60 秒，但忽略 `wait_for()` 的返回值。若 60 秒超时，函数
离开作用域并返回；Writer 稍后执行或完成时，会访问已经销毁的栈变量。

这是一条真实 UAF 路径。当前 500 条批次通常不足 60 秒，所以常规 ASan 没覆盖到。

**修复建议**：

- 将任务输入和 completion state 放入 `shared_ptr<OperationState>`，回调只捕获值和 shared_ptr。
- 对同步 JNI API，优先使用 `promise/shared_future` 或核心提供的同步 barrier API。
- 超时后设置取消标记；若事务已经开始，允许其整体完成，但不能访问 JNI 栈，也不能把超时当作
  “事务一定失败”。需要 operationId 供调用方查询最终状态。
- 明确检查 `wait_for()` 结果并返回 `Timeout`，不能沿用默认 `NotOpen`。
- 添加一个测试专用慢命令，使等待超时后继续完成，在 ASan 下验证无 UAF。

### F04：Room → Native 迁移丢失缩略图和会话状态，完整性对账无法发现

**位置**：

- Room 完整字段：`jitong_android/app/src/main/java/com/jitong/im/data/db/Entities.kt:39-50`
- 导出 SQL/Row：`jitong_android/app/src/main/java/com/jitong/im/data/MigrationExporter.kt:40-49,91-99`
- 编码：`MigrationExporter.kt:160-173`
- Native 解码：`jitong_android/app/src/main/cpp/im_db_jni.cpp:97-159`
- Native Schema：`client_core/src/storage/migrations/001_initial.sql:32-43,61-69`

**现象**：

Native Schema 和 `MigrationMessage` 支持 small/large thumbnail 的 fileId、path、size、sha256、w/h，
但 Kotlin Exporter 完全没有查询和编码这些字段，JNI decoder 也没有解码，所以迁移后全部变成默认
空值/0。

此外 Exporter 只读取 `messages` 并从消息重建 `conversations`。Room `conversations.unread`、真实
`lastMsg/lastTs` 没有导出；Native upsert 将 `unread/read_seq` 固定为 0。空会话或只存在于
conversation 表中的会话也会丢失。

现有 `MediaFileIntegrityTest` 只验证选定媒体路径/inode/hash，并不能证明全部媒体元数据迁移。
finish summary 只比行数、会话数、全局 seq 区间和 FTS 行数，也检测不到字段内容丢失。

**修复建议**：

- 二进制 batch 增加明确 `formatVersion`，补齐 12 个 thumbnail 字段；JNI 严格按版本解码。
- conversation 单独分页导出，保留 lastMsg、lastTs、unread，以及后续 read_seq 语义；不要只从消息
  猜测。
- 完整性验收增加字段级摘要：按稳定顺序计算消息关键字段 hash、conversation 字段 hash、媒体元数据
  hash；至少做全量计数 + 分层抽样内容对比。
- 补“只有 conversation 无 message”“非零 unread”“大小缩略图字段非空”的 instrumentation fixture。

---

## 3. 高优先级问题（P1）

### F05：迁移三态只存在于类和单测，没有接入正式 JNI 流程

**位置**：

- 状态 API：`client_core/src/storage/MigrationImporter.cpp:312-362`
- 正式入口：`jitong_android/app/src/main/cpp/im_db_jni.cpp:330-426`

`beginImport()` 和 `resetForReimport()` 只被 C++ 单测调用，正式 JNI/SDK 没有入口，也没有在
`nativeSubmitMigrationBatch()` 前校验状态。当前即使状态是 Disabled 或 Verified，仍然可以继续导入；
`nativeFinishMigration()` 也没有要求当前必须是 ShadowImport。

因此验收报告 3.5 所称“拒绝重复迁移”只证明 helper 单测成立，不证明生产链路成立。

**建议**：提供核心级 `startMigration/resetMigration/submitBatch/finishMigration` 编排 API，将状态校验与
数据操作放在同一 Writer 串行域；JNI 只调用用例 API。

### F06：重复 `msg_id` 虽未覆盖消息，却仍可能篡改会话摘要

**位置**：`client_core/src/storage/MigrationImporter.cpp:174-205`

消息 `ON CONFLICT DO NOTHING` 后，即使 `inserted == 0`，代码仍执行 conversations upsert，使用本批
输入的 `content/serverTime` 更新摘要。若相同 msg_id 的重复记录内容不同、时间更大，messages 表保留
旧消息，但 conversation.last_message 被改成并不存在的新内容。

这违反注释中“冲突不覆盖、不能掩盖冲突”的承诺。

**建议**：

- `inserted == 0` 时查询既有消息关键字段并比对；完全一致才视为幂等成功，不一致返回 Conflict 并
  回滚。
- conversation/FTS 只基于真正插入的记录更新；或基于数据库真实最新消息重新计算摘要。
- 增加“同 msg_id 不同 content/serverTime/peer/seq”反例测试。

### F07：finish 的完成标志和 Verified 状态不是原子提交

**位置**：`client_core/src/storage/MigrationImporter.cpp:274-290`

`finish()` 先写 `completed=1`，再写 `state=verified`。两次 upsert 之间没有事务；第二步失败或进程崩溃
会留下 `completed=1 + state=shadow_import/disabled` 的矛盾状态。`resetForReimport()` 的三个写入也
存在同样问题。

**建议**：由 Writer queue 包一个事务提交 summary 校验、completed、state 和最终 checkpoint；失败
全部回滚。为第二次写入注入失败并验证原子性。

### F08：NativeDatabase 的 open/close/submit/read 缺少生命周期同步

**位置**：`client_core/src/storage/NativeDatabase.cpp:38-147`

`m_status` 是 atomic，但 `m_impl->queue/pool/conn` 的创建、读取和 reset 没有同一生命周期锁。
`submit()` 可能在检查 Ready 后与 `close()` 并发，读取已经 reset 的 queue；`withRead()`、
`writerHandle()` 同理。JNI 的 `acquireDb()` 只把 `shared_ptr<NativeDatabase>` 复制出来，并不能阻止另一
线程对同一个对象调用 `close()`。

**建议**：为 NativeDatabase 建立明确状态机和 operation lease。close 先转 Closing、停止新 lease，
等待 active operations/reader leases，关闭 queue/pool/conn 后转 Closed。不要把 atomic status 当完整
生命周期同步。

### F09：ReadPool close 会关闭仍在使用的连接，且 key 未清零

**位置**：

- `client_core/src/storage/ReadPool.cpp:47-54,62-93`
- `client_core/include/client_core/storage/ReadPool.h:48-58`

`withRead()` 执行回调时不持 mutex；`close()` 不等待 `m_busy` 清零，直接 `sqlite3_close` 全部连接，
导致 active query 使用关闭后的连接。另一方面，构造时复制的 `m_key` 在 open 成功后仍长期保留，
析构/close 没有 cleanse；`NativeDatabase::Impl::key` 还重复保存了一份且没有实际用途。

**建议**：close 等 lease 归还；open 完成后立即 `OPENSSL_cleanse` ReadPool 的 key；删除 NativeDatabase
无用 key 副本，或使用 SecureKeyBuffer 并在所有失败路径清零。

### F10：影子库清理忽略删除失败，并可能破坏进程锁互斥

**位置**：`client_core/src/storage/DatabasePaths.cpp:68-81`

函数无条件忽略四次 `remove()` 的错误并返回 true。更严重的是，它会直接删除 `.lock`。如果另一个
进程仍持有旧 lock inode 的 flock，删除文件名后新进程可以创建同名新 inode 并取得另一把锁，两个
进程会同时认为自己持锁。

**建议**：清理前先取得同一 ProcessLock；持锁期间删除 DB/WAL/SHM，但不要先 unlink 正在作为互斥
锚点的 lock 文件。逐项检查 errno，仅 ENOENT 视为成功；失败要上报并验证实际文件状态。

### F11：批次 decoder 可被小输入诱发超大分配，并包含有符号移位 UB

**位置**：`jitong_android/app/src/main/cpp/im_db_jni.cpp:104-160`

问题包括：

- `count` 只检查非负，随后直接 `out.reserve(count)`；一个只有 4 字节、count=INT_MAX 的小数组即可
  请求巨大内存，`bad_alloc` 穿过 JNI 边界可能终止进程。
- `i32()` 在 signed int 上执行 `byte << 24`，高位输入可能超出 int 表示范围。
- `i64()` 使用 signed int64 左移累积，高位/负数编码可能触发有符号溢出 UB。
- 解完 count 条后未检查是否正好消费完输入，尾随垃圾会被接受。

**建议**：使用 `uint32_t/uint64_t` 解码后通过位级转换得到有符号值；限制 count 不超过协议上限
（当前应为 500，且需满足剩余字节最小长度）；捕获 allocation exception；最后要求 reader EOF。
增加 fuzz/畸形 batch corpus，并纳入 ASan+UBSan。

---

## 4. 完善项（P2）

### F12：DbKeyManager 的 key blob 创建不是原子且缺少并发互斥

**位置**：`jitong_android/app/src/main/java/com/jitong/im/data/crypto/DbKeyManager.kt:64-80,104-117`

两个线程首次调用可能各生成 realKey 并竞争 `writeBytes()`；调用者可能分别拿到不同 key，文件也可能
出现部分写。建议按 ownerId 串行化，写临时文件、fsync 后 atomic rename，并设置/验证 0600 权限。

### F13：密钥临时副本清零不完整

**位置**：

- `jitong_android/app/src/main/cpp/jni_db_key_bridge.cpp:71-88`
- `client_core/src/storage/NativeDatabase.cpp:31,87-90,123-126`
- `client_core/src/storage/ReadPool.cpp:10-16`
- `DbKeyManager.kt:104-138`

JniDbKeyBridge 从 Java byte[] 复制 key 后只删除 local ref，没有覆盖 Java byte[]；ReadPool 的 m_key 未
清零；Kotlin `wrapKey`、salt/ciphertext 临时数组也依赖 GC。无法做到 JVM 内存绝对清除，但至少应在
JNI 复制后 `SetByteArrayRegion` 覆盖原数组，C++ 容器使用 SecureKeyBuffer，Kotlin 可控 ByteArray 在
finally 中 `fill(0)`。文档应避免写“用完立即清零”这种当前尚未完全成立的绝对表述。

### F14：路径约束实现弱于接口声明

**位置**：`client_core/src/storage/DatabasePaths.cpp:31-65`

注释要求 filesDir 是绝对路径且库必须位于其下，但实现只拒绝 `..` 段，没有拒绝相对路径、NUL/空白
异常或经过 symlink 跳转的目录。Android 内部传入 `context.filesDir` 时风险较低，但该函数作为核心
安全边界应验证 absolute/canonical path，或把 filesDir 变成创建时注入的可信平台根目录，后续 JNI
不再接受任意字符串。

---

## 5. 验收报告与证据的差异

| 验收声明 | 审查结论 |
|---|---|
| “只读池多个读可以并行且每次独占连接” | 不成立；池满后会并发复用忙连接（F01） |
| “只有 Writer connection/thread 可写” | 不成立；finish 直接在 JNI 线程写 Writer connection（F02） |
| “迁移开关三态接入并拒绝重复迁移” | helper/单测成立，正式 JNI 路径未接入（F05） |
| “媒体字段只搬路径字符串且完整性不变” | inode/hash 测试覆盖部分路径，但缩略图字段未导出（F04） |
| “消息、FTS、会话摘要、checkpoint 原子提交” | batch 内基本成立；finish 的 completed/state 不原子（F07） |
| “close/destroy 每个任务确定完成” | 正常队列 drain 有覆盖，但 JNI 超时回调存在 UAF，close/read lease 也未同步（F03/F08/F09） |
| “ASan+UBSan 21/21” | 历史日志属实；未覆盖慢任务超时、池满同连接和恶意 count/高位整数 |
| “TSAN 无 race” | 仅代表被执行路径未报告 C++ data race，不能证明 SQLite connection lease 语义正确 |

---

## 6. 本次复测

执行：

```text
ctest --test-dir build/client-core-desktop --output-on-failure
```

结果：P6 相关测试全部通过：

- cipher_database
- database_paths
- schema_migrations
- db_command_queue
- read_pool
- room_import
- process_lock
- migration_switch
- kill_recovery

整套 21 项中 19 项通过，integration/transport 两项因当前受限执行环境禁止本地 `bind()` 而失败：

```text
std::system_error: bind: Operation not permitted
```

这两项失败不是本次审查认定的代码回归，也不能用来覆盖或否定上面的静态发现。现有测试通过说明
当前测试断言成立，不代表遗漏的不变量已经成立。

---

## 7. 建议修复顺序与重新验收门禁

### 第一批：并发与生命周期阻断

1. F01 ReadPool 真正 lease/等待/关闭协议。
2. F02 禁止 JNI 直接访问 Writer connection。
3. F03 JNI completion 改为堆上 operation state，正确处理超时。
4. F08 NativeDatabase Closing/operation lease。

门禁：

- 同一 sqlite3* 最大并发 lease 必须为 1。
- batch/finish/state/close 四线程并发压力，TSAN 无报告、ASan 无 UAF、所有 waiter 有确定结果。
- 超时后迟到回调测试在 ASan 下通过。

### 第二批：迁移完整性与原子性

1. F04 补全 thumbnails 和 conversations 导出。
2. F05 正式链接入 Disabled→ShadowImport→Verified。
3. F06 重复 msg_id 内容冲突检测。
4. F07 finish/reset 单事务。

门禁：

- 带 small/large thumbnail、非零 unread、空会话的真实 Room fixture 全字段对账。
- Verified 后 submit 被正式 SDK 拒绝。
- 相同 msg_id 不同内容整体回滚，conversation/FTS/checkpoint 均不变化。
- 在 completed 与 state 写入之间注入失败，重启后不能出现矛盾状态。

### 第三批：安全收口

1. F10 安全清理与锁文件语义。
2. F11 decoder 上限、无符号解码和 fuzz。
3. F12–F14 原子 key blob、密钥清零和可信路径。

门禁：

- 清理失败必须可观察；持锁进程存在时另一进程不能通过删除/重建 lock 绕过互斥。
- 畸形 count、负长度、截断、尾随垃圾、超长字符串在 ASan+UBSan 下稳定拒绝。
- 首次 key 创建并发 100 次只产生一把可复开数据库的 key，不能出现 blob 半写。

## 8. 建议最终判定

**Review result：CHANGES REQUIRED。**

第五轮完成了可审查的 P6 主体，但当前不满足最终验收。F01–F04 为进入 P7 前必须修复的阻断项；
F05–F11 应与本轮一并收口，否则 P7 消息 Outbox/Inbox 会建立在不可靠的连接租约、Writer 所有权和
迁移数据基础上，后续修复成本会显著放大。
