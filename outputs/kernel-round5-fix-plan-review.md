# 第五轮 P6 修复方案审查报告

> 审查对象：`outputs/kernel-round5-fix-plan.md`
> 审查日期：2026-09-09
> 审查范围：方案正确性、任务依赖、可执行性和验收充分性；不修改实现代码。

## 1. 结论

**方案总体可行，问题覆盖完整，但不能直接原样执行，建议修订后实施。**

方案正确承接了 F01–F14，分成“并发与生命周期 → 迁移完整性与原子性 → 安全收口”的顺序也基本
合理。ReadPool lease、取消 Writer 裸句柄、堆上 completion state、全字段迁移、状态机接入、原子
finish、decoder 加固和 key blob 原子写这些方向都正确。

但当前方案仍有 5 项方案级阻断缺口和 7 项需要补强的细节。如果直接照写，可能修复旧问题的同时
引入新的死锁，或者让迁移在测试夹具上通过、在 Legacy 持续收消息的真实环境中仍无法得到一致数据。

建议判定：**APPROVE WITH REQUIRED REVISIONS（修订后可执行）**。

---

## 2. 方案中正确、可以保留的部分

1. F01/F08/F09 合并处理是对的。ReadPool 的租约、NativeDatabase 生命周期和 close 顺序属于同一
   个所有权问题，不能拆开各修一半。
2. 删除 `writerHandle()` 并把 finish 放进 Writer command 是对的，能恢复“唯一 Writer owner”。
3. JNI completion 使用 `shared_ptr<OperationState>` 是正确方向，能消除超时后的栈 UAF。
4. batch 增加版本号、补全缩略图字段、单独迁移 conversations 是必要改动。
5. 重复 msg_id 由“静默跳过”提升为“相同则幂等、不同则冲突”符合数据迁移语义。
6. finish/reset 借用 Writer queue 的外层事务是对的；Importer 内不能再嵌套 `BEGIN`。
7. decoder 使用无符号整数、行数/长度上限、EOF 校验和 fuzz 是正确的安全收口。
8. `.lock` 清理、key blob 原子写和可信根路径都应该在 P6 收口，而不是推给 P7。

---

## 3. 必须修订的方案级问题

### R01：`shared_mutex` 持锁执行 `queue.close()/join` 会产生新的重入死锁

**对应原方案**：第一批 F08/F09/F01 第 1 条。

原方案建议 `close()` 持 `unique_lock`，并在锁内执行 queue close/join、pool close、connection close。
这会出现以下死锁：

1. close 线程取得 lifecycle unique lock，调用 queue.close 并等待 Writer thread join；
2. Writer 正在执行 `onDone` 或 invalidation callback；
3. callback 重新调用 NativeDatabase 的 `submit()/withRead()/status`，等待 shared lock；
4. unique lock 等 Writer 退出，Writer 等 shared lock，双方永久等待。

`open()` 如果持 unique lock 后再调用现有 `close()`，还可能发生同线程重复加锁死锁。

**必须改成两阶段关闭**：

- 锁内：`Ready → Closing`，从成员中 move 出 queue/pool/conn/lock 所有权，禁止新 operation/lease；
- 锁外：关闭 queue 并 join、等待 ReadPool lease、关闭连接；
- 锁内：清理最终状态并置 `Closed`。

或者让 NativeDatabase 由一个专属 serial executor 管理生命周期，但不能持生命周期锁等待外部线程或
执行用户回调。需要增加 callback 重入 close/submit 的专门测试。

### R02：修复方案没有处理“Legacy 迁移期间仍持续写入”的一致快照

**对应原方案**：第二批 F04/F05/F07。

当前默认 Backend 仍是 Legacy，迁移又按多批从 Room 导出。若导出过程中继续收到/发送消息：

- 开始统计、消息分页、conversation 导出和最终 summary 看到的可能不是同一个数据版本；
- 某条旧消息更新状态或缩略图路径时，基于 `id > checkpoint` 的游标不会再次导出更新；
- conversation.unread/lastMsg 可能比已导出的 messages 更新，字段 hash 永远对不上；
- “最后对账通过”只能是偶然时刻的结果，不能证明可切换。

**方案必须增加源端一致性策略，推荐二选一并明确采用哪一个**：

1. **短暂停写 + 一致事务快照**：切换前暂停 Legacy DB 写入，在 Room read transaction 中冻结快照，
   完成导出/对账后切 Backend；适合数据量较小、停顿预算可接受的场景。
2. **高水位全量 + 增量追平**：开始迁移记录 message id/更新版本高水位；全量只导出至高水位；Legacy
   的新增和更新通过 change log/outbox 记录；全量结束后循环应用 delta，最终短暂停写、应用最后一段
   delta、对账并原子切换。

当前项目已有 10 万级数据，推荐第二种。只增加字段 hash 不解决移动数据集问题。

### R03：新增 conversations 流后，必须定义多流 checkpoint 和完成协议

**对应原方案**：F04 第 3 条。

原方案提出单独 `exportConversations`，但仍只有一个字符串 checkpoint。消息流和会话流的游标、完成
状态、重试边界不同，不能共享一个模糊 checkpoint。

建议迁移元数据至少包含：

- `migration_epoch/format_version/source_high_watermark`；
- `messages_checkpoint`；
- `conversations_checkpoint`；
- 如采用增量追平，再加 `delta_checkpoint`；
- 每个 stream 的 `started/completed`；
- 最终 `verified/cutover_ready`。

每一批数据与对应 checkpoint 必须在同一 Native Writer 事务提交。finish 只能在所有 stream completed、
source snapshot/epoch 一致且字段摘要通过后置 Verified。

### R04：`.lock` 文件不应该保留“或最后再删”的选项

**对应原方案**：F10。

只要 lock 文件被 unlink，就存在释放/重建之间的竞态；持有旧 inode 锁的进程与打开新 inode 的进程可
同时成功。因此正式方案应该明确：

> `.lock` 作为稳定互斥锚点永久保留，不参与影子库清理；只清 DB、WAL、SHM。

如果业务强制要求删除 lock 文件，需要额外的目录级锁或不变 inode 机制，复杂度没有必要。测试应
验证清理后 lock 文件仍存在且第二进程始终无法绕过。

### R05：DbKeyManager 判断 key 丢失时只检查 Room 库，方案没有补 Native 库

**对应原方案**：F12。

当前 `DbKeyManager.getOrCreateRealKey()` 在 key blob 不存在时只检查 `jitong_<ownerId>.db`。若 Room 库
不存在、但 `filesDir/native_db/account_<ownerId>.db` 已存在，它会生成新 realKey 并写入 blob；旧
Native 密文库随后无法打开，新 blob 还覆盖了恢复语义。

F12 除并发锁和 atomic rename 外，还必须把“已有密文数据”定义为：

- Room DB/WAL/SHM 任一存在；或
- Native DB/WAL/SHM 任一存在。

任何一个存在而 key blob 缺失都返回 `DatabaseExistsWithoutKey`，不得生成新 key。更稳妥的是由统一
`DbKeyPlatform` 接收受信任的数据库存在性查询，避免 Kotlin 与 C++ 各自拼路径。

---

## 4. 需要补强的实施细节

### R06：ReadPool 需要明确 API 结果和公平性，不能只返回 bool

建议结果至少区分 `Ok/Busy/Timeout/Closing/CallbackFailed`。等待必须有 deadline；无限等待会让 JNI
或关闭流程永久悬挂。close 应唤醒所有 waiting borrower，让它们返回 Closing，同时只等待 active
lease，不等待已经被唤醒的 waiter 再次借出。RAII guard 的析构不能抛异常。

### R07：`withRead` 回调抛异常也必须可控

当前类型是 `std::function<void(sqlite3*)>`，调用方理论上可以抛异常。方案只说 RAII 归还连接，还应
规定异常不能穿过 JNI/Writer thread：核心边界 catch，归还 lease，转换成明确错误。生产代码最好
使用返回 `DbResult` 的 no-throw callable 约定。

### R08：state/self-test 的多条查询要处于同一只读快照

原方案称 state/self-test 直接走 `withRead`。这只能避免写连接并发，不保证多条 SELECT 之间的视图
一致。应在同一只读 lease 上显式 `BEGIN`/读取全部字段/`COMMIT`，或者作为 Writer queue barrier 后的
单个控制命令执行。不能“实测看得到就算”，需要定义 read-after-write 和 snapshot 语义。

### R09：F03 不应让 JNI 超时后依赖模糊的 operationId 补偿

P6 迁移是本地控制流，优先设计为：

- Kotlin 在 IO dispatcher 调用异步 Native API；
- Native completion 通过一次性 callback/future 返回；
- 生命周期可取消，但事务开始后等待最终完成；
- SDK 不在 JNI 栈上固定阻塞 60 秒。

如果保留同步 `submitAndWait`，超时只能表示“调用方停止等待”，不能表示数据库操作失败。必须返回
`TimedOutButMayCommit` 之类的状态，并允许按 operationId 查询；否则上层重试会与仍在运行的事务竞态。
`CommandResult* onTimeout` 这个接口形状不清晰，建议在写代码前先定义状态模型。

### R10：重复 msg_id 必须比较全部持久化语义，不能只比较四个字段

原方案只列 `peer_id/seq/content/server_time`。还需要比较 conversation、type、from_me、status 和全部
媒体/缩略图元数据，或者在源端生成 canonical row digest，Native 对既有行计算同一 digest。需明确
哪些字段允许单调更新，例如“发送中→已送达”；迁移幂等与业务消息状态合并不能混在同一套规则里。

### R11：batch version 应有 magic、版本和严格总量预算

只在开头加入整数 version，旧无版本数据和畸形 count 仍可能混淆。建议头部至少包含：

```text
magic | format_version | stream_type | row_count | payload_length
```

每个字符串设置单字段上限，整批设置累计字节预算；`row_count <= 500` 之外还要避免 500 个 1 MiB 正文
突破 8 MiB。解析使用 checked arithmetic，先验证再分配。若只在 JNI 内部使用，也应有稳定格式和
Golden 编解码测试。

### R12：key blob 原子写需要说明 Android 实现细节

“atomicMove”需要结合 minSdk。建议同目录临时文件、`FileOutputStream.fd.sync()`、校验长度后
`renameTo`/平台原子替换；失败时删除临时文件但保留旧 blob。owner lock 应覆盖“检查 DB 是否存在→
生成→写临时文件→rename→返回 key”全过程。还需测试进程被杀在 write/fsync/rename 各阶段后的恢复。

### R13：可信路径应选“初始化注入一次”，不要把 realpath 当主要方案

目标 DB 和 `native_db` 目录在首次创建时可能不存在，直接对完整目标 `realpath()` 会失败；仅做字符串
canonical 又挡不住 symlink race。建议采用原方案中更彻底的方式并提升为正式决策：SDK 创建时从
Android 内部取得可信 `filesDir`，Native 保存 canonical root；后续数据库 API 只接收 ownerId，使用
`mkdirat/openat` 和 `O_NOFOLLOW`（平台支持处）在可信目录下创建。不要继续让普通业务 JNI 传任意
filesDir 字符串。

---

## 5. 建议调整后的执行顺序

### 阶段 A：先冻结 API 与状态模型

1. 定义 NativeDatabase 状态：`Closed/Opening/Ready/Closing/Locked`。
2. 定义 operation lease、ReadLease、队列 completion/timeout 结果。
3. 删除公开 writerHandle，定义 begin/submit/finish/reset/query 的核心用例 API。
4. 定义迁移 wire format 和多流 checkpoint/epoch。

这一阶段先写接口和状态转移测试，不急着改全部实现。否则并发修复、JNI 修复和迁移协议会反复改接口。

### 阶段 B：修并发与生命周期

执行 F01/F02/F03/F08/F09，并纳入 R01、R06–R09。

必须先通过：

- 同一 read connection 最大并发 lease 为 1；
- callback 重入不会导致 close/join 死锁；
- active read + close 无 UAF；
- batch/finish/state/close 并发所有 waiter 都得到确定结果；
- 超时/取消/迟到完成状态可解释。

### 阶段 C：修迁移协议和一致性

执行 F04/F05/F06/F07，并纳入 R02、R03、R10、R11。

必须先选定并实现源端一致性策略。建议“高水位全量 + delta 追平 + 最终短暂停写”，再补：

- thumbnails 全字段；
- conversations 独立流；
- 非零 unread 与空会话；
- 更新过的旧消息；
- 多流 checkpoint 和 epoch；
- canonical row hash；
- Verified 前所有流/增量/摘要完整。

### 阶段 D：安全与文件系统收口

执行 F10–F14，并纳入 R04、R05、R12、R13。

### 阶段 E：重新验收

重新生成报告和日志，不能只在原报告上追加一句“已修复”。验收至少包括：

1. CTest normal、ASan+UBSan、TSAN 分别构建运行；
2. decoder fuzz/畸形 corpus；
3. Android arm64 instrumentation + x86_64 构建；
4. CheckJNI；
5. Legacy 持续写入条件下的影子迁移追平测试；
6. kill-recovery 覆盖 message/conversation/delta 多 checkpoint；
7. 数据字段 hash、媒体 inode/hash、未读和空会话对账；
8. 第二进程与 clearShadow 并发锁测试；
9. key blob 并发首次创建和各落盘阶段强杀恢复。

---

## 6. 对原方案门禁的修改建议

原方案三个批次门禁应追加以下阻断条件：

| 批次 | 必须追加的门禁 |
|---|---|
| 第一批 | callback 重入 close 无死锁；close 不持 lifecycle lock join；read waiter 有界超时并区分 Closing |
| 第二批 | Legacy 并发写入下可追平；消息/会话/delta 各自 checkpoint；旧消息更新不遗漏；同一快照字段摘要一致 |
| 第三批 | Native DB 存在而 key blob 缺失时 fail-close；lock 文件永久保留；key blob 写入各阶段强杀可恢复 |

---

## 7. 最终建议

这份修复方案不是推倒重来，约 70% 内容可以直接保留。实施前应先修订以下五点：

1. NativeDatabase 改为两阶段 close，禁止持 lifecycle lock join/callback。
2. 明确 Legacy 持续写入时使用“高水位 + delta + 最终短暂停写”或一致事务快照。
3. 为 message/conversation/delta 定义独立 checkpoint、epoch 和完成条件。
4. lock 文件永久保留，不采用“最后再删”。
5. DbKeyManager 同时检测 Room 与 Native 密文库存在性。

补齐后，这份方案可以落地，并且修复顺序适合继续执行。未补齐前不建议开始大规模代码修改，尤其
不要先改 JNI 签名和迁移二进制格式，否则后面加入多流 checkpoint 与 delta 时会二次返工。

---

## 8. 修订版 v2 复审补充（2026-09-09）

### 8.1 复审结论

当前 `kernel-round5-fix-plan.md` 已更新为 v2，并吸收了本报告此前提出的五项方案级修订：

- NativeDatabase 两阶段关闭；
- 高水位全量 + delta 追平 + 最终短暂停写；
- message/conversation/delta 多流 checkpoint 与 epoch；
- `.lock` 永久保留；
- key 丢失检测同时覆盖 Room 与 Native 密文库。

新方案已经不存在需要推翻重写的架构问题。复审判定由“修订后可落地”提升为：

> **CONDITIONALLY APPROVED——方案成熟度约 85%；补齐下面四项编码前置决议后可以进入实施阶段。**

### 8.2 编码前必须补齐的四项决议

#### ADR-FIX-01：禁止 Writer self-join

两阶段关闭虽然避免了持 lifecycle lock 执行 join，但 Writer 的 `onDone`/invalidation callback 如果直接
调用 `close()`，仍可能由 Writer thread 对自身执行 `join()`。

推荐规定：Writer thread 不直接执行可能重入数据库的用户回调，统一投递到独立 observer/executor；
数据库生命周期由 owner executor 关闭。如果必须允许 Writer 发起关闭，只能发停止请求，最终 join 和
资源回收交给其他线程完成，不能用 detach 规避。

同时，queue 必须先完成内部状态更新和 `pending` 递减，再向外投递 completion；任何外部回调都不得在
queue mutex 或 lifecycle lock 内执行。

#### ADR-FIX-02：Closing 只能有一个 shutdown owner

两阶段关闭在锁外执行 join 时，其他线程可能再次调用 `open/close`。需要明确：

- 只有成功完成 `Ready → Closing` 状态转移的线程负责真实 teardown；
- 其他 close 共用同一个 close completion，或幂等返回；
- open 遇到 Closing 时有界等待或返回 Busy，不能创建第二个 Impl；
- 每轮 open/close 带 lifecycle generation，旧 teardown 不能把新连接覆盖为 Closed；
- queue、pool、connection 和 ProcessLock 组成一个 teardown bundle，固定关闭顺序。

当前 `ProcessLock` 不可复制也不可移动。如果实现要把 lock 从成员中 move 出，需要补安全 move 语义，
或把整组资源放入 `unique_ptr<DatabaseResources>` 后整体摘除。

#### ADR-FIX-03：区分 SnapshotInsert 与 DeltaUpsert

v2 一方面要求重复 msg_id 全字段 digest 不同即 Conflict，另一方面要求 delta 合法更新旧消息的发送
状态、路径和缩略图。如果共用一种冲突规则，合法的“发送中→已送达”会被误判为数据冲突。

API 和 wire format 必须区分：

- `SnapshotInsert`：不存在则插入；已存在时不可变字段和 canonical snapshot digest 必须一致；
- `DeltaUpsert`：携带 `source_version/change_seq`，只允许更新明确列出的 mutable 字段；
- 重复 change_seq 幂等跳过，旧 change_seq 不得覆盖新状态；
- owner/msg_id/sender/conversation 等 immutable 字段变化始终返回 Conflict。

conversation delta 也必须带 source version/change seq，否则旧 unread/lastMsg 更新可能覆盖新状态。

#### ADR-FIX-04：key 创建锁必须跨进程

v2 中的 owner lock 不能只使用 Kotlin `synchronized` 或进程内 map。Android 多进程组件可能同时首次
创建 key blob，锁必须覆盖：

```text
检查 Room/Native DB → 检查 blob → 生成 key → 写临时文件 → fsync → 原子 rename → 返回 key
```

建议使用永久存在的账号级旁路锁文件和跨进程文件锁。验收必须包含第二进程并发首次创建，而不仅是
100 个线程并发。

### 8.3 v2 仍需补强的任务细节

#### 1. 把 Legacy change log 列为明确子任务

“高水位 + delta”需要实际增加 Legacy change log Schema，并让所有消息、会话、FTS、未读、图片下载
和状态更新入口与 change log 在同一个 Room 事务提交。阶段 C 应明确包括：

1. `legacy_change_log(change_seq, stream, key, op, source_version)`；
2. 全部 Legacy 写入口接入；
3. migration epoch/high-water 建立；
4. delta 分页导出和 Native 幂等应用；
5. Verified/cutover 后 change log 保留与清理。

否则“持续写入时可追平”仍然只是一句目标，没有实现闭环。

#### 2. ReadPool close 要覆盖 waiter 生命周期

close 不仅要等待 active lease，还要保证所有 condition_variable waiter 已观察到 Closing 并安全退出。
可以让 waiter 持共享 ControlBlock，并等待 `active == 0 && waiters == 0`；不能在 waiter 正从 wait 返回时
销毁 pool/cv。

#### 3. 异步 JNI completion 要补 Java 生命周期

若采用 callback，需要定义 GlobalRef、线程 attach/detach、handle generation、Java 异常清理和 SDK
destroy 顺序。推荐最终形成 Kotlin `suspend` API + requestId + 一次性 Native completion，并在统一 SDK
event executor 转发。不能把无人管理的 C++ future 裸句柄交给 Java。

#### 4. wire format 需要明确技术选型

项目已经使用 protobuf。实施前应记录继续自定义二进制格式还是复用 protobuf：protobuf 更利于未知
字段和版本演进，但要设置解析总量上限；自定义格式更紧凑，但 parser、兼容和 fuzz 责任全部由项目
承担。如果保留自定义格式，至少固定：

```text
magic | format_version | stream_type | row_count | payload_length
```

并维护 Kotlin encoder/C++ decoder Golden corpus。`format_version` 与 `migration_epoch` 不能混用。

#### 5. 先补 Android 真机性能基准

验收中提到的 10 万消息约 1.9 秒不能直接证明 Android 上停写不可接受。执行 delta 方案前应在最低目标
设备测量 Room→JNI→Native SQLCipher 全链路：总耗时、每批 P50/P95/P99、峰值 RSS、delta 追平速度和
最终停写窗口。这既能验证选择 delta 的必要性，也能为回滚阈值提供数据。

### 8.4 v2 最终执行门禁补充

除 v2 已有门禁外，还应增加：

- callback 重入 close 不会 self-join，所有 completion 恰好一次；
- 两个 open/close 并发时只有一个 teardown owner，旧 generation 不覆盖新状态；
- SnapshotInsert 与 DeltaUpsert 的重复、倒序、缺洞和 immutable 冲突测试；
- Legacy 所有写入口均产生 change log，迁移失败后 Legacy 搜索、收发和未读仍可使用；
- ReadPool 关闭时 active reader 和 waiting borrower 均安全退出；
- Android 10 万消息 + conversation + thumbnail + delta 全链性能和内存指标；
- key blob 多进程首次创建及 write/fsync/rename 各阶段强杀恢复；
- 自定义 wire format 的截断、尾随垃圾、超大字段、未知版本和 fuzz 测试。

### 8.5 v2 最终判定

v2 已经从“方向正确但存在方案阻断”提升为“架构可以落地、并发和增量语义需要最后冻结”。不需要
再次重写整份计划。将 ADR-FIX-01～04 追加进原修复方案，并把本节任务与门禁落实后，即可开始阶段 A。

---

## 9. 第六轮实现代码审查（2026-09-09）

### 9.1 结论

**结论：REQUEST CHANGES，第六轮暂不能按 `kernel-round6-acceptance.md` 的“APPROVED / P6 完成”结论放行。**

本轮的 SQLCipher、Schema、FTS、ReadPool 独占租约、消息与会话批量导入、进程锁及基础生命周期测试均已
落地，方向正确；但代码仍存在 2 个 P0 和多项 P1。尤其是并发 open/close、Writer 回调重入、双流迁移
完整性、导出侧会话摘要、跨进程 key 创建和真正的增量高水位没有闭环。当前实现适合作为 shadow import
实验版本，不能进入事实源切换。

### 9.2 已执行验证

执行了桌面端重新编译及第六轮相关 10 组测试：

```text
cipher_database / database_paths / schema_migrations / db_command_queue /
read_pool / room_import / process_lock / migration_switch /
kill_recovery / native_database_lifecycle
```

结果：**10/10 通过，总耗时 49.55 秒**。这些结果能证明已有用例内的功能成立，但没有覆盖下面列出的
并发重入、双流完成、空会话摘要、跨进程首次建 key 和在线 delta 场景，因此不能据此宣称 P6 完整闭环。

### 9.3 阻断问题

#### [P0] R03 open/close 状态机没有真正实现

`NativeDatabase::open()` 先调用 `close()`，随后在生命周期锁外取得进程锁、打开连接、迁移 Schema，最后
才挂载资源；代码从未进入 `Opening`。并发的第二个 open/close 因而可以释放或覆盖另一个 generation 的
进程锁、连接与状态。`close()` 也没有唯一 teardown owner：两个 close 可同时进入，后进入者可能先置
`Closed`，而前一个 teardown 尚未完成；期间的新 open 又可能被旧 close 释放锁并覆盖状态。

位置：`client_core/src/storage/NativeDatabase.cpp:87-155,175-200`。

修复要求：落实上一版 ADR-FIX-02，引入 generation + 唯一 teardown owner；把 ProcessLock 与连接资源作为
同一 generation 的资源包整体移交，旧 generation 不得写回新状态。补并发 open/open、open/close、
close/close 以及旧 teardown 与新 open 交叠测试。

#### [P0] Writer 回调重入 close 仍会 self-join

Writer 线程直接执行 invalidation sink 和 `onDone`，而 `close()` 无条件 `join()` Writer。任一回调重入
`NativeDatabase::close()` / `DbCommandQueue::close()` 都可能在线程中 join 自己。重复 close 分支还在持有
`m_mutex` 时调用用户回调，既可能重入死锁，也没有按残留请求逐项维护 pending；`fn/sink/onDone` 抛异常
还会直接终止 Writer。

位置：`client_core/src/storage/DbCommandQueue.cpp:41-63,81-125`。

修复要求：落实 ADR-FIX-01；Writer 先提交/回滚、更新 pending 和内部状态，再把 completion 投递到独立
executor；回调不得运行在队列互斥锁或 Writer join 路径上。所有用户函数入口捕获异常，并测试回调重入
close、重复 close、异常回调及 completion 恰好一次。

### 9.4 高优先级问题

#### [P1] 迁移状态查询不是同一快照

`queryMigrationState()` 连续执行 completed/state/checkpoint/summary 四组查询，却明确不使用读事务。在 WAL
并发写入期间，这些字段可能分别来自不同提交，返回一个从未真实存在过的混合状态。只读连接可以建立
`BEGIN` 读事务；不能用“每条 SELECT 自身是快照”替代“一组字段同一快照”。

位置：`client_core/src/storage/NativeDatabase.cpp:446-482`。

修复要求：同一只读连接上 `BEGIN`，完成全部读取后 `COMMIT`；任一查询失败则回滚并整体失败。`runSelfTest()`
也必须检查各查询返回值，不能无条件 `out.ok = true`。

#### [P1] 会话摘要会让含空会话的真实迁移无法完成

Android 导出侧用 `count(DISTINCT conversationId) FROM messages` 计算会话数，Native 对账侧用
`count(*) FROM conversations`。存在没有消息但带草稿/未读/置顶状态的空会话时，两端必然不一致。
Android 测试手工传入 `conversations=2`，绕开了生产 `MigrationExporter.summary()`，所以未捕获该问题。

位置：`jitong_android/app/src/main/java/com/jitong/im/data/MigrationExporter.kt:83-94`、
`client_core/src/storage/MigrationImporter.cpp:406-423`。

修复要求：导出摘要直接统计 `conversations` 表，并让测试使用生产 summary；另外读取并断言空会话的
unread、lastMsg、lastTs 等实际字段，而不只是断言行数。

#### [P1] 双流没有完成标记，finish 可在导入不完整时误置 Verified

`finish()` 只比较总数/min/max，然后写 `completed=1` 和 `Verified`；没有 messages/conversations 两条流的
epoch、完成标记和各自 checkpoint。若目标库历史数据使摘要碰巧相同，即使某条流未执行完也可通过。
`resetForReimport()` 又只清 `checkpoint`，没有清 `conversations_checkpoint` 与 `source_high_watermark`，重试
会继承旧元数据。

位置：`client_core/src/storage/MigrationImporter.cpp:426-444,500-519`。

修复要求：增加 migration_epoch、每流 completed/checkpoint；finish 必须核对同一 epoch 下两流完成与摘要。
reset 要原子清理本 epoch 的全部 checkpoint/high-water/完成信息。

#### [P1] `source_high_watermark=maxSeq` 不是可用的增量高水位

`conversation_seq` 是会话内序号，不同会话会重复，且无法表示编辑、删除、未读变化和 FTS/媒体状态更新；
因此 `maxSeq` 不能作为全局增量游标。本轮验收报告也承认在线 delta 要到 P13 才实现，这与“R02 已闭环、
P6 完成”的结论矛盾。

位置：`client_core/src/storage/MigrationImporter.cpp:440-444`。

处理要求：P6 文档应明确标记为“基础快照迁移完成、禁止切换事实源”；P13 使用 Legacy change log 的单调
`change_seq` + migration epoch + 独立 stream 游标，而不是复用 conversation seq。

#### [P1] key 创建锁仍是进程内锁，且显式清理顺序会遗失密钥

`ConcurrentHashMap<Int, Any>` 只能串行同一 JVM 进程；两个 Android 进程仍会同时使用同一个 `.tmp` 并各自
生成 key。Room 数据存在检查只检查主 `.db`，没有检查 Room 的 `-wal/-shm`。`clearLocalDatabase()` 又先删
key blob、后删数据库；数据库删除失败时会留下永远无法解密的数据，同时 Native 影子库也没有被清理。

位置：`jitong_android/app/src/main/java/com/jitong/im/data/crypto/DbKeyManager.kt:54-93,111-155`。

修复要求：使用账号级永久 lock 文件 + `FileChannel.lock()` 覆盖检查、生成、fsync、rename 全阶段；临时文件
名包含进程/随机标识并 fsync 父目录。检查 Room/Native 的 DB/WAL/SHM。显式清理先关闭两库并确认 DB/sidecar
清理完成，最后删 key；任一步失败保留 key 并返回失败。

#### [P1] ReadPool 打开失败仍把数据库报告为 Ready

只读池任一连接打开失败时，`open()` 仍挂载该 pool、设置 `Ready` 并返回 true；随后迁移查询和自检必然失败，
状态对外却显示可用。当前状态枚举没有表达“仅写可用”的 degraded 状态。

位置：`client_core/src/storage/NativeDatabase.cpp:139-155`。

修复要求：当前 P6 依赖读池，应 fail-close 并释放本 generation 全部资源；若确实允许降级，则新增明确状态、
能力位和恢复路径，不能伪装 Ready。

### 9.5 次要但应纳入本轮修复的问题

- `ReadPool::size()` 未加锁，与 open/close 并发读取 `m_conns` 会数据竞争；头文件还应直接包含
  `<condition_variable>`。位置：`client_core/include/client_core/storage/ReadPool.h:18-23,58`。
- `ReadPool::close()` 只等待 active lease，没有追踪 condition-variable waiter 生命周期。Native 当前通过
  `shared_ptr` 间接降低风险，但公开类被直接销毁时仍缺少可证明的 waiter 退出条件。
- `DatabasePaths` 注释声称拒绝空白但代码未实现；更重要的是它仍接受 JNI 传入的任意绝对路径，未落实受信
  root 固化和 symlink/openat 边界。位置：`client_core/src/storage/DatabasePaths.cpp:34-77`。
- snapshot 重复消息只比较 messages 主表，不校验/修复对应 FTS content/pinyin/initials；FTS 缺行或旧拼音
  仍会被当作幂等成功。摘要只比 FTS 行数也发现不了内容错误。
- JNI parser 需要补 null batch 检查及顶层 `try/catch`，避免 raw JNI 调用或内存异常让 C++ 异常越过 JNI
  边界。

### 9.6 放行门禁

完成以下条件后再将 P6 标记为 APPROVED：

1. 修复两个 P0，并新增真实生产入口的并发 open/close 与 callback self-close 测试；
2. 修复同快照查询、生产 summary 空会话不一致、双流完成标记和 reset 元数据；
3. 将增量迁移明确降级为 P13 未完成能力，P6 阶段禁止 cutover；
4. 完成跨进程首次建 key 和失败阶段恢复测试，调整显式清库顺序；
5. 重新执行 desktop 22/22、ASan+UBSan、Android arm64 真机/AVD、CheckJNI 与 shadow migration e2e；
6. 验收报告分别列出“代码已实现”“测试已覆盖”“后续轮次延期”，不得把预留字段写成能力闭环。

---

## 10. 第六轮审查问题修复复核（2026-09-10）

本节记录第 9 章问题的实际修复，不覆盖或删除原始审查证据。

### 10.1 已修复

- **P0 open/close**：新增完整生命周期互斥，open/close 的资源创建、移交和 teardown 不再交叠；状态真实进入
  Opening；补并发 open/close 测试。
- **P0 Writer 重入**：事务结果与 pending 先结算，sink/onDone 改由独立 completion executor 串行投递；
  callback 不在 Writer 或队列锁内运行；WriteFn/callback 异常被隔离；补 callback→close 回归。
- **读一致性**：migration state 与 self-test 使用同一只读事务快照；失败不再无条件报告 ok。
- **双流迁移**：begin/reset 初始化或清理两条 checkpoint/完成状态；finish 在非空流缺 checkpoint 时拒绝 Verified。
- **摘要**：Android 使用 conversations 表计数，保留空会话语义。
- **增量表述**：删除 `source_high_watermark=maxSeq` 的错误语义，仅记录 snapshot max conversation seq；真正
  全局 change_seq/epoch/delta/cutover 仍归 P13。
- **密钥**：增加 FileChannel 跨进程 owner 锁、唯一 tmp 文件、Room/Native sidecar 检查；显式清理在数据文件
  完全删除后才删除 key blob。
- **ReadPool**：size 加锁；close 等待 active lease 与 waiter；读池打开失败不再伪装 Ready。
- **FTS**：Schema v2 增加 `message_fts_identity`，支持按 owner/msg_id O(1) 核对 content/pinyin/initials，并有
  v1→v2 回填测试。
- **JNI**：批次数组 null 校验、RAII 释放以及 C++ 异常边界已补齐。

### 10.2 当前准确判定

**P6 基础快照影子迁移可以验收，但不具备事实源切换条件。** P13 的 Legacy change log、全局单调
change_seq、migration epoch、delta 追平和原子 cutover 未实现，必须继续保持 Room 为事实源。

本机验证：相关桌面回归 6/6；全量 CTest 22/22（端口用例在允许 bind 环境补跑）；ASan+UBSan 端口补跑
4/4；Android Kotlin 与 Native arm64-v8a/x86_64 构建成功。由于当前无 AVD 连接，本次变更后的 Android
instrumentation/CheckJNI、多进程首次建 key 真机用例仍是合入前门禁。
