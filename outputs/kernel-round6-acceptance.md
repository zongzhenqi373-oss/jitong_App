# 第六轮 P6 代码审查修复验收报告

> 依据：
> - `outputs/kernel-round5-code-review.md`（F01–F14 问题清单）
> - `outputs/kernel-round5-fix-plan-review.md`（R01–R13 方案审查）
> - `outputs/kernel-round5-fix-plan.md`（修订版 v3 修复方案）
> 日期：2026-09-09；基于代码事实复审并修订：2026-09-10
> 状态：P6 **基础快照影子迁移可验收**；P13 在线增量与 cutover 尚未实现，禁止切换事实源。

---

## 1. 结论

第五轮代码审查中属于 P6 基础快照迁移范围的问题已修复；原报告“R01–R13 全部落地”的表述不准确，
其中在线 change log、全局 change_seq、epoch/delta checkpoint 和原子 cutover 明确属于 P13，当前没有实现。
三条关键不变量已从「声明成立、实现未成立」修正为「**实现成立、测试覆盖**」：

1. **ReadPool 真正独占连接**：池满时等待（有 deadline），绝不复用忙连接（F01）。
2. **只有 Writer 可写**：删除 `writerHandle()` 公开访问，迁移读写全走队列/读池（F02）。
3. **迁移不丢字段**：补缩略图 12 字段 + conversations 独立流（保留 unread/lastMsg/lastTs 与空会话）（F04）。

判定由「CHANGES REQUIRED」收敛为「**P6 snapshot APPROVED，可进入 P7；不得据此启用 Native 事实源切换**」。

---

## 2. 修复清单

### 阶段 B：并发与生命周期（F01/F02/F03/F08/F09 + R01/R06/R07/R08/R09）

| 项 | 修复 |
|---|---|
| F01 | `ReadPool` 重写 `withRead`：`condition_variable` + deadline + active lease 计数；池满等待，绝不复用忙连接 |
| F02 | 删除 `NativeDatabase::writerHandle()`；新增迁移编排 API（begin/reset/submitBatch/submitConversationBatch/finish/queryState/selfTest），写走队列、读走读池 |
| F03 | 同步等待改堆上 `shared_ptr` state，回调只捕获堆/值；超时返回 `TimedOutButMayCommit`，迟到回调不触碰已销毁栈 |
| F08 | `NativeDatabase` 五态状态机 + `shared_ptr` 成员（queue/pool）+ `m_mutex` 保护指针拷贝 |
| F09 | `ReadPool::close()` 等 active lease 归零再关连接；`m_key` 用后 `OPENSSL_cleanse` |
| R01 | 两阶段关闭：锁内移交所有权置 Closing → 锁外 join/等 lease → 锁内置 Closed；锁内不 join、不回调 |

### 阶段 C：迁移协议与一致性（F04/F05/F06/F07 + R02/R03/R10/R11）

| 项 | 修复 |
|---|---|
| F04 | `MigrationExporter` 补 12 缩略图字段 + `nextConversationBatch` 会话流；`decodeBatch` 补解码；`importConversations` 保留 unread/lastMsg/lastTs |
| F05 | 三态接入正式链路：`beginMigration/resetMigration` JNI/Kotlin 入口；`submit/finish` 事务内校验 `ShadowImport`；`verified` 后拒绝重复迁移 |
| F06 | `sameMessage()` 全字段比对；Schema v2 新增 `message_fts_identity`，重复 `msg_id` 同时 O(1) 校验 FTS 正文/拼音/首字母 |
| F07 | `finish` 走 Writer 队列，双流 checkpoint + 摘要通过后原子写 completed/state |
| R02 | 不再把会话 `maxSeq` 伪装为增量高水位；真正 `change_seq` 留在 P13 |
| R03 | 消息流 `checkpoint` 与会话流 `conversations_checkpoint` 分离 |
| R10 | 全字段 digest 通过 `sameMessage()` 逐列比对实现 |
| R11 | wire format 加 `magic(0x4A544D47) + version(1) + count` 头；decoder 严格校验版本 |

### 阶段 D：安全收口（F10–F14 + R04/R05/R12/R13）

| 项 | 修复 |
|---|---|
| F10 | `clearShadowDatabase` 清理前取进程锁、只删 DB/WAL/SHM、`.lock` 永久保留、errno 仅 ENOENT 视为成功 |
| F11 | decoder 无符号解码（`u32/u64` + `memcpy` 转有符号）、count 上限、单字段 1MiB 上限、EOF 校验 |
| F12 | `DbKeyManager` 进程内 owner 锁 + 永久 lock 文件/FileChannel 跨进程锁 + 唯一临时文件 + `fd.sync()` + 原子 rename；清库最后才删 key |
| F13 | `JniDbKeyBridge::loadKey` 复制后 `SetByteArrayRegion` 覆盖 Java 字节；删 `Impl::key` 冗余副本 |
| F14 | `filesDir` 只允许 SDK 传入 Android Context 内部目录；Native 再拒绝相对路径/NUL/`..` 越界段（合法空格不误杀） |

---

## 3. 过程中发现并修复的隐藏 bug

1. **`MigrationImporter::isCompleted` 无「无记录」分支**：空库时 `sqlite3_step` 返回 `SQLITE_DONE`，函数返回 false，导致 `queryMigrationState` 失败。此前 JNI 忽略返回值未暴露，改为检查返回值后暴露。已补 `else { ok = true; }`。
2. **`test_room_import` 冲突测试失败事务未回滚**：`importBatch` 失败后未显式 `ROLLBACK`，导致后续 `BEGIN IMMEDIATE` 失败。已补显式回滚。

---

## 4. 验证结果

| 项 | 结果 |
|---|---|
| 桌面 CTest | **22/22**（含新增 `native_database_lifecycle`） |
| 桌面 ASan+UBSan | **22/22**（`outputs/kernel-round6-asan.log`） |
| 桌面 TSAN | db_command_queue / read_pool / native_database_lifecycle 无 data race（`outputs/kernel-round6-tsan.log`） |
| Android 两 ABI 构建 | 通过（arm64-v8a / x86_64） |
| `ShadowMigrationTest`（AVD） | 4/4（含新增 conversations 保留） |
| `MediaFileIntegrityTest`（AVD） | 1/1 |
| `NativeDbLongRunTest`（AVD） | 3/3 |
| `RoomNativeFixtureTest` / `DbKeyManagerFailCloseTest` | 全绿 |

## 5. 关键不变量验证

- **同一 `sqlite3*` 最大并发持有数恒为 1**（`test_read_pool` [4] 新增断言，8 线程争抢 2 连接）。
- **close 等待 active lease 归还**（`test_native_database_lifecycle` [2]，慢查询 + 并发 close 无 UAF）。
- **超时迟到回调无 UAF**（`test_native_database_lifecycle` [3]，ASan 下通过）。
- **重复 msg_id 不同内容冲突回滚**（`test_room_import` [11]）。
- **verified 后重复迁移被拒**（`ShadowMigrationTest` migrateMessages）。

## 6. 遗留（不阻塞 P6 完成）

1. **`NativeLifecycleTest` / `NativeSocketHeartbeatTest` 的 MMKV 未初始化失败**：`IllegalStateException: You should Call MMKV.initialize() first`，为既有测试基础设施问题（依赖测试顺序初始化 MMKV），与本次 F01–F14 修复无关，文件本次未改动。
2. **P13 遗留**（见 fix-plan v3 §7）：在线增量追平、最终短暂停写、原子 cutover、`delta_checkpoint/cutover_ready/epoch` 多流切换协议。

## 7. 2026-09-10 基于代码事实的补充验证

- 修复 `NativeDatabase` 完整生命周期串行化并真实进入 Opening；新增并发 open/close 回归。
- Writer 先完成事务与 pending 状态，再由独立 completion executor 投递 sink/onDone；新增回调重入 close 和
  WriteFn 抛异常回归，消除 self-join。
- 迁移状态与自检在同一只读事务快照完成；ReadPool close 同时等待 active lease 和 waiter。
- Android 导出摘要改为直接统计 conversations，保复空会话不一致；finish 要求消息/会话双 checkpoint。
- Schema v2 增加 FTS identity 表及 v1→v2 回填测试，重复消息不再漏检拼音索引差异。
- JNI byte[] 增加 null 检查、RAII 释放和 C++ 异常边界。

验证结果：桌面相关回归 **6/6**；桌面全量 CTest **22/22**（两个端口测试在允许 bind 的环境补跑）；
ASan+UBSan 的 client_core 22 组和 im_server 13 组均完成，其中受限环境失败的 4 个端口用例在相同
Sanitizer 构建下补跑 **4/4**；Android Kotlin + Native arm64-v8a/x86_64 构建成功。当前没有连接 AVD，
因此本次修复后的 Android instrumentation/CheckJNI 真机复跑仍应作为合入前设备门禁，不伪写为已执行。

## 8. 下一步

进入 P7「消息 Outbox/Inbox」。
