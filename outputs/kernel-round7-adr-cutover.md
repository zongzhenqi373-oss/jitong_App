# ADR-03：Cutover 为单向 epoch ／ ADR-04：权威数据与派生数据

| 项 | 值 |
|---|---|
| 状态 | **ACCEPTED**（不可变决策） |
| 日期 | 2026-09-10 |
| 负责人 | [待指定] |
| 关联 | `P7-G0`、§2 ADR-03 / ADR-04、`kernel-round7-implementation-plan.md` |

---

## 1. 背景与问题陈述

### 1.1 现状（代码事实）

| 事实 | 证据 |
|---|---|
| Room 当前 `version = 8` | `data/db/AppDatabase.kt:9-13` |
| Room 权威实体仅 `MessageEntity`、`MessageFtsEntity`、`ConversationEntity` | 同上 `entities = [...]` |
| **好友/好友申请不是 Room 实体** | `entities` 中无 Friend；好友为运行时/服务端状态 |
| Native Schema 当前 `version = 4` | 保留 001/002/003，新增 `004_local_sequence`（持久发号、Outbox payload 版本、会话 last_msg_id） |
| 现有迁移仅 `migration_meta(key/value)` 存 completed/state/checkpoint | 上一轮 P6 实现 |
| 已去掉 `fallbackToDestructiveMigration()` | `AppDatabase.kt:110-114`，失败报错并保留密文 |

### 1.2 问题

Room、Native SQLCipher、MMKV/Keystore **三者之间无天然跨库原子事务**。因此"写个 `cutover_ready = true` 布尔标记"无法保证原子性：标记落盘成功但数据未追平（或反之）都会产生两个事实源。

同时，把 FTS、会话 preview、可重算 unread 当作独立 delta 流回放，会引入"派生数据与权威数据不一致"的新问题，且无法从服务端重建。

---

## 2. ADR-03：单向 epoch（决策）

**不实现 Native → Room 反向导出。** 状态机固定为：

```
LEGACY_ACTIVE
    │ snapshot + 权威 delta
    ▼
PREPARED
    │ 停写、追平、对账通过
    ▼
NATIVE_COMMITTED_NO_WRITE
    │ 首次 Native 业务事务 commit（由 Database transaction hook 自动推进）
    ▼
NATIVE_COMMITTED_DIRTY
```

| 状态 | 语义 | 失败/回退行为 |
|---|---|---|
| `LEGACY_ACTIVE` | 迁移未开始或进行中 | 可继续 Legacy |
| `PREPARED` | 已停写追平、对账通过，等待提交 | 可继续 Legacy，可重做对账 |
| `NATIVE_COMMITTED_NO_WRITE` | 已承诺 Native，但**未**建立连接、**未**产生业务写 | 仅当 **DB 与 KV 镜像均一致**且 journal 校验完整时才允许**人工**回滚；KV 不一致（尤其 KV=`DIRTY`）时**禁止**回滚 |
| `NATIVE_COMMITTED_DIRTY` | Native 已产生业务写 | **永远禁止回到 Room**；只能修复 Native、从服务端重建，或执行未来单独设计的反向迁移工具 |

### 2.1 推进到 DIRTY 的方式（不可依赖上层"记得标记"）

首次 Native 业务事务 commit 前，由 **Database transaction hook** 把 journal 推进到 `NATIVE_COMMITTED_DIRTY`。

### 2.2 后端选择

`KernelBackendSelector` **只读 cutover journal** 在进程启动时选后端，进程内不可热切；不再读取另一个 `cutover_ready` 布尔 marker。

### 2.3 双写与一致性

- journal 的状态与 payload 存在 **Native DB**；
- 同时在**安全 KV 保存带 MAC 的最小镜像**；
- 两者不一致时 **fail-close**，按恢复矩阵处理，**不猜测最新值**。

### 2.4 发布与观察

- 先账号级灰度；
- Legacy Room **只读封存**一个观察窗口，不自动删除；
- 进入 DIRTY 后发生故障，UI 展示可恢复错误并优先做 Native 修复或服务端重建，**禁止静默启动 Legacy**。

---

## 3. 恢复矩阵（可机读，进入测试数据）

维度：`Native DB journal 状态 × KV 镜像状态 × 本地库存在性`

| # | DB journal | KV 镜像 | Room | Native | 期望 |
|---|---|---|---|---|---|
| 1 | 无 | 无 | 有 | 无 | 继续 Legacy |
| 2 | `LEGACY_ACTIVE` | 一致 | 有 | 有/无 | 继续 Legacy |
| 3 | `PREPARED` | 一致 | 有 | 有 | 继续 Legacy，可重做对账 |
| 4 | `PREPARED` | 缺失 | 有 | 有 | **fail-close**：等待修复，不自动选择 Legacy |
| 5 | `PREPARED` | 不一致 | 有 | 有 | **fail-close**：等待修复，尤其另一侧已提交时不得回 Legacy |
| 6 | `NO_WRITE` | 一致 | 有 | 有 | 进入 Native |
| 7 | `NO_WRITE` | 不一致 | 有 | 有 | **fail-close**：既不自动回 Legacy 也不自动进 Native，需人工/服务端判定 |
| 7b | `NO_WRITE` | KV=`DIRTY` | 有 | 有 | **fail-close**：KV 已脏而 DB 未推进（矛盾态），**绝不自动回 Legacy**；按 DIRTY 方向处理（Native 只读 + 服务端重建校验） |
| 8 | `DIRTY` | 一致 | 有 | 有 | 进入 Native，**禁止**回 Room |
| 9 | `DIRTY` | 不一致 | 有 | 有 | **fail-close**：进入 Native 只读或等待修复，**绝不**回 Room |
| 10 | `DIRTY` | 一致 | 有 | 损坏 | 修复 Native / 服务端重建，**不得**静默启动 Legacy |

**强杀点覆盖**：journal 每一状态、KV/DB 写入与 fsync 前后、数据库 commit 前后，均需验证冷启动恢复结果**唯一**。

2026-09-11 补充：`runtime/RecoveryResolver.h` 已实现纯决策并提供组合回归；尚不代表 Android
启动选择器已经接入。两侧 epoch/owner 不一致、损坏、单侧缺失都进入 Repair；两侧均缺失但发现
Native 库同样进入 Repair。只有一致的 NO_WRITE/DIRTY 且库/内核可用才能选 Native。
一致 NO_WRITE 的回滚仍须显式人工操作，不由 Resolver 自动执行。
服务端重建无法保证恢复本地未发 Outbox、独有文件和全部已读状态，必须保留原 Native 库与文件。

---

## 4. ADR-04：权威数据与派生数据（决策）

### 4.1 Room change log 只记录权威变化

仅以下四类进入 change log：

- `MESSAGE_UPSERT` / `MESSAGE_DELETE`
- `CONVERSATION_UPSERT`
- `READ_WATERMARK_UPDATE`
- `MEDIA_STATE_UPDATE`

### 4.2 派生数据不作为独立 delta 流

**FTS、会话 preview、可重算 unread 不作为独立 delta 流。** Native 在导入 message/conversation 的**同一事务**中维护派生表；导入完成后执行：

- FTS identity 校验
- 计数校验
- 水位校验
- 抽样 hash 校验

### 4.3 好友与申请不从 Room 编造

好友与申请**当前不是 Room 实体**，Native 登录后**从服务端重新同步**，不从 Room 编造 snapshot。

### 4.4 迁移来源表

| Native 数据 | 来源 / 策略 |
|---|---|
| messages | Room snapshot + MESSAGE delta |
| conversations / read watermark | Room snapshot + conversation/read delta |
| FTS | Native 根据导入消息**重建**并校验 |
| friends / friend_requests | Native 登录后由**服务端**重新同步 |
| media_variants / media_refs | 从 `MessageEntity` 媒体字段投影并检查本地文件 |
| transfer_tasks | 不复制活协程；cutover 前等待、取消或将可验证 `.part` 转为可恢复任务 |
| sync_gaps | 按已导入 seq 与服务端漫游结果**重算** |

### 4.5 全局 high-water

使用**全局 changeSeq**（不是 conversation seq）作为 high-water，否则 delete / 跨会话更新会漏。change log 使用 tombstone，Native 确认 checkpoint 前**不得 GC**。

---

## 5. 备选方案与取舍

| 选项 | 取舍 |
|---|---|
| **A. 单向 epoch（本决策）** | 范围可控；失败路径明确。**选中** |
| B. 可回退（实现 Native→Room 反向导出） | 显著扩大本轮范围，且反向导出本身需完整验证；本轮不做 |
| C. 布尔 `cutover_ready` marker | 无原子性保证，会产生双事实源。**否决** |
| D. FTS/unread 作为独立 delta 流 | 派生数据无法从服务端重建权，易产生不一致。**否决**（改由权威数据派生 + 校验） |

---

## 6. 验收

- [ ] `MIGRATION_8_9` 与 `LegacyWriteGateway` 落地，**静态枚举所有 Room writer** 并有测试 hook 检测绕过 gateway 的写；
- [ ] 从仍支持的每个 Room schema 版本升级到 9；
- [ ] snapshot 期间持续收发 / markRead / 图片下载，delta 重复、倒序、删除、缺口与强杀可恢复；
- [ ] 最终停写窗口 **P95/P99 ≤ 200ms**，达不到不切换；
- [ ] 恢复矩阵 10 条全部有自动化用例；
- [ ] 进入 `DIRTY` 后制造新消息再损坏启动条件，确认**不会**回旧 Room；
- [ ] journal 推进由 transaction hook 完成，移除任何"上层记得标记"的路径。

---

## 7. 不可变决策

1. **不做** Native→Room 反向导出；`DIRTY` 后永不回 Room；
2. 后端选择只读 cutover journal，不读布尔 marker；
3. DB journal 与安全 KV 镜像不一致时 **fail-close**，不猜测；
4. change log 只记权威变化；FTS/preview/可重算 unread 为派生，由权威数据重建并校验；
5. high-water 使用**全局 changeSeq**；tombstone 在 Native 确认 checkpoint 前不得 GC。
