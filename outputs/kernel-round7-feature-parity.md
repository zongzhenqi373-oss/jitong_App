# 第七轮功能矩阵与恢复矩阵（P7-G0）

| 项 | 值 |
|---|---|
| 状态 | G0 交付物（供评审） |
| 日期 | 2026-09-10 |
| 协议事实来源 | `jitong_android/.../net/Protocol.kt`（须与 `client_core/.../Protocol.h` 及服务端一致） |
| 关联 ADR | `kernel-round7-adr-db-unlock.md`、`kernel-round7-adr-capabilities.md`、`kernel-round7-adr-cutover.md` |

> 线格式：`[4B 大端包长(含协议号)][4B 小端协议号][pb payload]`；`DEF_BASE = 1000`。
> 本矩阵**不得**使用"其它功能/顺便迁移"作为责任项。

---

## 1. 必选能力矩阵（现有已交付能力，必须迁移）

### 1.1 账号与认证

| 能力 | 协议号 | Legacy 入口 | Native owner | 事实源 | 错误码/结果 | 必备用例 |
|---|---|---|---|---|---|---|
| 注册 | `REGISTER_RQ/RS` 1000/1001 | `ImClient.register(nick,tel,pass)` | `AccountSession` | 服务端 | `REGISTER_SUCC=1`/`NICK_EXIT=2`/`TEL_EXIT=3`/`INVALID=4` | 成功、昵称重复、手机号重复、超时、取消、重启后迟到响应不改写登录态 |
| 密码登录 | `LOGIN_RQ/RS` 1002/1003 | `ImClient.login(tel,pass,deviceId)` | `AccountSession` | 服务端 + TokenVault | `LOGIN_SUCCESS=0`/`NOTEXIT=1`/`PASSERROR=2`/`INVALID=3`/`RATE_LIMITED=4` | 成功、密码错、限流、超时、防重入 |
| Token 登录 | `TOKEN_LOGIN_RQ/RS` 1023/1024 | `ImClient.loginWithToken(session,deviceId)` | `AccountSession` | TokenVault(Keystore) | 同上 + 刷新分支 | 冷启动无感登录、Token 过期转刷新 |
| Token 刷新 | `TOKEN_REFRESH_RQ/RS` 1025/1026 | `ImClient.refreshToken(session,deviceId,requestId)` | `AccountSession`（single-flight） | TokenVault | `REFRESH_TOKEN_SUCCESS=0`/`FAIL=1` | 并发刷新只发一次、失败后转密码登录 |
| 登出 | `LOGOUT_RQ/RS` 1027/1028 | `ImClient.logout()` | `AccountSession` | TokenVault | `LOGOUT_SUCCESS=0`/`FAIL=1` | 成功、失败、登出后内存 key 清零 |
| 撤销会话 | （随 logout） | `ImClient.revokeSession(session,deviceId,allDevices)` | `AccountSession` | 服务端 | — | 单设备/全部设备 |
| 心跳 | `HEARTBEAT_RQ/RS` 1010/1011 | `ImClient` 私有 `startHeartbeat`（30s） | `AccountSession` | — | — | 前后台、超时重连、不因心跳触发重登录 |
| 被踢 | `KICKED_OFFLINE` 1012 | `Event.KickedOffline` | `AccountSession` 快照 | — | — | 被踢后禁止自动重登、状态进快照 |
| 安全握手 | `APP_CLIENT_HELLO` 1036 / `APP_SERVER_HELLO` 1037 / `APP_*_FINISHED` 1038/1039 / `APP_ENCRYPTED_FRAME` 1040 | `SecureChannel.handshake()` | `ClientSecureChannel` | — | `version != 1` 即失败 | 版本校验、签名校验、防降级（见 ADR-02） |

### 1.2 消息

| 能力 | 协议号 | Legacy 入口 | Native owner | 事实源 | 错误码 | 必备用例 |
|---|---|---|---|---|---|---|
| 发送文本 | `CHAT_INFO_RQ/RS` 1005/1006 | `ImClient.sendChat(friId,text,msgId)` | `MessageService` | 迁移前 Room / 后 Native | `CHAT_RESULT_SUCC=0`/`FAIL=1(离线转存)`/`NOT_FRIEND=2`/`SERVER_ERROR=3` | 本地回显、outbox、假成功、失败重试、Ack 按 msg_id 合并 |
| 发送媒体卡片 | 同上 | `ImClient.sendMediaCard(...)`（含 thumbnail/largeThumbnail 12 字段） | `MessageService`+`MediaService` | 同上 | 同上 | 缩略图字段完整（P6 已修）、授权接收者 |
| 接收消息 | `CHAT_INFO_RS` / 事件 | `Event.ChatReceived` → `ChatStore.save()` | `MessageService` Inbox | 同上 | — | Ack/Push/离线/漫游**同一入口**合并、幂等 |
| 回执 | — | `Event.ChatSendResult` | `MessageService` | 同上 | `CHAT_RESULT_*` | 成功/离线转存/非好友/服务错四种状态 |
| 未读/已读 | — | `ChatStore.clearUnread(ownerId,peerId)` | `MessageService` | 迁移前 Room / 后 Native | — | 仅首次提交的非本人新消息且会话未读时 +1；markRead 原子更新 watermark/unread |

### 1.3 同步

| 能力 | 协议号 | Legacy 入口 | Native owner | 事实源 | 必备用例 |
|---|---|---|---|---|---|
| 会话漫游 | `ROAM_CONV_RQ/RS` 1013/1014 | `ImClient.roamConversations()` | `SyncService` | 服务端 | 增量、并发、重连后补 |
| 历史漫游 | `ROAM_MSG_RQ/RS` 1015/1016 | `ImClient.roamMessages(peerId,beforeSeq,limit)` | `SyncService` | 服务端 | 分页、hasMore、minSeq、与本地合并 |
| 缺洞补洞 | 无 `roam_range_v1` 时用 1015/1016 | 同上（逐页追平） | `SyncService` + `sync_gaps` | Native | 最大页数/时长/指数退避；预算耗尽保留 gap，不形成请求风暴 |

### 1.4 好友

| 能力 | 协议号 | Legacy 入口 | Native owner | 事实源 | 错误码 | 必备用例 |
|---|---|---|---|---|---|---|
| 好友资料 | `FRIEND_INFO` 1004 | `Event.UserOrFriendInfo` | `FriendService` | **服务端**（非 Room 实体） | — | 幂等落库、资料更新 |
| 添加好友 | `ADD_FRIEND_RQ/RS` 1007/1008 | `ImClient.sendAddFriendRq/sendAddFriendRs` | `FriendService` | 服务端 | `ADD_FRIEND_AGREE=0`/`REJECT=1`/`OFFLINE=2`/`NOTEXIT=3`/`SELF=4`/`ALREADY=5`/`PENDING=6`/`DB_ERROR=7` | 同意/拒绝/离线/不存在/自己/已好友/待审/DB错 |
| 好友上下线 | `FRIEND_OFFLINE` 1009 | `Event.FriendOffline` | `FriendService`（Presence 仅路由状态） | 服务端 | `STATUS_ONLINE=0`/`OFFLINE=1` | 在线事件**不作为**好友事实源 |
| 申请列表 | `FRIEND_REQUEST_LIST_RQ/RS` 1034/1035 | `ImClient.requestFriendRequests()` | `FriendService` + `friend_requests` | 服务端 → Native | — | 列表、方向、状态、去重 |
| 删除好友 | `DELETE_FRIEND_RQ/RS` 1032/1033 | `ImClient.deleteFriend(friendId)` | `FriendService` | 服务端 → Native | `DELETE_FRIEND_SUCCESS=0`/`DB_ERROR=1`/`NOT_FRIEND=2`/`INVALID=3` | 成功/非好友/无效、本地级联更新 |

### 1.5 AI

| 能力 | 协议号 | Legacy 入口 | Native owner | 事实源 | 必备用例 |
|---|---|---|---|---|---|
| AI 回复建议 | `AI_REPLY_RQ/RS` 1029/1030 | `ImClient.requestAiReply(peerId,requestId,tone,maxSuggestions)` | `AiSuggestionService` | 服务端 | 冻结 conversationId+上下文版本+tone；同会话 single-flight；20s 超时 |
| AI 取消 | `AI_CANCEL_RQ` 1031 | `ImClient.cancelAiReply(requestId)` | `AiSuggestionService` | — | 取消 best-effort，但取消后响应**不得展示**；超时/换会话/换号/generation 变更均使旧响应失效 |

### 1.6 查询与搜索（本地，无协议号）

| 能力 | Legacy 入口 | Native owner | 事实源 | 必备用例 |
|---|---|---|---|---|
| 会话列表 | `ChatStore.hydrate()` | `SearchService`/Repository | 迁移前 Room / 后 Native | keyset cursor、并发插入不重不漏 |
| 历史 | 同上 | 同上 | 同上 | `(conversation_seq,msg_id)` 稳定排序；本地 `seq=0` 用 `(local_order,msg_id)` |
| 搜索（FTS4 + LIKE 兜底） | `ChatStore` 搜索 | `SearchService` | Native 重建 FTS | `n` 命中「你/年」不命中「真/正」；中文/全拼/首字母/多词/会话过滤/100 上限与 Legacy Golden 一致 |
| 跳转定位 | — | `SearchService` | — | `SearchHit` 携带 ownerId/conversationId/msgId/时间/原文/明确编码单位的高亮 ranges |

### 1.7 图片与文件（HTTPS 文件服务，`HTTPS_FILE_PORT=24564`；1017–1022 已废弃）

| 能力 | Legacy 入口 | Native owner | 事实源 | 约束 | 必备用例 |
|---|---|---|---|---|---|
| 秒传 | `HttpMediaClient.tryInstantUpload()`（preflight+proof） | `MediaService` | 服务端 | proof 与 token/deviceId/授权绑定、过期、防重放 | 命中只传 proof；未授权失败 |
| 整文件上传 | `HttpMediaClient.upload()` | `MediaService` | 服务端 + `transfer_tasks` | `FILE_MAX_SIZE=100MB`、`MAX_PACK_LEN=10MB` | 流式、进度、取消、失败重试 |
| Range 下载 | `HttpMediaClient.download()` | `MediaService` | 服务端 + `.part` | 总 hash 通过后 fsync + 原子 rename | 续传、hash 错、磁盘满、取消 |
| 图片编码三档 | 平台 Codec | `MediaService` + `IMediaCodec` | `media_variants` | JPEG 字节**不得**标 AVIF；ICC/EXIF/方向/色彩空间规范化 | 像素误差/SSIM/Delta-E Golden，非"能打开" |
| 存储安全 | — | `MediaService` | `media_refs` | canonical path 在账号沙箱；防 `..`/符号链接/MIME 伪造；跨账号同 hash 不泄漏存在性 | 负向测试全套 |

---

## 2. 增强能力（本轮**不宣称完成**，服务端交付后单独标记）

| capability | 能力 | 未启用降级 | 关闭测试 |
|---|---|---|---|
| `resumable_upload_v1` | 分片上传 | 整文件流式上传 | 显式关闭时上传仍可用，不无限重试；`transfer_parts` 不产生记录 |
| `roam_range_v1` | 按缺洞区间精确拉取 | 现有 `beforeSeq` 分页逐页追平（有页数/时长/退避预算） | 关闭时补洞仍收敛，不构造不存在的请求 |
| `read_sync_v1` | 多设备已读水位同步 | 仅本地已读 | 关闭时 markRead 仍正确 |

> `instant_proof_v1` / `range_download_v1` **仅作前向兼容声明**，不作为启用条件（见 ADR-02）。

---

## 3. DB 解锁状态矩阵（可机读，进入测试数据）

维度：`记住密码? × Token 有效? × Keystore 可用? × 密码变更? × wrapper/blob 状态?`

| id | rememberPwd | tokenValid | keystoreOk | pwdChanged | blobState | expected | mustNot |
|---|---|---|---|---|---|---|---|
| U01 | true | true | true | false | ok | `READY`(deviceWrapper) | — |
| U02 | true | true | false | false | ok | `NEEDS_PASSWORD_UNLOCK`→密码解锁→`READY` | 新建空库 |
| U03 | true | false | true | false | ok | 刷新后 `READY` | 建空库 |
| U04 | false | true | true | false | ok | `READY`(deviceWrapper) | — |
| U05 | false | true | false | false | ok | `NEEDS_PASSWORD_UNLOCK`→输入密码→`READY` | 建空库 |
| U06 | — | — | — | true | ok | 需旧密码/已解锁态原子替换 passwordWrapper | 覆盖旧 blob |
| U07 | — | — | — | — | wrapper 截断 | 该 wrapper 解失败→转另一路径→否则锁定 | 建空库 |
| U08 | — | — | — | — | GCM tag 错 | 同上（篡改检测） | 建空库 |
| U09 | — | — | — | — | 磁盘写失败 | 原子替换失败→保留旧 blob | 半写状态 |
| U10 | — | — | — | — | 两 wrapper 均失败 + 本地库存在 | `LOCKED_WITH_DATA`（UI 明确提示） | **静默清空** |

每条断言：不创建空库、不覆盖旧 blob、不 destructive migration、不因失败生成新 realKey。

---

## 4. Cutover 恢复矩阵（可机读，进入测试数据）

维度：`DB journal 状态 × KV 镜像一致性 × Room/Native 存在性`

| id | dbJournal | kvMirror | room | native | expected | mustNot |
|---|---|---|---|---|---|---|
| C01 | none | none | yes | no | 继续 Legacy | — |
| C02 | `LEGACY_ACTIVE` | consistent | yes | any | 继续 Legacy | — |
| C03 | `PREPARED` | consistent | yes | yes | 继续 Legacy，可重做对账 | — |
| C04 | `PREPARED` | missing | yes | yes | **fail-close**：继续 Legacy | 猜测最新值 |
| C05 | `PREPARED` | inconsistent | yes | yes | **fail-close**：继续 Legacy | 猜测最新值 |
| C06 | `NO_WRITE` | consistent | yes | yes | 进入 Native | — |
| C07 | `NO_WRITE` | inconsistent | yes | yes | **fail-close**：Repair，禁止自动 Legacy | `test_recovery_resolver`（纯决策；启动接线未验收） |
| C08 | `DIRTY` | consistent | yes | yes | 进入 Native | **回 Room** |
| C09 | `DIRTY` | inconsistent | yes | yes | **fail-close**：Native 只读/等待修复 | **回 Room** |
| C10 | `DIRTY` | consistent | yes | corrupt | 修复 Native / 服务端重建 | **静默启动 Legacy** |

强杀点：journal 每一状态、KV/DB 写入与 fsync 前后、DB commit 前后，冷启动恢复结果**唯一**。

---

## 5. G0 验收自检

- [ ] 四份 ADR 标记 `ACCEPTED`（db-unlock / capabilities / cutover；ADR-04 已并入 cutover ADR）
- [ ] 功能矩阵含协议号、Legacy 入口、Native owner、事实源、用例、错误码（§1）
- [ ] 必选与增强分栏，增强项关闭时有预期结果（§2）
- [ ] cutover 恢复矩阵与 DB 解锁状态矩阵可机读并进入测试数据（§3、§4）
- [ ] 无"其它功能/顺便迁移"责任项
