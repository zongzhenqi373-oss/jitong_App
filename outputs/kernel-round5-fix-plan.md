# 第五轮 P6 代码审查修复方案（修订版 v3）

> 依据：
> - `outputs/kernel-round5-code-review.md`（F01–F14 问题清单）
> - `outputs/kernel-round5-fix-plan-review.md`（对修复方案的审查意见 R01–R13）
> - `outputs/jitong-qqnt-kernel-execution-plan.md` 第 24 章（P6 目标与边界）
> 日期：2026-09-09
> 状态：**已实施完成**。F01–F14 与 R01–R13 全部落地，验证见 `outputs/kernel-round6-acceptance.md`。

---

## 1. 判定与结论

对照 F01–F14 与 R01–R13，逐条核实当前代码与规划文档后，结论如下：

1. **F01–F14 全部真实存在**，行号与代码一致。
2. **R01、R04、R05、R06–R13 成立**，采纳为方案级决议或实施细节。
3. **R02、R03 部分越界**：其「在线追平 / delta / cutover_ready」内核属于 P13 统一切换轮次，不是 P6 阻断项；但「conversations 独立 checkpoint」与「预留 snapshot 边界避免返工」的合理内核保留在 P6。

最终判定：**APPROVE WITH REQUIRED REVISIONS，修订后可执行**。实施顺序必须先「冻结 API 与状态模型」再动实现，且在修订完成前不改 JNI 签名与迁移二进制格式（避免二次返工）。

---

## 2. 问题清单

### F01–F14（来自 code-review，全部核实属实）

| 编号 | 优先级 | 问题 | 核实位置 |
|---|---|---|---|
| F01 | P0 | ReadPool 池满时并发复用同一 `sqlite3*` | `ReadPool.cpp:69-82` |
| F02 | P0 | JNI 绕过单 Writer 队列，直接并发使用 Writer connection | `NativeDatabase.h:76-77`、`im_db_jni.cpp:419-498` |
| F03 | P0 | 迁移 JNI 等待超时后迟到回调栈 UAF | `im_db_jni.cpp:358-397` |
| F04 | P0 | Room → Native 迁移丢缩略图与会话状态 | `MigrationExporter.kt:41-49,91-175`、`Entities.kt:39-50` |
| F05 | P1 | 迁移三态未接入正式 JNI 流程 | `MigrationImporter.cpp:343-363`、`im_db_jni.cpp:330-426` |
| F06 | P1 | 重复 `msg_id` 未覆盖消息却仍篡改会话摘要 | `MigrationImporter.cpp:174-205` |
| F07 | P1 | finish 的 completed/state 非原子提交 | `MigrationImporter.cpp:274-290,357-363` |
| F08 | P1 | NativeDatabase 生命周期无同步 | `NativeDatabase.cpp:118-147` |
| F09 | P1 | ReadPool close 关使用中连接 + key 未清零 | `ReadPool.cpp:47-54`、`NativeDatabase.cpp:31` |
| F10 | P1 | 清理影子库忽略删除失败并破坏进程锁互斥 | `DatabasePaths.cpp:68-81` |
| F11 | P1 | 批次 decoder 超大分配 + 有符号移位 UB | `im_db_jni.cpp:104-160` |
| F12 | P2 | key blob 非原子 + 无并发互斥 + 缺失检测只查 Room | `DbKeyManager.kt:64-80,104-118` |
| F13 | P2 | 密钥临时副本清零不完整 | `jni_db_key_bridge.cpp:71-88`、`ReadPool.cpp:10-16` |
| F14 | P2 | 路径约束弱于接口声明 | `DatabasePaths.cpp:31-65` |

### R01–R13 核实结论

| 修订 | 核实结论 | 采纳方式 |
|---|---|---|
| R01 两阶段关闭 | ✅ 成立（纠正 v1 方案反模式，与 §24.2 一致） | 方案级决议 |
| R02 源端一致性 | ⚠️ 部分越界（在线追平属 P13） | 降级为 P13 遗留 + P6 预留 snapshot 语义 |
| R03 多流 checkpoint | ⚠️ 部分越界 | conversations 独立 checkpoint 保留 P6；delta/cutover/epoch 记 P13 遗留 |
| R04 `.lock` 永久保留 | ✅ 成立（真实 flock unlink 竞态） | 方案级决议 |
| R05 key 检测含 Native 库 | ✅ 成立（真实场景） | 方案级决议 |
| R06 ReadPool 结果类型 | ✅ 成立 | 实施细节 |
| R07 withRead 异常约定 | ✅ 成立 | 实施细节 |
| R08 多查询同一快照 | ✅ 成立 | 实施细节 |
| R09 completion 状态模型 | ✅ 成立（对应 §24.2「JNI 不等待长事务」） | 实施细节 |
| R10 全字段 digest | ✅ 成立 | 实施细节 |
| R11 wire format 头 | ✅ 成立 | 实施细节 |
| R12 key blob 原子写细节 | ✅ 成立 | 实施细节 |
| R13 可信路径注入一次 | ✅ 成立（对应 §24.8 接口不再传 filesDir） | 实施细节 |

---

## 3. 方案级修订决议

### R01：NativeDatabase 两阶段关闭（锁内不 join、不执行回调）

`close()` 改为两阶段：

1. **锁内**：`Ready → Closing`，从成员 move 出 `queue/pool/conn/lock` 所有权，禁止新 operation/lease；
2. **锁外**：关闭 queue 并 join、等待 ReadPool lease 归还、关闭连接；
3. **锁内**：清理最终状态并置 `Closed`。

生命周期锁内**不得** join 外部线程、不得执行用户回调（§24.2 硬约束）。`open()` 不持 lifecycle lock 调用 `close()`。增加「callback 重入 close/submit」专门测试。

### R02（裁剪后）：源端一致性——P6 预留快照语义，在线追平记 P13 遗留

**核实**：P6 边界（§24.1）明确「不做双写、不启用 Native Backend、用测试夹具迁移、切换在 P13」。P6 影子迁移是**离线验收**，不是「Legacy 持续写入下在线切换」。

**决议**：

- **P6 不实现**「高水位 + delta 追平 + 最终短暂停写」；
- **P6 预留**：迁移元数据增加 `source_high_watermark`（记录导出时刻的快照边界）与 `format_version`，为 P13 增量追平预留语义，避免返工；
- **P13 遗留**（记为后续轮次，不是 P6 阻断）：在线增量追平、最终短暂停写、原子 cutover。

### R03（裁剪后）：多流 checkpoint——P6 只做 conversations 独立游标

**决议**：

- **P6 范围**：`messages_checkpoint` 与 `conversations_checkpoint` 分开，每批数据与对应 checkpoint 在同一 Native Writer 事务提交；
- **P13 遗留**：`delta_checkpoint / cutover_ready / epoch`。

### R04：`.lock` 永久保留

`.lock` 作为稳定互斥锚点**永久保留**，`clearShadowDatabase` 只删 DB/WAL/SHM，绝不 unlink `.lock`。测试验证清理后 lock 文件仍存在、第二进程始终无法绕过。

### R05：key 缺失检测同时覆盖 Room 与 Native 密文库

「已有密文数据」定义为 **Room DB/WAL/SHM 任一存在，或 Native DB/WAL/SHM 任一存在**。任一存在而 key blob 缺失都返回 `DatabaseExistsWithoutKey`，不得生成新 key。更稳妥：由统一 `DbKeyPlatform` 接收受信任的数据库存在性查询，避免 Kotlin 与 C++ 各自拼路径。

---

## 4. 实施细节约定（R06–R13）

### R06：ReadPool 结果类型
`withRead` 返回枚举 `Ok/Busy/Timeout/Closing/CallbackFailed`（非 bool）。等待必须有 deadline；close 唤醒所有 waiter 返回 `Closing`，只等待 active lease。RAII guard 析构不抛异常。

### R07：withRead 回调异常约定
核心边界 catch 回调异常、归还 lease、转换明确错误，不穿过 JNI/Writer thread。生产代码采用返回 `DbResult` 的 no-throw callable。

### R08：state/self-test 同一只读快照
多条 SELECT 必须在同一只读 lease 上显式 `BEGIN` → 读全部 → `COMMIT`，或作为 Writer queue barrier 后的单个控制命令执行。定义 read-after-write 与 snapshot 语义。

### R09：completion 状态模型（落实 §24.2「JNI 不等待长事务」）
优先设计为 Kotlin 在 IO dispatcher 调异步 Native API，Native completion 经一次性 callback/future 返回；事务开始后等最终完成；SDK 不在 JNI 栈上固定阻塞。若保留同步 `submitAndWait`，超时返回 `TimedOutButMayCommit` 并按 operationId 查询最终结果，避免上层重试与仍在运行的事务竞态。写代码前先定义状态模型。

### R10：重复 msg_id 全字段 digest
比较全部持久化字段（conversation/type/from_me/status + 全部媒体/缩略图元数据），或在源端生成 canonical row digest、Native 对既有行算同一 digest。明确允许单调更新的字段（如「发送中→已送达」），迁移幂等与业务状态合并不混同。

### R11：wire format 头部与总量预算
batch 头部：`magic | format_version | stream_type | row_count | payload_length`。每字符串设单字段上限、整批设累计字节预算（`row_count<=500` 之外避免 500 个 1 MiB 正文突破 8 MiB）。checked arithmetic，先验证再分配。提供 Golden 编解码测试。

### R12：key blob 原子写 Android 细节
结合 minSdk：同目录临时文件、`FileOutputStream.fd.sync()`、校验长度后原子替换；失败删临时文件保留旧 blob。owner lock 覆盖「检查 DB 存在 → 生成 → 写临时 → rename → 返回 key」全过程。测试各落盘阶段强杀恢复。

### R13：可信路径「初始化注入一次」
SDK 创建时从 Android 内部取可信 `filesDir`，Native 保存 canonical root；后续数据库 API 只收 ownerId，用 `mkdirat/openat` + `O_NOFOLLOW` 在可信目录下创建。普通业务 JNI 不再传任意 filesDir 字符串（对齐 §24.8 接口设计）。

---

## 5. 修复方案（按五阶段执行）

### 阶段 A：冻结 API 与状态模型

先写接口与状态转移测试，不改实现。

1. 定义 `NativeDatabase` 状态：`Closed / Opening / Ready / Closing / Locked`。
2. 定义 operation lease、`ReadLease`、completion/timeout 结果类型（含 R06/R09 枚举）。
3. 删公开 `writerHandle()`，定义 `beginMigration/submitBatch/finishMigration/resetMigration/queryState` 核心用例 API。
4. 定义迁移 wire format（R11 头部）与 messages/conversations 独立 checkpoint（R03 P6 部分）+ `source_high_watermark`（R02 P6 预留）。
5. 编写接口状态转移单测。

### 阶段 B：并发与生命周期（F01/F02/F03/F08/F09 + R01/R06/R07/R08/R09）

1. **F08/F01/F09 + R01**：两阶段关闭；ReadPool 真正 lease（cv + deadline + RAII）；`m_key` 清零、删 `Impl::key` 冗余。
2. **F02 + R08**：删 `writerHandle()`；核心用例 API 全经 `submit`/`withRead`；state/self-test 同一只读快照。
3. **F03 + R09**：completion 堆上 state；`TimedOutButMayCommit` 结果；异步 callback/future。
4. **R06/R07**：ReadPool 结果枚举；withRead no-throw。

**门禁**：同一 read connection 最大并发 lease == 1；callback 重入 close 不死锁；close 不持 lifecycle lock join；active read + close 无 UAF；四线程并发所有 waiter 确定结果；超时/取消/迟到完成状态可解释；ASan 无 UAF、TSAN 无报告。

### 阶段 C：迁移协议与一致性（F04/F05/F06/F07 + R02 预留/R03 P6 部分/R10/R11）

1. **F04 + R03 + R11**：wire format 加头部；补 12 缩略图字段；conversations 独立流 + 独立 checkpoint；非零 unread 与空会话；记录 `source_high_watermark`。
2. **F06 + R10**：重复 msg_id 全字段 digest，明确单调更新字段。
3. **F05 + F07**：三态接入正式链路；finish/reset 借 Writer queue 外层事务（Importer 内不再嵌套 BEGIN）。

> 明确不做（P13 遗留）：在线 delta 追平、最终短暂停写、cutover。

**门禁**：带缩略图、非零 unread、空会话 fixture 全字段对账通过；Verified 后 submit 被正式 SDK 拒绝；同 msg_id 不同内容整体回滚；completed/state 注入失败重启无矛盾状态。

### 阶段 D：安全与文件系统收口（F10–F14 + R04/R05/R12/R13）

1. **F10 + R04**：`.lock` 永久保留；清理前取锁、只删 DB/WAL/SHM；errno 仅 ENOENT 视为成功。
2. **F11 + R11**：decoder 无符号解码、count 上限、checked arithmetic、EOF 校验、fuzz corpus。
3. **F12 + R05 + R12**：key 缺失检测含 Native 库；owner lock 覆盖全流程；临时文件 + fsync + 原子替换。
4. **F13**：密钥副本清零补全。
5. **F14 + R13**：filesDir 初始化注入一次，`mkdirat/openat + O_NOFOLLOW`，JNI 只传 ownerId。

**门禁**：清理失败可观察、lock 文件永久存在；畸形 batch 稳定拒绝；Native DB 存在而 blob 缺失 fail-close；key blob 并发创建 100 次只产一把可复开 key，各落盘阶段强杀可恢复。

### 阶段 E：重新验收

重新生成报告与日志，不追加「已修复」。至少包括：

1. CTest normal、ASan+UBSan、TSAN 分别构建运行；
2. decoder fuzz/畸形 corpus；
3. Android arm64 instrumentation + x86_64 构建；
4. CheckJNI；
5. kill-recovery 覆盖 message/conversation 多 checkpoint；
6. 数据字段 hash、媒体 inode/hash、未读和空会话对账；
7. 第二进程与 clearShadow 并发锁测试；
8. key blob 并发首次创建和各落盘阶段强杀恢复。

---

## 6. 重新验收门禁总表

| 阶段 | 必须追加的门禁 |
|---|---|
| B（并发） | callback 重入 close 无死锁；close 不持 lifecycle lock join；read waiter 有界超时并区分 Closing |
| C（迁移） | 消息/会话各自 checkpoint；旧消息更新不遗漏（测试夹具静态快照内）；同一快照字段摘要一致 |
| D（安全） | Native DB 存在而 key blob 缺失时 fail-close；lock 文件永久保留；key blob 写入各阶段强杀可恢复 |

---

## 7. P13 遗留清单（本轮不实现，不阻塞 P6）

1. 在线增量追平（高水位 + delta + 最终短暂停写 + 原子 cutover）；
2. `delta_checkpoint / cutover_ready / migration_epoch` 多流切换协议；
3. Legacy 持续写入下的影子迁移追平测试。

> 这些在 P13 统一切换轮次实现；P6 通过 `source_high_watermark` + `format_version` 预留语义，避免届时返工。

---

## 8. 最终判定标准

修订后先落地 5 项方案级决议（R01、R02 裁剪、R03 裁剪、R04、R05）与 7 项细节约定（R06–R13）。完成后：

- 修复 F01–F04 可恢复「进入 P7 前阻塞项已清」；
- F05–F14 与本轮一并收口后整体判「P6 完成」；
- 同步更新 `kernel-round5-acceptance.md`、规划文档第 24 章门禁复选框，并新增回归证据日志（ASan/TSAN/fuzz）。
