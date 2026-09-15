# 第七轮 P7 实施方案 v2：全部现有业务迁入 C++ 厚内核

> 日期：2026-09-10
> 依据：`outputs/jitong-qqnt-kernel-execution-plan.md` 第 25 章及
> `outputs/kernel-round7-implementation-plan.md` 审查结论
> 状态：**完成 ADR 后可执行**
> 目标：在不制造双连接、双写和数据回退风险的前提下，将当前 Android 已交付业务迁入 C++；分片上传、
> 精确区间补洞、多设备已读作为独立增强能力，不冒充现有功能迁移成果。

---

## 1. 本轮完成定义

### 1.1 必须完成的现有能力

| 领域 | 必须迁移的现有能力 | Native owner |
|---|---|---|
| 账号 | 注册、密码登录、Token 登录/刷新、single-flight、防重入、心跳、重连、前后台、被踢、登出、换号 | `AccountSession` |
| 消息 | 文本本地回显、Outbox、发送、回执、失败重试、接收、幂等、会话摘要、未读/已读 | `MessageService` |
| 同步 | 离线消息、会话漫游、历史分页、seq 排序、缺洞检测、用现有漫游接口逐页追平 | `SyncService` |
| 好友 | 好友列表、申请列表、同意/拒绝、删除、资料更新、在线/离线 UI 事件 | `FriendService` |
| AI | 上下文快照、tone、请求、20 秒超时、single-flight、取消、迟到响应抑制 | `AiSuggestionService` |
| 查询 | 会话、历史、全局/会话搜索、中文/拼音/首字母、高亮、跳转定位 | `SearchService` |
| 图片 | SAF 导入、宽高/方向、AVIF、大小缩略图、超长图、秒传、上传下载、分级加载、缓存 | `MediaService` |
| 文件 | SAF 导入、SHA-256、秒传 proof、整文件流式上传、Range 断点下载、取消、重试、恢复 | `MediaService` |
| 数据 | SQLCipher Schema v4、单 Writer、ReadPool；Room snapshot+delta、单向 cutover 为目标 | `NativeDatabase/MigrationCoordinator` |

协议事实以 `Protocol.kt/Protocol.h` 为准，功能矩阵必须包含至少：1000/1001 注册、1029/1030 AI 回复、
1031 AI 取消、1032/1033 删除好友、1034/1035 好友申请列表，以及现有登录、聊天、漫游、心跳和 Token
协议。不得使用“其他功能”作为责任项。

### 1.2 本轮不宣称完成的增强能力

以下能力只有服务端、客户端和 E2E 同时交付后才可单独标记完成：

- `resumable_upload_v1`：分片上传；
- `roam_range_v1`：按指定缺洞区间拉取；
- `read_sync_v1`：多设备已读水位同步。

未交付时分别降级到整文件流传、现有漫游分页追平和本地已读。增强项关闭不影响第七轮“现有能力迁移”
结论，但验收报告不得写成已完成。

### 1.3 平台边界

Kotlin/Android 只保留 Compose 渲染、导航、输入、生命周期信号、SAF/权限、Keystore、安全 KV、平台图片
Codec、网络可用性、JNI DTO/Flow 映射和主线程调度。Token 决策、重试、协议、SQL、去重、未读、补洞、
搜索合并、媒体状态机、缓存选择及事实源选择全部归 C++。

---

## 2. 编码前 ADR：P7-G0

G0 没有完成前，只允许补 Harness、测试 fixture 和文档，不允许实施 cutover 或修改生产事实源。

### ADR-01：数据库无感解锁采用双包装

选择：同一个随机 `realKey` 同时保存两个独立 AEAD wrapper。

```text
realKey
 ├─ passwordWrapper = AES-GCM(PBKDF2(passHash, passwordSalt), realKey)
 └─ deviceWrapper   = AES-GCM(Android Keystore account key, realKey)
```

要求：

- wrapper 文件使用版本化二进制格式，至少包含 magic、version、ownerId、keyId、算法、salt、nonce、密文；
- 两个 wrapper 使用独立 nonce 和 AAD，AAD 绑定 ownerId、wrapper type、version 和 keyId；
- Keystore alias 包含 ownerId，不跨账号复用；不保存明文密码或 passHash；
- 首次密码登录解开或创建 realKey 后，才允许创建 deviceWrapper；不得因 Keystore 失败生成新 realKey；
- Token 冷启动优先 deviceWrapper；失败后数据库保持锁定，转入 `NEEDS_PASSWORD_UNLOCK`；
- 密码变化时，只有已经成功获得 realKey 后才能原子替换 passwordWrapper；失败保留旧 blob；
- 登出只清 Token 和内存 key，是否删除 deviceWrapper 由“保留本地聊天记录”产品选项决定；清理必须显式；
- Keystore wrapper 降低的是冷启动门槛，不改变 SQLCipher realKey 强度，也不能绕过 Android 锁屏安全边界。

验收：记住/未记住密码、Token 有效/过期、Keystore 失效、密码改变、wrapper 截断/GCM tag 错、磁盘写失败
全覆盖；任何失败都不创建空库、不覆盖旧库、不 destructive migration。

### ADR-02：服务端能力协商不升级安全协议版本

保持当前应用安全协议 `version=1`。若本轮实现增强项，则在既有 protobuf hello 中增加可选
`repeated string capabilities`，客户端和服务端分别声明能力，生效集合取交集；缺字段等价空集合，未知值
忽略。旧 protobuf 端依靠 unknown-field 兼容，不改变 TLS、GCM 和握手 version 校验。

若本轮不实施任何增强项，则不修改生产握手协议，只在 Native 内保留 `ServerCapabilities` 空集合和兼容
分支。当前秒传、Range 下载和整文件上传按既有 HTTP API 使用，不把它们错误地依赖于新 capability。

### ADR-03：Cutover 为单向 epoch

选择：不实现 Native→Room 反向导出。状态机固定为：

```text
LEGACY_ACTIVE
    │ snapshot + delta
    ▼
PREPARED
    │ 停写、追平、对账通过
    ▼
NATIVE_COMMITTED_NO_WRITE
    │ 首次 Native 业务事务 commit
    ▼
NATIVE_COMMITTED_DIRTY
```

- `LEGACY_ACTIVE/PREPARED` 失败可以继续 Legacy；
- `NATIVE_COMMITTED_NO_WRITE` 只有在 Native 未建立连接、未产生业务写且 journal 校验完整时才允许人工回滚；
- `NATIVE_COMMITTED_DIRTY` 永远禁止回到 Room，只能修复 Native、从服务端重建或执行未来单独设计的反向
  迁移工具；
- 后端选择只读 cutover journal，不再读取另一个 `cutover_ready` 布尔 marker；
- journal 的状态和 payload 存在 Native DB，同时在安全 KV 保存带 MAC 的最小镜像；两者不一致时 fail-close，
  按恢复矩阵处理，不猜测最新值。

### ADR-04：权威数据与派生数据

Room change log 只记录权威变化：

- `MESSAGE_UPSERT/MESSAGE_DELETE`；
- `CONVERSATION_UPSERT`；
- `READ_WATERMARK_UPDATE`；
- `MEDIA_STATE_UPDATE`。

FTS、会话 preview 和可重算 unread 不作为独立 delta 流。Native 在导入 message/conversation 的同一事务中
维护派生表；导入完成后执行 FTS identity、计数、水位和抽样 hash 校验。好友与申请当前不是 Room 实体，
Native 登录后从服务端重新同步，不从 Room 编造 snapshot。

### G0 验收

- [ ] 四份 ADR 标记 `ACCEPTED`，写明负责人、日期和不可变决策；
- [ ] 功能矩阵列出协议号、Legacy 入口、Native owner、事实源、用例和错误码；
- [ ] 必选能力与增强能力分栏，增强项关闭时有预期结果；
- [ ] cutover 恢复矩阵和 DB 解锁状态矩阵可机读并进入测试数据。

---

## 3. 实施顺序与工作包

### P7-G1：Golden Harness 与可测契约

交付：

- 领域 Golden：输入协议帧/命令，比较领域事件、数据库事务和稳定 DTO；
- UI instrumentation：只验证 Compose 状态映射和交互，不把 Compose 截图混入 C++ 单元 Golden；
- Fake clock、random、transport、filesystem、codec、secure KV、scheduler；
- `MessageDto/ConversationDto/FriendDto/FriendRequestDto/SearchHit/MediaTaskDto/AiSuggestionDto` 版本化定义；
- 协议 fuzz corpus：截断、尾随、未知字段、错误长度、超大计数、坏 GCM tag。

事件契约：

- state 是可重放快照；invalidation 携带单调 `dbVersion`，版本跳跃时重新 query；
- 网络 IO 和 DB Writer 永不等待 Kotlin collector；同 domain 的 invalidation 可合并为最大 dbVersion；
- operation completion 写入有界 registry，按 operationId exactly-once 终结，可查询直到确认消费或 TTL；
- 被踢/认证失败进入 AccountState 快照；进度允许合并，禁止把业务完成事件静默 drop。

验收：Legacy/Native 同一输入可以结构化 diff；失败输出首个字段差异；测试不依赖真实 sleep；collector
暂停、buffer 满、Activity 重建后能依靠 dbVersion 补查收敛。

### P7-G2：SQLCipher Schema v4 与 Repository

保留既有 001/002/003，新增 004。先于业务 Service 完成 0/1/2/3→4 升级验证，不改写旧脚本。

新增/调整：

| 实体 | 最小职责 |
|---|---|
| `friend_requests` | 申请 id、双方、方向、状态、serverVersion、时间；唯一键防重复 |
| `sync_gaps` | owner/conversation/from/to、attempt、nextRetry；区间合并拆分 |
| `media_variants` | mediaId、variant、真实 MIME、宽高、hash、localPath、状态 |
| `media_refs` | owner、msgId、mediaId、引用关系；授权仍以服务端为准 |
| `transfer_tasks` | 扩充 generation、阶段、offset、错误域、取消终态 |
| `transfer_parts` | 只为 `resumable_upload_v1` 预留；能力关闭时不产生记录 |
| `migration_checkpoint` | epoch、stream、snapshot/delta 水位 |
| `cutover_journal` | epoch、状态、摘要、schemaVersion、keyId、dirty 标志 |

Repository 必须提供业务级事务，例如 `commitIncomingMessage`、`commitOutgoingDraft`、`commitAck`、
`markConversationRead`、`upsertMediaAndMessage`，禁止 Service 拼接多个裸 DAO 操作。FTS 在消息事务中维护。
本地 `conversation_seq=0` 消息使用 `(local_order,msg_id)` 稳定排序；确认消息使用
`(conversation_seq,msg_id)`，查询层定义两者合并规则和 cursor 版本。

验收：空库 `0→2`、现有 Native `1→2`、迁移失败完整回滚；`EXPLAIN QUERY PLAN` 命中目标索引；10 万条
并发读写无 `SQLITE_BUSY`；FTS 插删改/重建一致；ASan+UBSan 通过。

### P7-G3：ClientRuntime、调度器与 SDK/JNI

`NativeSdkHandle` 唯一拥有账号级 `ClientRuntime`；Runtime 拥有 Account/Message/Sync/Friend/AI/Search/Media
服务、数据库和 runtime-owned completion executor。

SDK 至少提供：

```text
start/stop/register/login/logout/setForeground
sendText/sendImage/sendFile/cancelTransfer/retryMessage
loadConversations/loadHistory/markRead/search/jumpToMessage
loadFriends/loadFriendRequests/respondFriendRequest/deleteFriend
requestAiReply/cancelAiReply
observeAccount/observeConversations/observeMessages/observeTransfers/observeAi
```

所有命令返回 operationId，底层异步完成；JNI 不暴露 SQL、Socket、Token、DB key、`sqlite3*` 或裸指针。
destroy 顺序固定为 close、拒绝新任务、取消、drain/barrier、释放 JNI global ref，禁止自身线程 join。事件含
runtimeGeneration、ownerId、dbVersion/operationId；旧 generation 丢弃。

验收：start/stop/logout/destroy 可重入并发；双账号快速切换无串号；callback 重入、collector 暂停和 destroy
风暴无 UAF、死锁、永久 pending operation 或线程泄漏；CheckJNI 全绿。

### P7-G4：账号、文本、同步、好友与 AI

账号：迁移注册、密码/Token 登录、刷新 single-flight、防重入、心跳、重连、被踢、登出。注册和登录使用
不同 operationId；迟到注册/登录/刷新结果必须同时通过 requestId 和 runtimeGeneration 校验。

文本：

```text
SendIntent → 生成 msg_id/local_order
→ 单事务 messages(SENDING)+outbox+conversation
→ commit 后回显 → 安全通道发送
→ Ack 按 msg_id 合并 server_time/seq/status 并删除 outbox
```

接收的 Ack、Push、离线和漫游都进入同一 Inbox 合并入口。`msg_id` 负责幂等，`conversation_seq` 负责顺序。
未读只在首次提交的非本人新消息且当前会话未读时增加；本地 markRead 原子更新 read watermark/unread。

同步：维护 contiguousSeq、maxSeenSeq 和 `sync_gaps`。没有 `roam_range_v1` 时使用现有 beforeSeq 漫游分页
逐页追平，设置最大页数、最大持续时间和指数退避；预算耗尽保留 gap 并等待下次触发，不形成请求风暴。

好友：申请列表、同意/拒绝、删除、资料幂等落库；好友事实和在线事件分离，Presence 只表达路由状态。

AI：请求时冻结 conversationId、上下文版本和 tone；同一会话 single-flight；取消是 best-effort，但取消后
响应不得展示；超时、换会话、换号或 generation 改变均使旧响应失效。正文和候选不得写日志。

验收：Ack/Push/漫游排列组合、两端同时发送、离线重连、outbox 重启恢复、重复好友事件、AI 超时取消及
迟到响应均有确定结果；10 万消息压力无重复未读、丢失或无限队列。

### P7-G5：查询、搜索与定位

会话和历史使用版本化 keyset cursor；所有查询走 ReadPool 和值对象。保持现有搜索语义：中文/普通文本
FTS4 前缀加 LIKE 子串兜底；单英文字母只查 initials；多字母匹配全拼或首字母；按 msgId 去重并返回正文
高亮区间。`SearchHit` 必须携带 ownerId、conversationId、msgId、时间、原文和明确编码单位的高亮 ranges。

验收：`n` 命中“你/年”而不命中“真/正”；中文、全拼、首字母、多关键词、会话过滤、100 条上限与 Legacy
Golden 一致；点击搜索结果能分页加载并稳定定位；10 万/100 万消息记录 P50/P95/P99。

### P7-G6：图片、文件与存储安全

图片链路：

```text
SAF URI → 授权窗口内复制到账号临时目录
→ Native 流式 hash + 平台探测方向/色彩
→ Native 下发确定编码参数 → 平台输出原图/大图/小图
→ Native 校验魔数/MIME/尺寸/hash
→ 秒传 preflight+proof 或整文件上传 → 授权接收者 → 发送消息
```

`IMediaCodec` DTO 包含 source/output color space、ICC 处理、EXIF orientation、alpha、像素上限、encoder
版本和真实 MIME。JPEG 字节不得标记 AVIF。展示按本地原图→大图→小图→占位选择，后台下载更高清版本；
协议宽高先驱动等比占位，替换图片不改变布局。

文件保持现有秒传、整文件流式上传和 Range `.part` 下载，总 hash 通过后 fsync+原子 rename。worker 只持
`shared_ptr<TaskState>`，回 Runtime 时通过 weak_ptr 和 generation。取消后任务终态不可复活。

存储：canonical path 必须位于账号沙箱；防 `..`、符号链接和 MIME 伪造；引用关系与消息同事务更新，物理
删除在 commit 后；相同 hash 的跨账号对象不能泄漏存在性或绕过授权；设置缓存、并发、像素、内存和磁盘
上限。

验收：JPEG/PNG/AVIF、透明、EXIF、ICC/Display-P3、超宽/超高/超大视觉 Golden；用像素误差/SSIM/Delta-E
阈值而非“能打开”；秒传命中只传 proof，未授权下载失败；URI 失效、低磁盘、进程中断、hash 错、取消和
跨账号同 hash 均有负向测试。

### P7-G7：Room 8→9 Change Log 与在线迁移

Android 交付 `MIGRATION_8_9`、`LegacyChangeLogEntity/Dao` 和唯一 `LegacyWriteGateway`。Room 当前权威实体
只有 messages、messages_fts、conversations；所有 messages/conversations 写入口必须通过 gateway，在同一
Room 事务追加权威 change log。FTS 仍由 Legacy 原事务维护供 Legacy 使用，但不作为独立 delta stream。

迁移来源：

| Native 数据 | 来源/策略 |
|---|---|
| messages | Room snapshot + MESSAGE delta |
| conversations/read watermark | Room snapshot + conversation/read delta |
| FTS | Native 根据导入消息重建并校验 |
| friends/friend_requests | Native 登录后由服务端重新同步 |
| media_variants/media_refs | 从 MessageEntity 媒体字段投影并检查本地文件 |
| transfer_tasks | 不复制活协程；cutover 前等待、取消或将可验证 `.part` 转为可恢复任务 |
| sync_gaps | 按已导入 seq 与服务端漫游结果重算 |

流程：建立 epoch/high-water；分页 snapshot；按全局 changeSeq 重放 delta；最终获取停写闸门；追平最终
high-water；逐表计数、关键水位、FTS identity、媒体存在性和抽样 hash 对账；写 `PREPARED`；确认两侧
持久化完成后写 `NATIVE_COMMITTED_NO_WRITE`，然后重启进入 Native。change log 使用 tombstone，Native
确认 checkpoint 前不得 GC。

验收：静态枚举所有 Room writer，并以测试 hook 检测绕过 gateway 的写；从仍支持的每个 Room schema 版本
升级到 9；snapshot 期间持续收发/markRead/图片下载；delta 重复、倒序、删除、缺口和强杀可恢复；最终停写
窗口 P95/P99 ≤ 200ms，达不到不切换。

### P7-G8：单向 Cutover、Android 薄 UI 与发布

`KernelBackendSelector` 只根据 ADR-03 journal 在进程启动选后端，进程内不可热切。Native UI 路径只依赖
JitongSdk，不引用 ImClient、ChatStore、AppDatabase、HttpMediaClient。首次 Native 业务事务 commit 前由
Database transaction hook 把 journal 推进到 `NATIVE_COMMITTED_DIRTY`，不能依赖上层“记得标记”。

发布先账号级灰度；Legacy Room 只读封存一个观察窗口，不自动删除。进入 DIRTY 后发生故障，UI 展示可恢复
错误并优先进行 Native 修复或服务端重建，禁止静默启动 Legacy。

验收：journal 每一状态、KV/DB 写入与 fsync 前后、数据库 commit 前后强杀，恢复路径唯一；DIRTY 后制造
新消息再损坏启动条件，确认不会回旧 Room；静态依赖门禁、Release/R8 JNI keep、arm64 ABI/so 加载、build-id
和符号化、APK/so 体积、冷启动均有证据。

### P7-G9：全量验证与结论

必跑：C++ 单元/Golden/E2E、Schema 0→2 和 1→2、Room 各支持版本→9、ASan+UBSan、Android arm64
CheckJNI、真实双端文本/好友/AI/图片/文件、弱网/断网/进程强杀、2 小时长稳和迁移 cutover。

条件性：TSAN 和 x86_64 运行。不可用时记录 `NOT_AVAILABLE`、环境原因、替代证据和风险接受人，不能记 PASS。

---

## 4. 依赖关系与提交策略

```text
G0 ADR/矩阵
 ├─ G1 Harness
 └─ G2 Schema/Repository
       └─ G3 Runtime/SDK
            ├─ G4 账号/消息/同步/好友/AI
            ├─ G5 查询/搜索
            └─ G6 图片/文件
                  └─ G7 Room delta/迁移
                        └─ G8 Cutover/薄 UI
                              └─ G9 全量验证
```

每个 G 独立提交并附对应测试。G1～G7 只允许测试或影子运行，生产仍由 Legacy 单独承载；G8 才允许一次
切换。禁止巨型提交，禁止在同一账号上让 Kotlin 和 Native 同时建连接或共同写业务数据。

---

## 5. 最终验收门禁

### 5.1 功能

- [ ] 1.1 中每项都有 Legacy 对照、Native owner、自动化用例和 E2E 证据，无“暂走 Legacy”；
- [ ] 注册、登录、Token、AI、好友、文本、同步、搜索、图片和文件的失败/超时/取消/重启路径闭环；
- [ ] UI 排序、未读、搜索高亮与定位、图片清晰度及错误提示不低于 Legacy。

### 5.2 架构与并发

- [ ] Kotlin 仅保留 UI 和平台原子能力；进程内一个 Runtime、Socket、Writer 和事实源；
- [ ] IO/Writer 不等待 UI collector；dbVersion 跳跃可补查；operation exactly-once；
- [ ] start/stop/logout/destroy、刷新/重连、Ack/Push/漫游、任务取消并发无 UAF/死锁/串号/状态复活。

### 5.3 数据与切换

- [ ] Native Schema v4 和目标 Room v9 可从全部支持版本无损升级；失败完整回滚（Room 当前仍 v8）；
- [ ] snapshot+权威 delta+停写+对账+journal 完整，派生 FTS 可重建；
- [ ] 每个强杀点恢复结果唯一；进入 DIRTY 后绝不回到陈旧 Room；
- [ ] DB 双 wrapper、schema、epoch、high-water、keyId 和 journal 校验闭环。

### 5.4 安全、媒体与性能

- [ ] TLS/SPKI、应用层 nonce/sequence、防重放、Token 与设备证明不退化；
- [ ] 文件授权、秒传 proof、总 hash、真实 MIME、路径和资源上限生效；
- [ ] 日志/crash/测试制品不含密码、Token、DB key、正文、完整文件路径或媒体字节；
- [ ] 文本首屏、Ack、历史和搜索 P95 相对 Legacy 不退化超过 10%；图片流量和存储收益有实测数据；
- [ ] ASan+UBSan、CheckJNI 和所有必跑门禁全绿，无跳过。

最终只有两种结论：全部必选门禁满足时为 `PASS`；任一必选门禁未满足、失败或无证据时为
`CHANGES REQUIRED`。增强项单独报告，不影响现有功能迁移结论，也不得混入 PASS 宣称。

---

## 6. 交付物

```text
outputs/kernel-round7-adr-db-unlock.md
outputs/kernel-round7-adr-capabilities.md
outputs/kernel-round7-adr-cutover.md
outputs/kernel-round7-feature-parity.md
outputs/kernel-round7-acceptance.md
outputs/kernel-round7-golden.log
outputs/kernel-round7-unit.log
outputs/kernel-round7-e2e.log
outputs/kernel-round7-migration-cutover.log
outputs/kernel-round7-android-arm64.log
outputs/kernel-round7-checkjni.log
outputs/kernel-round7-asan-ubsan.log
outputs/kernel-round7-tsan.log
outputs/kernel-round7-performance.md
outputs/kernel-round7-media-bandwidth.md
outputs/kernel-round7-security-review.md

client_core/src/storage/migrations/002_p7_state_machines.sql
jitong_android/.../data/db/MIGRATION_8_9
jitong_android/.../data/migration/LegacyWriteGateway
jitong_android/.../core/JitongSdk.kt
jitong_android/.../core/NativeBindings.kt
client_core/include/client_core/{runtime,message,sync,friend,ai,search,media,migration}/*
client_core/src/{runtime,message,sync,friend,ai,search,media,migration}/*
```

每份验收材料必须记录 commit、构建类型、设备、系统、ABI、命令、通过/失败/跳过数量及原始日志路径。
报告中写出的代码路径、协议号、Schema 版本和测试数量必须可由仓库复核。
