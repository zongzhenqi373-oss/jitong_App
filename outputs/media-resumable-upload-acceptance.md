# 媒体断点续传协议 — 实施与验收报告

> 目标协议：秒传优先、未命中则分片上传、下载继续用 Range。
> 整文件 `POST /api/v1/upload` 保留为旧客户端兼容路径。

## 一、协议与实现清单

### 服务端（im_server）

| 端点 | 语义 | 实现 |
|---|---|---|
| `POST /api/v1/uploads` | 创建上传会话：先秒传预检（命中→PoP 挑战）；未命中→返回 upload_id/1MiB 分片/24h 有效期/已收列表。upload_id 绑定上传人+设备+接收人+摘要+大小；同人同设备同文件重试复用 open 会话 | `HttpFileServer::handleCreateUpload` |
| `PUT /uploads/{id}/chunks/{index}` | 鉴权+归属校验（upload_id 不是授权凭证）；片大小由服务端按会话元数据计算；收流后校验分片摘要；按编号 `seekp` 定位写临时文件（不信任客户端偏移）；完成分片持久化 `upload_chunks`；同片同内容幂等、不同内容 409 | `handleUploadChunk` |
| `GET /uploads/{id}` | 恢复事实源：会话状态+已收分片列表 | `handleGetUpload` |
| `POST /uploads/{id}/finalize` | 齐片校验→整文件重算 SHA-256+大小→原子转正（图片内容寻址去重）→登记 `media_objects`（秒传立即可命中，不等消息发送）→回填 file_id（由 upload_id 派生，天然幂等）→授权记录 | `handleFinalizeUpload` |
| `DELETE /uploads/{id}` | 取消：状态终态 + 删临时文件；已 finalize 409 | `handleCancelUpload` |
| GC | 过期 open/cancelled 会话：删临时文件+DB 记录 | `gcLoop` 扩展 |

新增表：`upload_sessions`、`upload_chunks`（PK 幂等判定）、`media_objects`（内容寻址秒传索引）；`findMediaObject` 先查索引再回退 messages 表。

协议修正：chunk 端点所有错误路径先排空请求体再响应（否则客户端写大 body 时被 RST，只能看到 EPIPE 而非错误码）。

### Native（client_core）

- migration 007 `upload_drafts`：固定 msg_id + variant（图片原图/大图/小图独立状态共享一条消息草稿）+ upload_id + chunks_done + file_id + 状态机（Pending/Uploading/Finalized/Sent/Cancelled/Failed）。
- `NativeRepository`：登记幂等（保留进度）、会话写入（终态不可回退）、分片确认幂等、finalize 同事务回填 file_id（不同 file_id 拒绝）、终态保护、会话丢失重置、活跃扫描。所有 UPDATE 校验 `sqlite3_changes`。
- `MediaService`（编排，可注入 `IUploadTransport`）：
  - 恢复纪律：先 `querySession` 以服务端为准合并，再只传缺失分片；404/410 → 重建会话清空进度；服务端已 finalized 而本地未记录 → 直接采用服务端 file_id。
  - 全部 variant Finalized → 以固定 msg_id `commitOutgoingDraft` 落 messages+Outbox（重试时 `findMessage` 幂等，不重复发消息）；成功后唤醒 Runtime Outbox 泵真实发送，ACK/重试由既有 Outbox 状态机负责。
  - 取消（Cancelled 终态，不自动恢复）与网络异常（Failed 可恢复）严格区分。
- `ClientCoreUploadTransport`（生产）：复用 ClientCore 的 TLS+token 媒体通道；4xx=确定性拒绝，5xx/本地=可恢复；统计实际发送字节数。

### Android

- JNI：`nativeMediaEnqueueUpload`（C++ 流式算 SHA-256/大小）/`nativeMediaPumpUpload`/`nativeMediaResumeUploads`/`nativeMediaCancelUpload`/`nativeMediaUploadState`；MediaService 懒创建挂句柄，随句柄按依赖序销毁。
- `NativeMediaController`：发送改走草稿+泵（退避重试 1s/2s/4s，终态即停，仍失败保留草稿待恢复）；取消直接终止草稿；登录就绪后 `resumeUploads` 自动恢复。
- 旧整文件上传 `sdk.uploadMedia` 与 Legacy `HttpMediaClient` 未动（旧客户端兼容路径）。

## 二、验收矩阵

| 验收项 | 覆盖 | 结果 |
|---|---|---|
| 秒传命中 | `test_chunked_upload` §7（finalize 后即命中）+ `media_resume_e2e` §3 | PASS |
| 传到一半断网→恢复只传缺失 | `test_media_service` §3（1/3 片后断网，恢复只补 2 片，字节数断言） | PASS |
| 强杀恢复 | `test_media_service` §4（销毁重建，DB 草稿+服务端状态恢复）+ `media_resume_e2e` §2（服务端重启会话持久化） | PASS |
| 同片重复提交幂等 | `test_chunked_upload` §2 | PASS |
| 乱序分片 | `test_chunked_upload` §2（先传第 1 片） | PASS |
| 同片号不同内容冒用 | `test_chunked_upload` §2 → 409 | PASS |
| finalize 前缺片 | `test_chunked_upload` §3 → 409 missing_chunks | PASS |
| 整文件摘要不符 | `test_chunked_upload` §5 → 409 sha256_mismatch | PASS |
| 取消后重启不恢复 | `test_chunked_upload` §6（410/幂等取消）+ `test_media_service` §7 + Android `NativeUploadDraftTest` | PASS |
| 文件完成但消息发送前强杀 | `test_media_service` §5/§6（Finalized 崩溃→重启补发；findMessage 幂等不重复） | PASS |
| 消息已发未收 ACK | Outbox 状态机既有覆盖 + `media_resume_e2e` 消息卡片经 Outbox 发送 | PASS |
| 下载 Range 续传 | `media_resume_e2e` §1（下载校验内容一致）；既有 Range 语义不变 | PASS |
| upload_id 越权 | `test_chunked_upload` §2/§4（非上传人 403、非好友 403、无 token 401） | PASS |

## 三、性能数据（本机回环，2.5MiB 文件，1MiB 分片）

| 场景 | 耗时 | 实际传输字节 |
|---|---|---|
| 首传（3 片全量） | 56 ms | 2,109,497（=文件大小，无重复） |
| 续传（已传 1/3） | 40 ms | 1,060,921（只补缺失 2 片） |
| 秒传 | 15 ms | 65,536（仅 64KiB PoP 证明） |

（`media_resume_e2e` 实测输出；传输字节由生产传输层计数器统计。）

## 四、回归

- im_server CTest：**15/15 PASS**（含新增 `chunked_upload`、`media_resume_e2e`）
- client_core ASan/UBSan CTest：**40/40 PASS**（含新增 `upload_draft`、`media_service`；schema 升至 v7）
- Android：静态依赖门禁 PASS；双 ABI Debug/AndroidTest 构建通过；Pixel 7 Android 14 联合回归 **17/17 PASS**（含新增 `NativeUploadDraftTest`）

## 五、边界与后续

- 上传与下载两侧现在都是断点续传（上传=分片会话，下载=Range+.part 校验）。
- 服务端重启后 finalize 授权记录（`m_uploads`）仍在内存——接收人下载依赖消息绑定路径不受影响；纯上传记录的内存性属既有设计，如需服务端重启后免消息直接下载需再持久化 `upload_records`。
- 图片消息的 imgW/H 与缩略图尺寸字段由 Android 侧 ImageCodec 元数据在草稿登记时补齐（本轮草稿表未含尺寸列，消息卡片尺寸字段沿原路径由 sendMedia 元数据写入——后续如需草稿全字段化可加列）。

## 六、后续修正：图片尺寸与取消入口（2026-09-17）

- 上一节最后一条关于“尺寸字段沿原 sendMedia 路径写入”已不符合当前分片上传实现：
  新路径由 MediaService 从 upload_drafts 组装消息卡片，之前确实遗漏了宽高。本次以
  Native Schema v8 增量迁移增加每个 variant 的 image_width/image_height；Android
  登记草稿时传入 ImageCodec 的原图/大图/小图尺寸，消息卡片从持久草稿恢复这些字段。
  旧 v7 草稿迁移后尺寸为 0（未知），不伪造尺寸。
- Native 聊天页媒体任务列表改按当前固定 msg_id 前缀 `m-` 显示；不再沿用旧上传路径
  的 `img_`/`file_` 前缀，否则用户看不到正在上传任务的取消入口。
- 定向验证：ClientCore `schema_v3`、`message_service`、`media_service` 3/3 PASS；
  Android 双 ABI Debug/AndroidTest 构建通过；Pixel 7 Android 14
  `NativeUploadDraftTest` 2/2 PASS。尚未做真实界面点击取消与强杀恢复的设备级验收。
- 全量 ClientCore CTest：沙箱内 38/40 PASS，`integration` 与 `transport` 因本机
  TCP `bind` 被沙箱拒绝；授权沙箱外单独重跑 2/2 PASS，合计 40 项均通过。
