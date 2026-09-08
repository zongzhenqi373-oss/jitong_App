# P0-T03 消息收发 / 漫游 / 幂等 / 排序基线

> 事实来源：`jitong_android` 当前 Kotlin 实现 + `protocol/im.proto` + `im_server` 处理逻辑。
> 只记录字段名、类型、协议号、常量与状态迁移，不记录正文内容。

## 1. 帧格式

```text
[4B 大端包长 = 4 + payload.size][4B 小端协议号][pb payload]
```

| 项 | 值 |
|---|---|
| 包长语义 | **包含** 4B 协议号，不含自身 4B |
| 包长端序 | 大端（`DataInputStream.readInt()`） |
| 协议号端序 | **小端** |
| 合法范围 | `4 <= len <= MAX_PACK_LEN(10*1024*1024)`；否则 `IOException("非法包长")` → 关闭连接 |
| EOF | `EOFException` → `readFrom` 返回 `null`（对端正常关闭） |
| 读取顺序 | 先读 4B 长度 → `readFully(body)` → 从 body 前 4B 小端拼 type → payload = body[4..len) |

建立安全通道后，所有业务帧被封装为 `APP_ENCRYPTED_FRAME(1040)`，明文为 `le32(innerType) || payload`（见 `auth-baseline.md` §9.3）。

心跳：`HEARTBEAT_RQ(1010)` / `HEARTBEAT_RS(1011)`，间隔 30s，响应忽略。

## 2. 发送链路

```text
MainViewModel.send(text)
  ├─ msgId = UUID.randomUUID().toString()          # 客户端生成，全局幂等键
  ├─ ChatMessage(msgId, peerId, fromMe=true, text, status=SENDING)
  ├─ append(m, incrUnread=false)                   # 内存上屏
  ├─ launch { store?.save(...) }                   # 异步落库（不等待）
  └─ launch { client.sendChat(peerId, text, msgId) }
       └─ ImClient.sendChat: ChatInfoRq{ myid, friid, msg, type=TEXT, msgId }
                             不填 ts / seq（服务端权威）
                             send(CHAT_INFO_RQ = 1005)
```

- 发送统一 `withContext(Dispatchers.IO)` + `writeLock`（Mutex）串行写。
- 图片/文件：先 HTTPS `POST /api/v1/upload` 取 `file_id`，再复用同一 `CHAT_INFO_RQ`（type=IMAGE/FILE）。

### 2.1 状态枚举

```text
ChatMessage.Status: SENDING, DELIVERED, OFFLINE_STORED, RECEIVED, FAILED
DB int 映射:       SENDING=0, DELIVERED=1, RECEIVED=2, OFFLINE_STORED=3, FAILED=4
```

### 2.2 ACK 回写

`Event.ChatSendResult` → `updateStatus(peerId, msgId, status, seq)`

| result | 常量 | 映射状态 | 附加 |
|---|---|---|---|
| 0 | `CHAT_RESULT_SUCC` | `DELIVERED` | `seq > 0` 时回写 seq 并重排 |
| 1 | `CHAT_RESULT_FAIL` | `OFFLINE_STORED` | 对方离线已转存 |
| 2 | `CHAT_RESULT_NOT_FRIEND` | `FAILED` | + Toast |
| 3 | `CHAT_RESULT_SERVER_ERROR` | `OFFLINE_STORED` | 假成功 |

SQL：`UPDATE messages SET status=? WHERE ownerId=? AND msgId=?`；`UPDATE messages SET seq=? WHERE ownerId=? AND msgId=?`

### 2.3 现状缺口（**必须有意识地补齐或显式接受**）

| 编号 | 缺口 | V3 要求 |
|---|---|---|
| M-01 | **无消息级超时重发**（无 `withTimeout`、无 ACK 超时、无自动重发） | `Outbox` 在认证可用后重发，超时重试用同一 `msg_id` |
| M-02 | **无 kill 进程后的 Outbox 恢复**（`SENDING` 行不会被重发） | P7-T02 要求可恢复 |
| M-03 | 落库与发送不在同一事务，落库失败不阻塞发送 | `Outbox` 先持久化 `SENDING` 再发送 |

## 3. 接收与幂等

```text
readLoop → Frame.readFrom → secureChannel.decrypt → dispatch(frame)
  → CHAT_INFO_RQ(1005) 分支
      ts = if (rq.ts > 0) rq.ts * 1000L else System.currentTimeMillis()   # 服务端秒 → 毫秒
      TEXT  → Event.ChatReceived(fromId, msg, msgId, ts, seq)
      IMAGE/FILE → Event.MediaCard(...)
```

- **幂等键 = `msgId`**（客户端 UUID）
  - 内存：`conv.indexOfFirst { it.msgId == msg.msgId }`
  - DB：`UNIQUE(ownerId, msgId)` + `@Insert(onConflict = IGNORE)`，`rowId == -1` 视为重复
- 落库涉及 `messages` + `messages_fts` + `conversations`
- **事务边界**：`MessageDao.insertWithFts`（`@Transaction`）与 `ConversationDao.upsertOnMessage`（`@Transaction`）**是两个独立事务**
- 重复时的修正：`!inserted && kind==FILE && !fromMe` → `reconcileIncomingFile`
  ```sql
  UPDATE messages SET fileId=?, fileName=?, fileSize=(:fileSize>0 才覆盖),
         ts=(:ts>0), seq=(:seq>0), status=2
  WHERE ownerId=? AND msgId=? AND type=2 AND fromMe=0
  ```
- 登录装载：`ChatStore.hydrate` 先 `recoverInterruptedFileDownloads`
  ```sql
  UPDATE messages SET status=2 WHERE ownerId=? AND type=2 AND fromMe=0 AND status=0
  ```
  再 `allForOwner`；若 `ftsCount()==0` 则 `rebuildFts` 全量重建。

## 4. 漫游

### 4.1 触发时机

1. 登录成功（`LOGIN_SUCCESS` 或 `TOKEN_LOGIN_RS` 成功）→ `client.roamConversations()`
2. `openChat(friend)`：本地空 → `requestHistory(peerId, Long.MAX_VALUE)`；本地非空且游标为 null → 取 `local.filter { it.seq > 0 }.minOfOrNull { it.seq }` 作游标（全为 seq=0 则用 `Long.MAX_VALUE`）
3. 上拉 `loadMoreHistory(peerId)`

### 4.2 协议

| 方向 | 协议号 | 消息 |
|---|---|---|
| 请求会话 | `ROAM_CONV_RQ = 1013` | — |
| 响应会话 | `ROAM_CONV_RS = 1014` | — |
| 请求消息 | `ROAM_MSG_RQ = 1015` | `RoamMsgRq{ myid(1), peer_id(2), before_seq(3), limit(4) }` |
| 响应消息 | `ROAM_MSG_RS = 1016` | 含 `has_more`、`min_seq`，消息 seq **倒序**返回 |

- 客户端分页常量：`PAGE_SIZE = 20`
- 服务端约束：`beforeSeq <= 0 → INT64_MAX`；`limit <= 0 → 20`，`> 100 → 100`；`has_more = (rows.size() == limit)`；`min_seq = rows.back().seq`（空批为 0）

### 4.3 响应处理

```text
Event.RoamMessages(peerId, msgs, hasMore, minSeq)
  ├─ 跳过非 TEXT/IMAGE/FILE
  ├─ peerId = if (fromId == myId) toId else fromId ; fromMe = (fromId == myId)
  ├─ status = if (fromMe) DELIVERED else RECEIVED
  ├─ append(..., incrUnread=false, updateConversation=false)   # 不刷新会话预览
  ├─ 游标：if (minSeq > 0 && (cur == null || minSeq < cur)) loadedMinSeq[peerId] = minSeq
  └─ roamHasMore[peerId] = hasMore ; roamLoading.remove(peerId)
```

- **UI 上拉只调 `loadMoreHistory(peerId)`，不传 `beforeSeq`**
- 防并发：`roamLoading: MutableSet<Int>`；`roamHasMore == false` 或游标为 null 直接 return
- `Event.RoamConversations` **只更新内存会话行**（`conversationId = 0L` 占位），**不落消息表**（避免占位行抢占 `msgId` 唯一约束）；覆盖条件 `old == null || item.ts >= old.lastTs`
- `resetToLogin()` 清空 `loadedMinSeq` / `roamHasMore` / `roamLoading`

## 5. 离线消息

- **无独立协议号**，与实时消息走同一 `CHAT_INFO_RQ(1005)` 分支
- 服务端在线：转发 + 回执 `result = CHAT_RESULT_SUCC(0)`
- 服务端离线：`saveMessage(delivered = false)` + 回执 `result = CHAT_RESULT_FAIL(1)`，seq 由服务端分配并回传
- 登录补发：`pullUndelivered(userId)` → 逐条 `deliver(CHAT_INFO_RQ)` → `markDelivered()`；若原发送方在线，额外给发送方补 `ChatInfoRs{ result=SUCC, msg_id, seq }`
- 客户端因此可能在补发时收到「自己发的、本地已存在同 msgId」的消息 → 走 §3 的重复修正路径

## 6. 数据库表结构

### 6.1 `messages`

主键 `id: Long`（autoGenerate，仅物理行 id）

```text
ownerId:Int, msgId:String, conversationId:Long, peerId:Int, fromMe:Boolean,
type:Int(0=TEXT / 1=IMAGE / 2=FILE),
content:String?, mediaPath:String?, imgW:Int, imgH:Int, ts:Long, seq:Long(=0),
fileId:String?, fileName:String?, fileSize:Long, contentType:String?, sha256:String?,
thumbnailFileId:String?, thumbnailPath:String?, thumbnailSize:Long, thumbnailSha256:String?,
thumbnailW:Int, thumbnailH:Int,
largeThumbnailFileId:String?, largeThumbnailPath:String?, largeThumbnailSize:Long,
largeThumbnailSha256:String?, largeThumbnailW:Int, largeThumbnailH:Int,
localPath:String?, transferred:Int, status:Int
```

索引：

```text
UNIQUE(ownerId, msgId)                      # 幂等
INDEX(ownerId, conversationId, seq)         # 会话内排序/分页
INDEX(ownerId, conversationId, status)      # 失败重发/未读
```

> 客户端 `(conv, seq)` 索引**非唯一**；服务端 `messages` 表有 `UNIQUE(conversation_id, seq)`。
> V3 §9.3 要求的 `(owner_id, peer_id, conversation_seq)` / `(owner_id, peer_id, server_time)` / `(owner_id, status, local_order)` 为**新增索引**。

### 6.2 `messages_fts`

```sql
CREATE VIRTUAL TABLE messages_fts USING FTS4(content, pinyin, initials, msgId);
```

- 独立存储（**非** external content），无触发器
- **tokenizer 未显式声明 → FTS4 默认 `simple`**
- 只有 `type == 0`（TEXT）且 content 非空才建索引

### 6.3 `conversations`

主键 `conversationId: Long`；`ownerId:Int, peerId:Int, lastMsg:String, lastTs:Long, unread:Int`
**未定义**显式索引/UNIQUE。

### 6.4 库

- 文件名 `jitong_<ownerId>.db`，`version = 8`，`exportSchema = false`
- SQLCipher：`SupportOpenHelperFactory(key.copyOf())`，`System.loadLibrary("sqlcipher")`
- `fallbackToDestructiveMigration()`；迁移 `MIGRATION_2_3 … 7_8`

### 6.5 conversationId 算法

```text
conversationId = (min(a,b) << 32) | max(a,b)
```

（`MIGRATION_4_5` 把旧的 `min * 2^20 + max` 统一为此式）

## 7. seq 与排序

| 字段 | 语义 |
|---|---|
| `seq` | **会话级**，服务端分配，本地默认 0（未确认） |
| `ts` | 毫秒；服务端秒 × 1000，服务端为 0 时用本地 `currentTimeMillis()` |
| `id` | 本地自增 rowid |

> 未发现 `conversation_seq` / `server_time` / `local_order` 命名；V3 引入这些概念时需在 P6 做字段映射。

排序规则：

```text
内存 sortBySeq: compareBy({ if (seq > 0) seq else Long.MAX_VALUE }, { ts })
DB allForOwner: ORDER BY CASE WHEN seq > 0 THEN seq ELSE 9223372036854775807 END, ts, id
搜索结果:       ORDER BY ts DESC LIMIT 100
```

合并：`mergeMessages(fromDb, inMemory)` 以 DB 为基底、按 `msgId` 去重、`sortBySeq` 合并（防止 hydrate 冲掉异步到达的离线消息）；`mergeConversations` 以 `lastTs >= db.lastTs` 决定是否覆盖。

**空洞检测：未实现**（注释称 seq 用于空洞检测，但代码中未找到）。漫游仅以 `minSeq` 为游标，`rows.size() == limit` 即认为 `has_more`。

## 8. Golden 场景矩阵

| ID | 场景 | 输入 | 状态变化 | 协议序列 | 期望结果 |
|---|---|---|---|---|---|
| M-01 | 发送成功（对方在线） | text | `SENDING → DELIVERED` | `CHAT_INFO_RQ(1005)` → `CHAT_INFO_RS(1006, result=0)` | 单条消息；`seq > 0` 回写并前移；无重复 |
| M-02 | 发送（对方离线） | text | `SENDING → OFFLINE_STORED` | `CHAT_INFO_RQ` → `result=1` | 状态 3；seq 已由服务端分配并回传 |
| M-03 | 发送给非好友 | text | `SENDING → FAILED` | `CHAT_INFO_RQ` → `result=2` | 状态 4 + Toast |
| M-04 | 服务端假成功 | text | `SENDING → OFFLINE_STORED` | `CHAT_INFO_RQ` → `result=3` | 状态 3 |
| M-05 | 双方同时发送 | A、B 几乎同时各发 1 条 | 各自 `SENDING → DELIVERED` | 2 × `CHAT_INFO_RQ` | 两端最终 seq 顺序一致；各端 2 条消息，无乱序 |
| M-06 | 重复包 | 服务端重发同一 `msgId` | 无变化 | 2 × `CHAT_INFO_RQ(同 msgId)` | DB 只 1 行（`onConflict=IGNORE`）；UI 不重复通知 |
| M-07 | 离线消息补发 | B 离线期间 A 发 3 条，B 上线 | B 端 `RECEIVED` | 登录后 3 × `CHAT_INFO_RQ` | B 端 3 条，顺序与 A 端一致；A 端收到 `ChatInfoRs(result=0)` 补发 |
| M-08 | 断线重发 | 发送后立刻断线 | `SENDING`（当前**不会**自动重发，见缺口 M-01） | `CHAT_INFO_RQ` 已发出，无回执 | 现状：停留在 `SENDING`；Native 目标：重连后以同一 `msg_id` 重发 |
| M-09 | 历史分页（首屏） | 进入会话，本地为空 | — | `ROAM_MSG_RQ(1015, before_seq=Long.MAX_VALUE, limit=20)` | 返回 20 条（服务端上限 100），`has_more = (size==limit)`；游标更新为 `min_seq` |
| M-10 | 历史分页（上拉） | 上拉到顶部 | — | `ROAM_MSG_RQ(before_seq=loadedMinSeq)` | 更早 20 条；游标下移；`roamHasMore=false` 后不再请求 |
| M-11 | 分页并发 | 快速连续上拉 3 次 | — | 仅第 1 次发出请求 | `roamLoading` 防并发；后续被短路 |
| M-12 | seq 缺口 | 本地 max_seq=100，服务端已有 101~105 | — | SyncEngine 应发现缺口并补洞 | 现状：Android **无补洞**；Native 目标：分页补齐 101~105 |
| M-13 | 服务端重复返回同一页 | 同一 `before_seq` 返回两次 | — | 2 × 相同 `ROAM_MSG_RS` | 因 `msgId` 幂等，DB 不重复；游标不回退（`minSeq < cur` 才更新） |
| M-14 | 漫游时不刷新会话预览 | 漫游 20 条历史 | 会话行 `lastMsg/lastTs` 不变 | `ROAM_MSG_RS` | `updateConversation=false`；会话预览只由 `ROAM_CONV_RS` 更新 |
| M-15 | kill 进程恢复 | 有 `SENDING` 消息时杀进程 | 重启后 hydrate | — | 现状：`SENDING` 保持；Native 目标：Outbox 恢复并重发 |
| M-16 | 会话内排序 | 混合 seq=0（未确认）与 seq>0 | — | — | seq>0 按 seq 升序；seq=0 排末尾并按 ts 兜底 |
| M-17 | 文件消息重复修正 | 离线补发与本地 SENDING 同 msgId | `SENDING → RECEIVED` | `CHAT_INFO_RQ(type=FILE)` | `reconcileIncomingFile` 更新 fileId/size/ts/seq/status=2 |
| M-18 | 超大包 | 收到 `len > 10MB` | 连接关闭 | — | `IOException("非法包长")` → 断线；不 OOM |

## 9. Native 内核需补齐的差异（P7/P8 阶段对照）

| 编号 | Android 现状 | V3 要求 | 处理 |
|---|---|---|---|
| N-01 | 无消息级超时重发与 Outbox 恢复 | `Outbox` 持久化 + 重发 | **新增能力** |
| N-02 | 无 seq 缺口补洞 | `SyncEngine` 对比水位并补洞 | **新增能力** |
| N-03 | 落库与会话更新分属两个事务 | 单事务写消息、FTS、会话摘要 | **改造** |
| N-04 | 无 `local_order` | 未确认消息用 `local_order` 排序 | **新增字段** |
| N-05 | `conversations` 无索引 | — | P6 补齐 |
| N-06 | 网络线程/主线程直接写 Room | 单写线程，网络线程只投递命令 | **改造** |
