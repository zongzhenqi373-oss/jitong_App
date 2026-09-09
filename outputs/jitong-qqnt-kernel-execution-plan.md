# 即通 QQNT 式薄 UI + 厚内核——可执行任务规划

> 来源：`outputs/jitong-qqnt-thin-ui-thick-kernel-plan-v3.md`
> 执行原则：以当前 Android 行为为兼容基线；每一阶段通过验收门禁后才能切换下一条真实业务链路；禁止新旧核心同时连接或同时写同一份业务数据。

## 0. 使用方法

每个任务统一按下面的闭环执行：

```text
Plan → Implement → Unit Test → Integration Test → Review → Fix → Verify → Commit
```

任务状态：

- `[ ]` 未开始
- `[-]` 执行中
- `[x]` 已完成并通过验收
- `[!]` 阻塞，必须记录原因

完成任务时必须在任务下面补充：

```text
完成提交：<commit>
验证命令：<commands>
验证结果：<result>
遗留问题：<issues or none>
```

## 1. 全局约束

以下规则适用于全部任务：

1. 不删除或覆盖当前 Kotlin 实现，直到 Native Kernel 灰度验收完成。
2. `KernelBackend` 在进程启动前确定，进程运行期间不可热切换。
3. 一个进程只允许一个长连接实现处于活动状态。
4. 一个账号同一时刻只能有一个消息数据库事实源。
5. C++ 网络线程禁止执行数据库写入、图片编码和大文件摘要计算。
6. JNI 回调只复制数据并投递事件，不执行 Kotlin 业务。
7. Token、密码、中间密钥不得写入日志。
8. 所有协议新增字段遵循 protobuf 兼容规则：不修改已使用字段号，不复用删除字段号。
9. 每次迁移必须以当前 Android 行为作为 Golden Test 对照。
10. 构建、单测和关键集成测试失败时禁止进入下一阶段。

## 2. 目标目录

```text
client_core/
├── include/client_core/
│   ├── JitongSdk.h
│   ├── Models.h
│   ├── Events.h
│   └── PlatformServices.h
├── src/
│   ├── sdk/
│   ├── account/
│   ├── transport/
│   ├── message/
│   ├── storage/
│   ├── search/
│   └── media/
└── tests/
    ├── unit/
    ├── integration/
    └── golden/

jitong_android/app/src/main/
├── cpp/
│   ├── CMakeLists.txt
│   ├── im_core_jni.cpp
│   ├── native_sdk_handle.h
│   ├── jni_observer.cpp
│   └── android_platform_services.cpp
└── java/com/jitong/im/core/
    ├── JitongSdk.kt
    ├── NativeBindings.kt
    ├── NativeEventSink.kt
    ├── KernelBackend.kt
    └── platform/
        ├── AndroidKeystoreProvider.kt
        ├── AndroidFileProvider.kt
        ├── AndroidImagePlatform.kt
        ├── AndroidSecureKv.kt
        └── AndroidNetworkObserver.kt
```

## 3. 依赖关系

```mermaid
flowchart LR
    P0["P0 Android基线"] --> P1["P1 Native构建"]
    P1 --> P2["P2 JNI生命周期"]
    P1 --> P3["P3 Transport"]
    P2 --> P4["P4 安全通道"]
    P3 --> P4
    P4 --> P5["P5 Account/Token"]
    P5 --> P6["P6 C++数据库"]
    P5 --> P7["P7 消息收发"]
    P6 --> P7
    P7 --> P8["P8 漫游补洞"]
    P6 --> P9["P9 搜索"]
    P2 --> P10["P10 Media SDK骨架"]
    P5 --> P11["P11 秒传与传输"]
    P10 --> P11
    P6 --> P12["P12 媒体消息与缓存"]
    P8 --> P13["P13 Android UI切换"]
    P9 --> P13
    P11 --> P13
    P12 --> P13
    P13 --> P14["P14 灰度清理"]
```

可并行组：

- P2 JNI 生命周期与 P3 Transport 可并行。
- P9 搜索与 P10/P11 Media SDK 可并行。
- 单元测试与当前模块实现同步进行，不允许最后集中补测试。

---

# P0：冻结 Android 行为基线

## P0-T01 建立基线测试目录

- [ ] 创建 `client_core/tests/golden/`。
- [ ] 创建 `jitong_android/app/src/androidTest/` 对应测试目录。
- [ ] 创建 `outputs/kernel-baseline/` 保存非敏感测试结果。
- [ ] 编写 `BASELINE.md`，声明测试账号、环境、服务端版本和数据清理方法。

验收：

- 测试目录可被 CMake/Gradle 发现。
- 不提交密码、Token、私钥、真实手机号。

## P0-T02 固化登录基线

参考文件：

- `jitong_android/app/src/main/java/com/jitong/im/net/ImClient.kt`
- `jitong_android/app/src/main/java/com/jitong/im/net/SecureChannel.kt`
- `jitong_android/app/src/main/java/com/jitong/im/ui/AuthCoordinator.kt`
- `jitong_android/app/src/main/java/com/jitong/im/ui/MainViewModel.kt`

任务：

- [ ] 记录密码登录请求/响应字段，不记录字段值。
- [ ] 记录 Token Login 分支条件。
- [ ] 记录 Refresh 分支、Single-Flight 和 requestId 复用行为。
- [ ] 覆盖密码错误、Token 过期、Refresh 过期、被踢下线。
- [ ] 覆盖连续点击登录只产生一个认证任务。

验收：

- 每种场景有输入、状态变化、协议类型和最终结果。
- 可以据此判断 Native 行为是否与 Android 一致。

## P0-T03 固化消息和漫游基线

- [ ] 记录发送消息从 `SENDING → SENT/FAILED` 的状态变化。
- [ ] 记录 `msg_id`、`conversation_seq` 和时间字段变化。
- [ ] 覆盖双方同时发送。
- [ ] 覆盖重复包、断线重发、离线消息、历史分页和 seq 缺口。
- [ ] 导出不含敏感正文的结构化 Golden Case。

验收：同一组输入可以在 Kotlin Legacy 和 Native Kernel 上重复执行。

## P0-T04 固化搜索基线

参考文件：

- `data/ChatStore.kt`
- `data/db/Daos.kt`
- `util/PinyinIndex.kt`
- `ui/SearchHighlight.kt`

- [ ] 建立中文、全拼、首字母、拼音前缀、LIKE 子串用例。
- [ ] 加入“你/ni/n”“正/zheng/z”等容易混淆的案例。
- [ ] 保存期望命中 msgId 和汉字高亮区间。

验收：至少 50 个固定搜索用例全部可重复执行。

## P0-T05 固化媒体基线

参考文件：

- `net/HttpMediaClient.kt`
- `util/ImageCodec.kt`
- `ui/MainViewModel.kt`
- `ui/ChatScreen.kt`

- [ ] 覆盖普通图、横长图、竖长图。
- [ ] 覆盖 JPEG、PNG、HEIC、Display P3、EXIF 旋转。
- [ ] 记录三档资源尺寸、摘要、Content-Type 和显示优先级。
- [ ] 覆盖秒传命中、未命中、PoP 失败、下载 404。
- [ ] 保存测试图片的像素摘要或感知哈希，验证颜色没有倒退。

### P0 门禁

- [ ] P0-T01～P0-T05 全部完成。
- [ ] Golden 数据不包含秘密。
- [ ] 当前 Android Debug 构建通过。

---

# P1：打通 Android Native 构建

## P1-T01 CMake 组件化

修改：

- `client_core/CMakeLists.txt`
- `protocol/CMakeLists.txt`

任务：

- [ ] 增加 `CLIENT_CORE_WITH_SQLITE`。
- [ ] 增加 `CLIENT_CORE_WITH_MEDIA`。
- [ ] Android 构建关闭 tests/tools。
- [ ] 宿主机 `protoc` 与 Android 目标 `libprotobuf` 分离。
- [ ] 所有进入共享库的静态库启用 PIC。
- [ ] 保持桌面端原构建路径可用。

验收命令：

```bash
cmake -S client_core -B build/client-core-desktop
cmake --build build/client-core-desktop
ctest --test-dir build/client-core-desktop --output-on-failure
```

## P1-T02 Android externalNativeBuild

修改：

- `jitong_android/app/build.gradle.kts`
- 新建 `jitong_android/app/src/main/cpp/CMakeLists.txt`

任务：

- [ ] 配置 NDK/CMake。
- [ ] 配置 `arm64-v8a` 和 `x86_64`。
- [ ] 接入 OpenSSL Android 目标库。
- [ ] 接入 protobuf C++ Android 运行库。
- [ ] 首阶段关闭 SQLite 和 Media。

验收：两个 ABI 都生成 `libjitong_kernel.so`。

## P1-T03 Native Self Test

新建：

- `im_core_jni.cpp`
- `NativeBindings.kt`

实现：

```text
nativeVersion()
nativeSelfTest()
```

验收：真机和 x86_64 模拟器均返回相同核心版本和 protobuf/OpenSSL 自检结果。

### P1 门禁

- [ ] Desktop C++ 构建没有回归。
- [ ] Android 两个 ABI 构建通过。
- [ ] `.so` 可加载并完成 Self Test。

---

# P2：JNI 生命周期与平台接口

## P2-T01 NativeSdkHandle

新建：

- `native_sdk_handle.h/.cpp`

任务：

- [ ] `jlong` 指向 `NativeSdkHandle`，禁止指向裸 `ClientCore*`。
- [ ] 每次 API 调用在锁内取得 `shared_ptr<JitongSdk>`。
- [ ] `destroying` 后拒绝新调用。
- [ ] `nativeDestroy` 幂等。

## P2-T02 JniObserver

新建：

- `jni_observer.h/.cpp`
- `NativeEventSink.kt`

任务：

- [ ] 缓存 `JavaVM*`、类和 method ID。
- [ ] 正确 New/DeleteGlobalRef。
- [ ] 只在线程由 JNI 临时 Attach 时 Detach。
- [ ] 每次 CallVoidMethod 后检查并清理异常。
- [ ] 回调只入队，不做业务。

## P2-T03 平台原子能力接口

新建：

- `PlatformServices.h`
- `AndroidKeystoreProvider.kt`
- `AndroidFileProvider.kt`
- `AndroidImagePlatform.kt`
- `AndroidSecureKv.kt`
- `AndroidNetworkObserver.kt`

任务：

- [ ] 定义异步签名接口。
- [ ] 定义 URI/FileHandle 读取接口。
- [ ] 定义图片 probe/decode/encode 接口。
- [ ] 定义 opaque secure KV 接口。
- [ ] 所有接口禁止包含 Token、消息、秒传等业务语义。

## P2-T04 生命周期压力测试

- [ ] 创建/销毁 100 次。
- [ ] 回调并发销毁。
- [ ] 后台线程回调 Attach/Detach。
- [ ] 开启 CheckJNI。
- [ ] 使用 ASan 构建跑 UAF 检查。

### P2 门禁

- [ ] 无 UAF、double free、GlobalRef 泄漏。
- [ ] 所有异常转换成 SDK 错误，不能穿透 JNI。

---

# P3：Transport 和消息边界

## P3-T01 重构 TcpTransport 生命周期

修改：

- `client_core/src/TcpTransport.h`
- `client_core/src/TcpTransport.cpp`

任务：

- [ ] 每次 connect 创建新的 socket/TLS stream。
- [ ] 重连前 `io_context.restart()`。
- [ ] 重置 `m_notified/m_closing/writeQueue`。
- [ ] 明确 connect/close 状态机。
- [ ] 禁止工作线程 join 自身。
- [ ] 所有异步 handler 绑定安全生命周期 token。

## P3-T02 帧编解码独立模块

新建：

- `transport/FrameCodec.h/.cpp`

任务：

- [ ] 4B 大端包长编码解码。
- [ ] 4B 小端协议号编码解码。
- [ ] `0 < bodyLen <= MAX_PACK_LEN`。
- [ ] 精确读取 Header 后再精确读取 Body。
- [ ] 非法长度直接关闭连接。
- [ ] 未知协议号可记录并忽略，越界不能访问函数表。

## P3-T03 单写发送队列

- [ ] 所有发送都 post 到 asio executor。
- [ ] 同一时刻只有一个 `async_write`。
- [ ] shutdown 明确处理队列中的未发送请求。
- [ ] 业务回调返回 `CANCELLED/DISCONNECTED`。

## P3-T04 Transport 测试

- [ ] 半包、粘包、1 字节分段。
- [ ] 0 长度、超大长度、错误端序。
- [ ] 连续重连 50 次。
- [ ] 服务端中途关闭。
- [ ] 发送过程中 close。

### P3 门禁

- [ ] 无消息边界错位。
- [ ] 错误包长后可安全断线并新建会话。
- [ ] 重连 50 次无资源增长。

---

# P4：TLS 与应用层安全通道

## P4-T01 TLS 客户端配置

- [ ] TLS 最低版本 1.3。
- [ ] 验证证书链和域名。
- [ ] 设置正确 SNI。
- [ ] 实现 SPKI Pinning。
- [ ] 禁止 `verify_none` 和永久返回 true 的 callback。

## P4-T02 ClientSecureChannel

新建：

- `transport/ClientSecureChannel.h/.cpp`

对齐 `SecureChannel.kt`：

- [ ] X25519 临时密钥。
- [ ] Ed25519 服务端身份签名验证。
- [ ] transcript hash。
- [ ] HKDF-SHA256 派生双向密钥、nonce 和 Finished key。
- [ ] AES-256-GCM。
- [ ] sessionId/sequence/protocol 进入 AAD。
- [ ] 收发 sequence 分离且单调递增。
- [ ] 中间密钥安全清除。

## P4-T03 安全负向测试

- [ ] 错误证书。
- [ ] 错误 SPKI。
- [ ] 错误服务端签名。
- [ ] 修改密文/tag/AAD。
- [ ] 重放 sequence。
- [ ] Finished 不一致。

### P4 门禁

- [ ] 与 Android Golden 握手字段和方向完全一致。
- [ ] 抓包不可看到业务明文。
- [ ] 任意认证失败都不会继续解析业务包。

---

# P5：AccountSession、Token 与防重入

## P5-T01 AuthStateMachine

新建：

- `account/AuthStateMachine.h/.cpp`
- `account/AccountSession.h/.cpp`

状态：

```text
Stopped → Connecting → Handshaking → Authenticating → Authenticated
                    ↘ Backoff → Reconnecting
```

任务：

- [ ] 相同登录请求复用 RequestId。
- [ ] 不同账号并发登录返回 `AUTH_OPERATION_IN_PROGRESS`。
- [ ] UI 按钮禁用不作为正确性保证。
- [ ] 被踢后禁止自动重连。

## P5-T02 DeviceProofService

- [ ] C++ 构造 canonical proof。
- [ ] requestId 关联异步签名结果。
- [ ] 超时/取消后丢弃迟到签名。
- [ ] 平台层只执行 P-256 sign。

## P5-T03 TokenManager

新建：

- `account/TokenManager.h/.cpp`
- `account/TokenStore.h/.cpp`

任务：

- [ ] C++ 解析和保存完整 TokenSession。
- [ ] 启动时自动判断 Access/Refresh 有效期。
- [ ] 实现 Refresh Single-Flight。
- [ ] 持久化 pending requestId。
- [ ] 刷新成功原子轮换 Token。
- [ ] Token reuse 清理 Token family。
- [ ] 等待 Token 的 Socket/HTTP 请求统一恢复或失败。

## P5-T04 自动登录入口

UI 只调用 `sdk.start()`：

- [ ] 无凭证上报 `NeedLogin`。
- [ ] Access 有效自动 Token Login。
- [ ] Access 临期、Refresh 有效自动刷新后登录。
- [ ] Refresh 过期清理并上报 `NeedLogin`。

## P5-T05 认证测试

- [ ] 密码登录成功/失败。
- [ ] Token 登录成功/过期。
- [ ] Refresh 成功/过期/重用。
- [ ] 10 个并发请求触发一次刷新。
- [ ] 刷新途中断线，重连沿用 requestId。
- [ ] 连点登录只发一条认证链路。

### P5 门禁

- [ ] Kotlin UI/ViewModel 不包含 Token 分支和刷新逻辑。
- [ ] Native 自动登录行为通过 Android Golden 对照。

---

# P6：C++ SQLCipher 数据层

## P6-T01 SQLCipher Android 构建

- [ ] 将 SQLCipher C/C++ 库接入 Native 构建。
- [ ] 验证普通 SQLite 无法读取加密数据库。
- [ ] 每账号独立数据库路径。
- [ ] DB key 由 C++ 管理并通过平台安全存储包装。

## P6-T02 Schema 与 Migration

新建：

- `storage/Schema.sql` 或版本化 migration 文件。

表：

- [ ] messages
- [ ] conversations
- [ ] friends
- [ ] message_fts
- [ ] sync_watermarks
- [ ] outbox
- [ ] media_records
- [ ] transfer_tasks

关键约束：

- [ ] `UNIQUE(owner_id,msg_id)`。
- [ ] `(owner_id,peer_id,conversation_seq)`。
- [ ] `(owner_id,peer_id,server_time)`。
- [ ] `(owner_id,status,local_order)`。

## P6-T03 单写线程和只读连接池

- [ ] `DbCommandQueue`。
- [ ] 单个 Writer connection/thread。
- [ ] 查询使用只读连接。
- [ ] 写事务完成后触发 Query invalidation。
- [ ] 网络线程只投递命令，不同步等待磁盘。

## P6-T04 Room 数据迁移器

- [ ] Android 只读分页导出旧数据。
- [ ] C++ 按 msg_id 幂等导入。
- [ ] 重建 FTS。
- [ ] 比对消息数、会话数、max_seq 和抽样摘要。
- [ ] 原子写 `migration_completed`。
- [ ] 中断后可重新执行。

### P6 门禁

- [ ] 数据库加密验证通过。
- [ ] 迁移可中断恢复。
- [ ] 网络线程不执行 SQL。
- [ ] 旧 Room 数据数量与 Native 数据一致。

---

# P7：消息 Outbox / Inbox

## P7-T01 完整消息模型

- [ ] 定义 `MessageId/ConversationId/ConversationSeq` 强类型。
- [ ] 保留 sender/receiver/peer/type/content/time/status。
- [ ] 媒体元数据进入统一 Message 模型。
- [ ] JNI 只输出稳定 DTO/protobuf。

## P7-T02 Outbox

- [ ] 内核生成 msg_id。
- [ ] 先持久化 SENDING。
- [ ] 认证可用后发送。
- [ ] 超时重试使用同一 msg_id。
- [ ] ACK 回填同一行的 seq/time/status。
- [ ] kill 进程后恢复未完成任务。

## P7-T03 Inbox

- [ ] 实时、离线、漫游共用一个入口。
- [ ] `owner_id + msg_id` 幂等。
- [ ] 单事务更新消息、会话和搜索索引。
- [ ] 重复消息不重复通知 UI。

## P7-T04 Query Subscription

- [ ] `observeConversations()`。
- [ ] `observeMessages(conversationId)`。
- [ ] 增量失效通知。
- [ ] JNI/Kotlin 订阅销毁后停止回调。

### P7 门禁

- [ ] 双方同时发送最终 seq 顺序一致。
- [ ] 重发和重复包不产生第二条消息。
- [ ] UI 消息列表只来自 Native Query。

---

# P8：漫游、离线和补洞

## P8-T01 SyncWatermark

- [ ] 持久化每会话 local_max_seq。
- [ ] 保存同步状态和分页 cursor。
- [ ] 重启后恢复未完成同步。

## P8-T02 会话摘要同步

- [ ] 认证成功后自动拉取。
- [ ] 与本地水位比较。
- [ ] 发现缺口创建补洞任务。

## P8-T03 历史分页

- [ ] UI 上报滚动到顶部，不传 beforeSeq。
- [ ] SyncEngine 从数据库读取当前 minSeq。
- [ ] 处理 hasMore/minSeq。
- [ ] 防止相同分页并发。

## P8-T04 补洞测试

- [ ] 缺 1 条、连续多条、页边界缺失。
- [ ] 同步期间收到实时消息。
- [ ] 同步期间断网和进程退出。
- [ ] 服务端重复返回同一页。

### P8 门禁

- [ ] 实时/离线/漫游不重不漏。
- [ ] UI 不计算 beforeSeq 和 seq 缺口。

---

# P9：C++ FTS4、拼音搜索和高亮

## P9-T01 选择并封装 C++ 拼音实现

- [ ] 评估体积、许可证、多音字、Android/桌面支持。
- [ ] 封装为 `IPinyinConverter`，禁止业务依赖第三方 API。
- [ ] 用 P0 Golden Case 验证与 Android 当前结果兼容。

决策门槛：若没有可兼容库，先将当前拼音映射表生成为 C++ 只读数据，不允许改变搜索语义后直接切换。

## P9-T02 入库索引

- [ ] 正文生成 normalized/pinyin/initials。
- [ ] messages 与 FTS4 同事务写入。
- [ ] 历史消息可分批重建。

## P9-T03 查询引擎

- [ ] 中文/普通文本查询。
- [ ] 拼音前缀查询。
- [ ] 首字母查询。
- [ ] LIKE 子串补充。
- [ ] msg_id 去重和游标分页。

## P9-T04 高亮映射

- [ ] 建立拼音 token 到汉字下标映射。
- [ ] 返回 UTF-16 或 Unicode code point 明确语义的区间。
- [ ] Kotlin 只按区间加粗。

### P9 门禁

- [ ] P0 的至少 50 个 Golden 搜索用例全部通过。
- [ ] “n”不会错误匹配“正/真”，但能按产品规则匹配“你”。

---

# P10：Media SDK 骨架与图片流水线

## P10-T01 Media 公共 API

- [ ] `sendImage/sendFile/openImage/cancelTransfer/setVisibleMessages`。
- [ ] `TransferState/ImageDisplayState`。
- [ ] UI API 不暴露缩略图等级选择和 HTTP URL。

## P10-T02 Android File/Image 平台适配

- [ ] Content URI → FileHandle/fd。
- [ ] probe 宽高、EXIF、色彩空间。
- [ ] decode 到明确像素格式。
- [ ] 转换到 sRGB。
- [ ] AVIF 编码。
- [ ] 临时文件和原子 rename。

## P10-T03 ThumbnailPipeline

- [ ] 普通图片完整等比缩放。
- [ ] 横向超长图居中裁剪缩略图。
- [ ] 纵向超长图展示顶部首屏。
- [ ] 原图不裁剪。
- [ ] 生成小图、大图、原图三档元数据。
- [ ] 原始宽高进入 MediaCard。

## P10-T04 色彩与像素测试

- [ ] Display P3 → sRGB。
- [ ] HEIC/HDR 输入。
- [ ] EXIF 90/180/270。
- [ ] 对比 Android 基线像素或感知哈希。

### P10 门禁

- [ ] 不再出现粉色/绿色滤镜。
- [ ] 长图缩略图符合基线。
- [ ] UI 无图片处理决策。

---

# P11：秒传、分片、下载和取消

## P11-T01 StreamingHash

- [ ] 64 KiB 或配置化缓冲流式 SHA-256。
- [ ] 支持进度和取消。
- [ ] 不把大文件整体加载内存。

## P11-T02 InstantUploadClient / PoP

- [ ] 提交 sha256/size/contentType/receiverId。
- [ ] 校验 challenge offset/length 不越界。
- [ ] 从同一文件身份读取指定片段。
- [ ] `SHA-256(nonce || chunk)`。
- [ ] 服务端授权完成后再返回成功。
- [ ] 文件在计算期间改变时拒绝秒传。

## P11-T03 ChunkUploader

- [ ] upload_id 与 checkpoint。
- [ ] chunk hash。
- [ ] 并发窗口和带宽限制。
- [ ] 指数退避。
- [ ] 网络恢复续传。
- [ ] 完整摘要校验。

## P11-T04 RangeDownloader

- [ ] `.download` 临时文件。
- [ ] HTTP Range。
- [ ] ETag/文件摘要防止错误续传。
- [ ] 完成后摘要校验和原子 rename。
- [ ] 失败删除或保留可恢复 checkpoint。

## P11-T05 任务生命周期

- [ ] TransferTask 由 `shared_ptr` 管理。
- [ ] 回调捕获 `weak_ptr`。
- [ ] 回主执行器前 `lock()`。
- [ ] cancel 后不再上报 Completed。
- [ ] SDK destroy 等待任务安全停止。

### P11 门禁

- [ ] 秒传命中不上传整文件。
- [ ] 1 GiB 测试文件内存占用保持有界。
- [ ] 中断续传成功。
- [ ] 取消/销毁压力测试无 UAF。

---

# P12：媒体消息、缓存和分级加载

## P12-T01 MediaRecord

- [ ] 保存 original/small/large 的 fileId、sha256、path 和状态。
- [ ] 消息数据库与媒体记录保持事务关联。
- [ ] 服务端 404 区分失效索引和临时网络错误。

## P12-T02 MediaCard

- [ ] 上传成功和授权完成后发送 MediaCard。
- [ ] 携带原始宽高、三档索引、Content-Type、size、sha256。
- [ ] 接收、离线和漫游使用同一个解析入口。

## P12-T03 DownloadScheduler

- [ ] `file_id + variant` 单飞。
- [ ] P0～P4 优先级队列。
- [ ] UI 可见消息只作为输入信号。
- [ ] 点击原图抢占低优先级后台预取。

## P12-T04 ImageDisplayState

- [ ] OriginalReady。
- [ ] LargeThumbnailReady + 原图下载中。
- [ ] SmallThumbnailReady + 升级中。
- [ ] Placeholder(width,height)。
- [ ] Failed(retryable)。

### P12 门禁

- [ ] UI 只渲染当前最佳资源。
- [ ] 图片升级时气泡尺寸不变化。
- [ ] 点击优先显示本地最高质量并请求原图。

---

# P13：Android UI 切换

## P13-T01 KernelBackend

新建：

- `KernelBackend.kt`
- `JitongSdk.kt`

- [ ] 启动时确定 Legacy/Native。
- [ ] 进程内不可修改。
- [ ] 确保只创建一个连接核心。

## P13-T02 LoginScreen/MainViewModel

- [ ] 登录页只调用 `sdk.login()`。
- [ ] 启动只调用 `sdk.start()`。
- [ ] 删除 Native 路径中的 Token 判断、refresh 和 reconnect 编排。
- [ ] UI 只映射 `AccountState/ConnectionState`。

## P13-T03 Conversation/Chat

- [ ] 会话和消息只订阅 Native Query。
- [ ] sendText/sendImage/sendFile 只调用 SDK。
- [ ] UI 不直接写 Room。
- [ ] UI 不解析 protobuf。

## P13-T04 Search

- [ ] 查询交给 SDK。
- [ ] 直接使用内核返回 highlightRanges。
- [ ] UI 只绘制加粗区间。

## P13-T05 Image UI

- [ ] 上报 visible message IDs。
- [ ] 点击调用 `sdk.openImage(msgId)`。
- [ ] 根据 ImageDisplayState 渲染。
- [ ] 保持 width/height 等比占位。

### P13 门禁

- [ ] Native 模式下 ViewModel 不含业务状态机。
- [ ] P0 所有 Android Golden Case 通过。

---

# P14：灰度、性能和清理

## P14-T01 指标

- [ ] 登录成功率和时延。
- [ ] Token 刷新成功率。
- [ ] 消息发送成功率、重复率、缺失率。
- [ ] 重连次数和恢复时延。
- [ ] 数据库写入/查询耗时。
- [ ] 搜索耗时。
- [ ] 秒传命中率和节省字节。
- [ ] 缩略图/原图下载流量。
- [ ] Native 崩溃、ANR、内存和线程数。

## P14-T02 灰度顺序

- [ ] 开发者开关。
- [ ] 内部测试账号。
- [ ] 小比例设备。
- [ ] 扩大比例。
- [ ] Native 默认开启。

任一核心指标显著劣化时停止扩大并回退到 Legacy 重启。

## P14-T03 删除旧实现

只有所有门禁通过后才允许：

- [ ] 删除 Kotlin `ImClient` 协议实现。
- [ ] 删除 Kotlin `SecureChannel/AppCrypto` 业务路径。
- [ ] 删除 ViewModel Token 刷新代码。
- [ ] 删除 ViewModel 媒体上传下载编排。
- [ ] 将旧 Room 数据库改为只迁移工具或移除。
- [ ] 更新 README、架构图和答辩材料。

### P14 最终门禁

- [ ] 所有 P0 Golden Case 通过。
- [ ] Native 核心至少完成一轮真实灰度。
- [ ] 无已知 P0/P1 崩溃、UAF、丢消息和数据损坏问题。
- [ ] Android 与桌面端实际复用同一 C++ 业务模块。

---

## 19. 每次提交建议

建议按任务提交，不要按阶段积累成超大提交：

```text
build(native): add Android core shared library
feat(jni): add safe NativeSdkHandle lifecycle
refactor(transport): make TLS transport reconnectable
feat(security): add client application secure channel
feat(auth): move token lifecycle into native AccountSession
feat(storage): add SQLCipher message store and DB writer
feat(message): add persistent outbox and idempotent inbox
feat(sync): add roam pagination and seq gap recovery
feat(search): add native FTS4 pinyin search and highlights
feat(media): add thumbnail pipeline and Android image adapter
feat(transfer): add PoP instant upload and resumable chunks
feat(android): switch UI to JitongSdk native backend
chore(legacy): remove duplicated Kotlin protocol stack
```

## 20. 执行前必须确认的技术决策

这些决策必须在对应阶段开始前形成 ADR，不能边写边猜：

- [ ] ADR-001：C++ protobuf Android runtime 的来源和版本策略。
- [ ] ADR-002：OpenSSL/SQLCipher/libavif 的构建与升级策略。
- [ ] ADR-003：C++ 拼音库选择及许可证。
- [ ] ADR-004：数据库密钥生成、包装、恢复和登出销毁。
- [ ] ADR-005：JNI DTO 使用 protobuf envelope 还是稳定手写 DTO。
- [ ] ADR-006：媒体文件通过临时路径还是 fd 交给 C++。
- [ ] ADR-007：Token SecureString 和内存清理边界。
- [ ] ADR-008：Legacy Room 到 Native SQLCipher 的迁移和回退策略。
- [ ] ADR-009：后台保活、Android 网络变化与系统限制的边界。
- [ ] ADR-010：缩略图规格、长图阈值和 AVIF 质量参数。

## 21. 第一轮建议执行范围

第一轮只执行到 P2，不立即迁移真实登录：

```text
P0 Android基线
→ P1 Native构建
→ P2 JNI生命周期和平台接口
```

第一轮交付物：

- Android 行为 Golden Case；
- 两个 ABI 的 `libjitong_kernel.so`；
- 可安全创建销毁的 Native SDK；
- 平台原子能力接口；
- CheckJNI/ASan 验证结果；
- 不影响当前 Kotlin 客户端的关闭状态 Native 开关。

完成上述范围后，再开始 P3/P4 网络与安全通道迁移。

## 22. 第二轮工作与验收范围：P3 Transport + P4 安全通道

> 第二轮目标不是迁移登录、Token、数据库、消息或媒体业务，而是先建立一个可重连、
> 边界明确、默认安全的 C++ 网络底座。完成后，旧 `test_e2e` 必须能越过当前
> `WAIT_CLIENT_HELLO` 阻断，证明 C++ ClientCore 与服务端完成同一套安全握手。

### 22.1 起点与范围边界

#### 22.1.1 已具备的前置条件

- Android `libjitong_kernel.so` 已能构建、加载、创建和安全销毁。
- Native 事件桥、句柄表、CheckJNI、宿主机 ASan/UBSan 和 Android HWASan 已通过。
- 服务端已经强制执行 TLS 后的应用层安全握手，并坚持 fail-close。
- `security_e2e` 已证明：TLS 成功后直接发送 Login(1000)，服务端会因等待
  AppClientHello(1036) 而断开，业务明文不会进入 AuthHandler。
- `im.proto` 已定义 AppClientHello、AppServerHello、AppFinished 和
  AppEncryptedFrame；服务端已有可对照的 `AppCrypto` 与 `Session` 实现。

#### 22.1.2 本轮纳入

1. P3：Transport 生命周期、FrameCodec、单写队列和消息边界测试。
2. P4：TLS 1.3、证书/域名/SNI/SPKI、C++ ClientSecureChannel 和安全负向测试。
3. 恢复服务端完整 `test_e2e`，至少跑通注册、登录、在线/离线文本、互踢、漫游、
   HTTP 图片上传/授权下载。
4. Android arm64 接入同一 Transport/SecureChannel，完成最小连接和握手验证。
5. 普通测试、CheckJNI、ASan/UBSan、Android HWASan 回归。

#### 22.1.3 本轮不纳入

- P5 登录状态机、Token 自动刷新与登录 Single-Flight。
- P6 C++ SQLCipher 数据库迁移。
- P7/P8 Outbox、消息落库、漫游补洞和多设备同步内核化。
- P9 搜索迁入 C++。
- P10～P12 图片缩略图、秒传、分片和下载调度内核化。
- x86_64 模拟器实跑（按本轮决定延期；仍要求 x86_64 编译通过）。
- AVIF 偏色修复不属于 P3/P4，但 Media Golden 必须继续保留失败记录，不能删除或放宽。

### 22.2 P3 工作与验收：Transport 和消息边界

#### 22.2.1 P3-T01 TcpTransport 生命周期

实现要求：

- 每次 `connect()` 创建新的 resolver、socket 和 TLS stream，不复用已关闭对象。
- 重连前调用 `io_context.restart()`，并清空上一个连接的解析、读写和计时状态。
- 明确状态机：`Idle → Resolving → Connecting → TlsHandshaking → Connected → Closing → Closed`。
- `connect()`、`close()`、远端断开和握手失败最终只能产生一次关闭通知。
- 使用 connection generation/lifetime token；旧连接迟到 handler 不得操作新连接。
- 禁止 IO 工作线程 join 自身；析构必须有确定的停止和 join 顺序。
- 连接失败后可以在同一个 `TcpTransport` 实例上安全重试。

验收用例：

- DNS/连接失败后重连成功。
- TLS 握手中主动关闭。
- 服务端在 Header、Body 和写入过程中分别关闭。
- 连续连接—断开 50 次，线程数、句柄数和回调次数不持续增长。
- 旧连接回调迟到时不污染新连接状态。

#### 22.2.2 P3-T02 FrameCodec

线格式必须保持现有兼容约定：

```text
[4B 大端 body_len = 4 + payload_size]
[4B 小端 protocol_type]
[protobuf payload]
```

实现要求：

- 从 `TcpTransport` 中抽出唯一的 `FrameCodec`，禁止测试和生产各复制一套端序逻辑。
- Header 固定精确读取 4 字节；解析并校验 body_len 后，再精确读取 Body。
- 合法范围为 `4 <= body_len <= 10 MiB`；body_len 小于协议号或超过上限立即 fail-close。
- payload 长度使用安全减法，所有长度转换先检查再转换，禁止整数溢出和超大分配。
- protocol_type 解码为小端 `uint32_t`；分发前检查范围，未知号记录后忽略或上报，
  绝不能直接越界访问函数表。
- 包长损坏后不尝试在密文字节流中扫描“下一个包头”；关闭本次连接，由上层建立新会话。

验收矩阵：

| 场景 | 预期 |
|---|---|
| Header 每次只到 1 字节 | 聚合到 4 字节后才解析 |
| Body 每次只到 1～7 字节 | 精确聚合到 body_len 后只回调一次 |
| 两帧一次到达（粘包） | 连续解析两帧，不串包 |
| Header 与半个 Body 同时到达 | 保存已有字节并补齐剩余部分 |
| body_len=0/1/2/3 | 立即断线，不分配 Body |
| body_len=10 MiB | 按边界规则接受 |
| body_len>10 MiB | 立即断线 |
| 把包长误写为小端 | 按非法超长处理并断线 |
| protocol_type 未注册 | 不访问越界函数指针 |
| 截断 protobuf | 解析失败只丢弃本包或关闭连接，不崩溃 |

#### 22.2.3 P3-T03 单写发送队列

实现要求：

- 所有发送统一 `post` 到同一个 Asio executor/strand。
- 队列从空变为非空时只启动一次 `async_write`，完成后再发送下一项。
- 同一 TLS stream 任意时刻最多一个写操作，不允许多线程直接并发 `async_write`。
- 每个待发送项拥有完整不可变 buffer，异步完成前内存保持有效。
- close 后拒绝新发送；队列中未开始/未完成项统一返回 `CANCELLED` 或 `DISCONNECTED`。
- 写失败触发一次连接关闭，剩余队列逐项完成失败，不能永远悬挂。

验收用例：

- 8 个线程并发提交至少 10,000 个小帧，服务端解帧数量、内容和边界全部正确。
- 大帧写入过程中追加小帧，顺序与入队顺序一致。
- 写入过程中 close，所有请求都有且仅有一次结果。
- ThreadSanitizer 可运行的平台执行发送队列并发测试；不可运行时至少使用压力测试 + ASan/UBSan。

#### 22.2.4 P3-T04 P3 门禁

- [ ] `FrameCodec` 单元测试覆盖上述全部边界。
- [ ] 真实 loopback TLS 测试覆盖半包、粘包、1 字节分段和中途断开。
- [ ] 连续重连 50 次成功，关闭通知不重复，无线程/句柄持续增长。
- [ ] 单写队列并发压力通过，无交叉写导致的 TLS record/业务帧损坏。
- [ ] 错误包长后安全断线，并可建立全新连接恢复。
- [ ] `client_core` 普通 CTest 与 ASan/UBSan 全绿。
- [ ] Android arm64 Native 生命周期、CheckJNI、HWASan 回归不退化。

### 22.3 P4 工作与验收：TLS 与应用层安全通道

#### 22.3.1 P4-T01 TLS 客户端安全配置

实现要求：

- TLS 最低和最高策略明确；本项目验收要求协商结果为 TLS 1.3。
- 必须验证证书链、有效期和主机名，SNI 使用逻辑服务域名 `im.example.com`，
  即使 TCP 实际连接 `127.0.0.1` 也不能用 IP 替代身份名。
- 在正常证书验证之后执行 SPKI SHA-256 Pinning；证书轮换至少支持 current/next 两个 pin。
- 禁止 `verify_none`、永久返回 true 的 verify callback，以及仅在日志中提示但继续连接。
- CA 文件缺失、证书错误、域名错误和 pin 错误必须在进入应用握手前失败。

负向用例：

- 自签但不受信证书、过期证书、错误 SAN、错误 SNI、错误 SPKI、空 pin 集。
- 证书链正确且 current pin 命中；证书轮换时 next pin 命中。
- 验证失败后不得发送 AppClientHello 或任何业务帧。

#### 22.3.2 P4-T02 ClientSecureChannel 四步握手

必须对齐 Android `SecureChannel.kt` 和服务端 `Session.cpp` 的字段、字节序及 transcript：

```text
C++ Client                         Server Session
    |-- AppClientHello ----------->| version / X25519 pub / nonce / randomId / suite
    |<-- AppServerHello ------------| server pub / nonce / sessionId / keyId / Ed25519 signature
    |-- AppFinished --------------->| client verify_data
    |<-- AppFinished ---------------| server verify_data
    |==== AppEncryptedFrame ========| 双向业务密文，sequence 分方向递增
```

实现要求：

- 每次连接生成新的 X25519 临时密钥、32 字节 client_nonce 和 16 字节 random_id。
- 严格校验 version、cipher_suite、各字段长度和服务端 key_id。
- 使用内置受信 Ed25519 公钥验证 AppServerHello 签名；未知 key_id fail-close。
- transcript 的 label、长度前缀、字段顺序和大端整数与服务端完全一致。
- X25519 共享秘密全零必须拒绝。
- HKDF-SHA256 一次派生 C→S/S→C key、nonce prefix、Finished key，方向不能混用。
- Finished 使用 transcript hash 计算并常量时间比较；验证完成前禁止业务发送。
- AES-256-GCM 的 AAD 必须绑定 version、session_id、sequence；协议号放进密文正文。
- 收发 sequence 分离，从 1 开始严格 `expected+1`；重复、跳号、回退和溢出均断线。
- tag 固定 16 字节；认证失败时不得把任何明文交给业务分发。
- 私钥、共享秘密、派生密钥和临时明文在离开作用域时安全清零。
- 重连必须生成新 session_id/密钥/nonce，旧会话密文在新连接上不可重放。

#### 22.3.3 P4-T03 安全负向测试

| 攻击/故障 | 预期 |
|---|---|
| ServerHello 签名翻转 1 bit | 握手失败，无业务包 |
| server nonce/pubkey/sessionId 长度错误 | 握手失败 |
| Finished verify_data 错误 | 握手失败 |
| 密文、tag、AAD 任意翻转 1 bit | GCM 认证失败并断线 |
| 重放上一帧 sequence | 拒绝重放并断线 |
| sequence 跳号或回退 | 拒绝并断线 |
| 把 S→C 密文送入 C→S 方向 | 因方向密钥不同认证失败 |
| 旧连接密文注入新连接 | 因 sessionId/密钥不同失败 |
| 安全通道建立后发送明文 Login | 服务端 fail-close（现有 security_e2e） |
| 握手超时 | 15 秒内关闭，释放全部状态 |

#### 22.3.4 Android/C++ 一致性 Golden

使用固定的测试私钥、nonce、random_id 和 ServerHello，Android 与 C++ 必须输出完全一致的：

- signing transcript 字节及 SHA-256；
- X25519 shared secret；
- HKDF 派生的双向 key、nonce prefix、Finished key；
- client/server Finished verify_data；
- sequence=1 的 AAD、nonce、ciphertext 和 tag。

Golden 文件中只放测试向量，禁止写入生产私钥、Token、手机号和真实 sessionId。

#### 22.3.5 P4 门禁

- [ ] TLS 1.3、证书链、域名、SNI 和双 pin 全部通过；所有负向证书用例 fail-close。
- [ ] Android/C++/Server 三端安全向量逐字节一致。
- [ ] P4 全部篡改、重放、方向混淆和握手超时用例通过。
- [ ] 抓取 loopback 流量，只能看到 TLS records，测试关键词和 protobuf 业务正文不可见。
- [ ] `security_e2e` 继续通过，证明服务端没有为了兼容 C++ 而允许降级。
- [ ] 删除旧 `test_e2e` 的 `DISABLED`，完整业务 e2e 通过。
- [ ] 完整业务 e2e 在 ASan+UBSan 下通过。
- [ ] Android arm64 最小流程达到：TLS → 应用握手完成 → 加密 Heartbeat/登录请求 → 正常解密响应。
- [ ] CheckJNI 与 HWASan 无新增错误。

### 22.4 完整业务 E2E 验收清单

恢复后的 `test_e2e` 至少必须覆盖：

1. TLS 1.3 + 应用层四步握手成功。
2. 注册成功，重复昵称/手机号返回确定错误。
3. 密码正确登录成功、错误密码失败，登录请求与响应均在 AppEncryptedFrame 内。
4. 两用户在线 C2C 文本转发，发送方 ACK、接收方消息内容正确。
5. 接收方离线时服务端转存，重连登录后补发。
6. 同账号新连接登录后旧连接收到 Kicked，旧连接停止发送业务。
7. 心跳至少持续三个周期，连接保持；心跳超时能关闭。
8. 漫游会话摘要、历史分页、beforeSeq/minSeq/hasMore 正确且无重复。
9. HTTP 图片上传成功，图片卡片只携带元数据和授权 fileId。
10. 消息参与者可下载且 SHA-256 一致，第三方持有 fileId 仍返回未授权。
11. 服务停止后所有 ClientCore/Session/HTTP worker 可确定退出，临时数据库和文件可清理。

测试隔离要求：

- 数据库、证书、身份密钥和上传目录只能使用测试生成物或 `/tmp` 临时目录。
- 禁止访问仓库中的 `data/im.db`、真实 uploads 和生产配置。
- 端口冲突不得被误报成业务失败；优先支持绑定端口 0 并回传系统分配端口。
- 每个断言必须有有界超时，失败时打印当前连接状态、最后协议号和安全状态。

### 22.5 第二轮交付物

预期代码：

```text
client_core/src/transport/FrameCodec.{h,cpp}
client_core/src/transport/ClientSecureChannel.{h,cpp}
client_core/src/TcpTransport.{h,cpp}              生命周期与单写队列重构
client_core/tests/test_frame_codec.cpp
client_core/tests/test_transport.cpp
client_core/tests/test_secure_channel.cpp
client_core/tests/golden/app-security-v1.json
im_server/tests/test_e2e.cpp                     移除 DISABLED，恢复完整链路
```

预期证据：

```text
outputs/kernel-round2-transport-test.log
outputs/kernel-round2-security-test.log
outputs/kernel-round2-e2e-test.log
outputs/kernel-round2-checkjni.log
outputs/kernel-round2-hwasan.log
outputs/kernel-round2-packet-capture-note.md
```

### 22.6 第二轮最终判定规则

只有下列条件同时满足，第二轮才标记为“通过”：

- P3 与 P4 所有门禁项完成，没有跳过的安全负向测试。
- 旧完整业务 e2e 已解除 DISABLED，普通构建和 ASan+UBSan 均通过。
- Android arm64 完成真实应用握手和至少一个加密业务请求。
- 错误包长、错误证书、错误签名、篡改密文、重放 sequence 均 fail-close。
- 没有 `verify_none`、明文业务降级、并发写、未认证明文上抛等临时兼容代码。
- 第一轮 Native 生命周期、CheckJNI、HWASan 和 Search Golden 回归不退化。

以下情况只能判定“条件通过”，不能判定完成：

- 只有算法单测，没有真实 TLS loopback/e2e。
- `test_e2e` 仍为 DISABLED，或通过关闭服务端应用安全握手使其运行。
- 只证明 TLS 加密，没有完成 Ed25519 身份验证、Finished 和 AES-GCM sequence 防重放。
- 只完成 arm64 编译，没有在 Android arm64 进程中完成握手。
- ASan/HWASan 或 CheckJNI 出现未解释错误。

AVIF 偏色、真机和 x86_64 实跑继续作为已知遗留项记录；它们不阻塞 P3/P4 的代码开发，
但不得从总体验收风险清单中删除。

## 23. 第三轮工作与验收范围：P5 AccountSession、Token 与登录防重入

> 第三轮只迁移账号认证域。目标是让 Android UI 只表达“启动、密码登录、注册、退出”
> 等用户意图，由 C++ 内核统一完成连接、应用安全握手、设备证明、Token 判断、刷新、
> 自动登录、断线恢复和防重入。本轮不迁移消息数据库、Outbox、漫游、搜索和媒体业务。

### 23.1 起点、目标与范围边界

#### 23.1.1 已具备的前置条件

- P1/P2 已完成 Android Native 构建、`NativeSdkHandle`、`JniObserver` 和平台原子能力接口。
- P3 已完成可重连 `TcpTransport`、消息边界、connection generation 和单写队列。
- P4 已完成 TLS 1.3、CA/hostname/SPKI、四步应用握手和 AES-256-GCM 业务帧。
- Android arm64 已通过真实 socket 加密 Heartbeat 往返，不再只有进程内算法自检。
- 服务端已经支持密码登录、Token Login、Refresh Token 轮换、Refresh reuse 检测、退出登录、
  设备签名验证、Presence 替换和旧连接踢下线。
- Kotlin 现有 `AuthCoordinator`、`MainViewModel`、`ImClient`、`TokenVault` 和 `Prefs` 可作为
  行为基线，但不能作为 Native 新实现的业务依赖。

#### 23.1.2 本轮完成目标

1. 新增 C++ `AccountSession` 和 `AuthStateMachine`，成为唯一认证流程编排者。
2. C++ 实现密码登录、Token Login、Refresh、自动登录、退出和被踢状态处理。
3. C++ 实现登录 Single-Flight 与 Refresh Single-Flight。
4. 设备证明使用 Android Keystore 中不可导出的持久化 P-256 私钥；C++ 只构造待签名数据。
5. Token 的有效期判断、原子轮换、requestId 持久化和 reuse 清理由 Native 管理。
6. 增加正式 JNI/SDK 认证接口；第二轮 test-only Heartbeat JNI 不得冒充业务接口。
7. Android Native 模式下，`MainViewModel` 不再包含 Token 分支、Refresh Deferred、重连登录编排。
8. 保持 Legacy 模式可回退，但同一进程只能选择一个 Backend，禁止双连接、双登录。

#### 23.1.3 本轮不纳入

- P6：C++ SQLCipher、Room 数据导入和 DB key 迁移。
- P7/P8：消息 Outbox/Inbox、Native 消息查询、漫游和 sequence 补洞。
- P9：C++ FTS4、拼音搜索和高亮。
- P10～P12：Native 图片流水线、秒传、分片、下载调度和媒体缓存。
- P13 的会话列表、聊天页、搜索页和图片页整体切换。
- 删除 Kotlin `ImClient`、`SecureChannel`、Room 或媒体实现；它们仍作为 Legacy 回退路径保留。
- 将登录密码或其摘要持久化到 Native/平台 KV。

### 23.2 目标架构与所有权

第三轮完成后的 Native 认证链路：

```text
LoginScreen / App startup
        |
        | login() / start() / logout()
        v
JitongSdk + KernelBackend                 Kotlin 薄适配层
        |
        | stable JNI commands/events
        v
NativeSdkHandle
        |
        v
AccountSession + AuthStateMachine         C++ 唯一业务编排者
   |             |             |
   |             |             +--> TokenManager --> ISecureKv（平台原子能力）
   |             +----------------> DeviceProofService --> IP256Signer（Android Keystore）
   +------------------------------> ClientCore/TcpTransport/ClientSecureChannel
                                                  |
                                                  v
                                               im_server
```

所有权规则：

- UI 只持有显示状态，不持有 Token、requestId、重连次数或认证 Job。
- `AccountSession` 持有认证状态机、当前账号意图和连接 generation。
- `TokenManager` 持有内存中的 `TokenSession`；持久化只通过 opaque `ISecureKv`。
- Android 平台层只做 Keystore 签名和加密 KV 读写，不判断何时登录、刷新或重连。
- `ClientCore` 负责协议编码和安全发送，不把 Token 原文回调给 UI。
- 日志、错误对象和 JNI event 禁止携带密码、密码摘要、Access Token、Refresh Token、私钥。

### 23.3 P5-T01：稳定的 Account API、状态和错误模型

新增建议文件：

```text
client_core/include/client_core/account/AccountTypes.h
client_core/include/client_core/account/AccountSession.h
client_core/src/account/AccountSession.cpp
client_core/src/account/AuthStateMachine.h
client_core/src/account/AuthStateMachine.cpp
```

对外命令建议：

```cpp
void start();
void loginWithPassword(std::string tel, SecureString password, bool rememberAccount);
void registerAccount(std::string nick, std::string tel, SecureString password);
void logout(bool allDevices);
void cancelAuthentication();
AccountState accountState() const;
ConnectionState connectionState() const;
```

对 UI 暴露的状态：

```text
AccountState:
  Stopped
  NeedLogin
  Connecting
  Authenticating(method, requestId)
  Authenticated(userId)
  Refreshing
  BackingOff(attempt, retryAt)
  Kicked(reason)
  Failed(code, retryable)

ConnectionState:
  Disconnected / Connecting / SecureChannelReady / Connected
```

错误必须使用稳定错误码，例如：

```text
INVALID_INPUT
AUTH_OPERATION_IN_PROGRESS
NO_LOGIN_RECORD
ACCESS_TOKEN_EXPIRED
REFRESH_TOKEN_EXPIRED
REFRESH_TOKEN_REUSED
DEVICE_PROOF_FAILED
TLS_FAILED
APP_HANDSHAKE_FAILED
NETWORK_UNAVAILABLE
AUTH_REJECTED
KICKED_OFFLINE
CANCELLED
```

验收要求：

- JNI 只传稳定 DTO/枚举，不把 C++ 异常和服务端内部字符串直接抛给 UI。
- 每次状态变化包含单调递增的 `stateVersion`，迟到事件不能覆盖新状态。
- UI 重建后重新订阅可立即收到当前快照，不依赖补发历史事件恢复状态。

### 23.4 P5-T02：AuthStateMachine 与登录 Single-Flight

状态机建议：

```text
Stopped
  └─ start ─> LoadingCredential
                 ├─ no token/expired refresh ─> NeedLogin
                 └─ usable token ─> Connecting

NeedLogin ── passwordLogin ─> Connecting ─> SecureHandshaking ─> Authenticating
                                                            ├─ success ─> Authenticated
                                                            ├─ retryable ─> Backoff
                                                            └─ fatal ─> NeedLogin/Failed

Authenticated ── access near expiry ─> Refreshing ─> Authenticating(Token)
Authenticated ── kicked ─> Kicked
任意状态 ── logout/cancel ─> Stopped 或 NeedLogin
```

Single-Flight 规则：

- `AccountSession` 内同一时刻最多一个认证 operation。
- 同账号、同认证意图的重复调用复用同一 `operationId/requestId`，不再连接、不再发包。
- 已有认证在飞时，不同账号登录立即返回 `AUTH_OPERATION_IN_PROGRESS`，不得清理当前凭证。
- 从调用入口同步占位，再异步执行，关闭“协程/线程尚未调度”的重复提交窗口。
- `operationId + connectionGeneration` 同时校验；旧连接的 LoginRs/RefreshRs 一律丢弃。
- cancel、logout、destroy 后迟到签名、网络响应和计时器不能改变状态或重新建连。
- 被服务端踢下线进入 `Kicked`，禁止自动重连；必须等待用户显式登录。

必须覆盖的竞态：

- 连点登录 20 次。
- 登录按钮与自动登录同时触发。
- 正在 Refresh 时 App 前后台切换并触发 reconnect。
- 密码登录 A 在飞时提交账号 B。
- 断线回调和 LoginRs 同时到达。
- logout 与 RefreshRs 同时到达。
- SDK destroy 与设备签名回调同时发生。

### 23.5 P5-T03：DeviceProofService 与 Android Keystore

新增建议文件：

```text
client_core/src/account/DeviceProofService.h/.cpp
jitong_android/app/src/main/cpp/platform/p256_signer_jni.cpp
jitong_android/app/src/main/java/com/jitong/im/core/platform/AndroidP256Signer.kt
```

实现要求：

- C++ 是 canonical proof 格式的唯一构造者，必须继续与服务端 `DeviceProof` 逐字节一致。
- Android Keystore 创建不可导出的 P-256 私钥，并返回 X.509 SPKI DER 公钥。
- 平台接口只接收 `requestId + bytesToSign`，返回 `requestId + publicKey + DER signature/error`。
- 密码登录签名绑定：`operation + appSessionId + deviceId + tel\0passHash + publicKey`。
- Token Login 签名绑定：`operation + appSessionId + deviceId + accessToken`。
- Refresh 签名绑定：`operation + appSessionId + deviceId + refreshToken\0requestId`。
- Logout 签名绑定：`operation + appSessionId + deviceId + refreshToken\0allDevices`。
- 签名前再次确认当前 operation/generation；签名返回后也必须再次确认。
- Keystore key alias 与稳定 deviceId 绑定，不能每次进程启动生成新设备身份。
- 密钥失效、用户认证失败或签名超时必须 fail-close，不允许退回 Native 临时软件私钥。
- 密码明文仅用于当次摘要计算，使用后清理；日志禁止打印 proof 原文和签名输入。

兼容约束：

- 第二轮 `DeviceProofKey` 软件密钥只保留给桌面测试或明确的测试实现。
- Android 正式登录路径必须使用 `AndroidP256Signer`，不能继续使用每个 `ClientCore` 实例
  临时生成的 P-256 key/deviceId。

### 23.6 P5-T04：TokenManager、持久化与 Refresh Single-Flight

新增建议文件：

```text
client_core/src/account/TokenManager.h/.cpp
client_core/src/account/TokenStore.h/.cpp
client_core/src/account/SecureString.h/.cpp
```

Native `TokenSession` 至少保存：

```text
userId
sessionId
accessToken
refreshToken
accessExpiresAt
refreshExpiresAt
tokenFamily/version（若协议可获得）
deviceId
```

实现要求：

- Access Token 只在剩余有效期大于安全窗口时直接使用，建议窗口为 30～60 秒。
- Access 临期且 Refresh 有效：所有等待认证的 Socket/HTTP 操作加入同一个等待队列。
- 同一 TokenSession 最多一个 Refresh 请求在飞；10 个调用者必须共享同一 future/result。
- 首次 Refresh 前生成并持久化 `pendingRefreshRequestId`，网络重试复用相同 requestId。
- 收到成功响应后，以一个平台 KV 事务/版本化双槽方案原子替换新 TokenSession，并清除 pending。
- 超时或断线不清除 pending requestId；重新连接和应用握手后用原 requestId 重试。
- 明确失败、Refresh 过期、设备证明失败或 reuse 检测：清除完整 Token family 和 pending，
  唤醒全部等待者为 `NeedLogin`，不能各自再次 Refresh。
- 内存中的 Token 使用 `SecureString`/可清零 buffer；复制次数有测试或审查约束。
- Token 不经过 `JniObserver` 上抛，不进入 `StateFlow`、异常文本、Crash 日志或埋点。

自动登录决策表：

| 本地状态 | 动作 | 结果 |
|---|---|---|
| 无 TokenSession | 不连接认证或连接后保持未认证 | `NeedLogin` |
| Access 有效且剩余时间充足 | TLS/应用握手后 Token Login | 成功进入 `Authenticated` |
| Access 临期、Refresh 有效 | 先 Refresh，再用新 Access Token Login | 成功进入 `Authenticated` |
| Access 过期、Refresh 有效 | Refresh 后 Token Login | 成功进入 `Authenticated` |
| Refresh 已过期 | 清凭证 | `NeedLogin` |
| Refresh reuse/Token family 吊销 | 清整族凭证，禁止自动重试 | `NeedLogin` + 安全错误 |
| KV 损坏或解密失败 | 清损坏记录，不上传内容 | `NeedLogin` |

### 23.7 P5-T05：连接恢复、前后台与被踢处理

实现要求：

- 自动重连由 `AccountSession` 管理，UI 和 `MainViewModel` 不持有 reconnect Job。
- 网络不可用时等待网络事件，不执行忙轮询。
- 网络恢复后采用带 jitter 的指数退避，例如 1s、2s、4s、8s，封顶 30s。
- 每轮重连必须新建 TLS/应用安全会话；旧 AppEncryptedFrame 不得复用。
- 重连后根据 Token 有效期重新执行 Token Login/Refresh，不能假定旧 socket 登录态仍有效。
- 正常后台切换不立即退出；心跳/系统网络变化决定连接是否需要恢复。
- `KickedOffline`、主动 logout、Refresh reuse 属于禁止自动重连状态。
- 密码错误、设备签名错误等确定性失败不做指数重试。
- 同一进程只允许一个 `AccountSession` 拥有活动 `ClientCore`；Legacy 与 Native 不得同时连接。

### 23.8 P5-T06：正式 JNI、JitongSdk 与 Android 薄 UI 接入

正式 JNI 建议：

```text
nativeStart(handle)
nativeLoginWithPassword(handle, tel, password, remember)
nativeRegister(handle, nick, tel, password)
nativeLogout(handle, allDevices)
nativeCancelAuthentication(handle)
nativeGetAccountState(handle)
```

事件建议：

```text
onAccountStateChanged(stateDto)
onConnectionStateChanged(stateDto)
onAuthOperationCompleted(operationId, resultCode)
```

Android 改造要求：

- `JitongSdk.start()` 成为冷启动唯一入口。
- `LoginScreen` 只校验纯展示级输入并调用 `sdk.login()`/`sdk.register()`。
- Native 模式下从 `MainViewModel` 删除或旁路：
  - `refreshAwaiter`；
  - `refreshMutex`；
  - `refreshRequestInFlight`；
  - `pendingRefreshRequestId` 的业务判断；
  - Token 有效期分支；
  - 自动重连认证 Job；
  - 被踢后清 Token/重连的编排。
- Kotlin 不解析 `LoginRs/TokenLoginRs/RefreshTokenRs`；Native 内核消费后只发稳定状态。
- UI 按钮禁用只改善体验，不能作为 Single-Flight 的正确性保证。
- Backend 在进程启动时固定为 Legacy 或 Native，运行中切换必须先重启进程。
- 本轮只切认证域；消息、Room、搜索、媒体仍可暂时由同一选定 Backend 的 Legacy 路径承担，
  但禁止额外创建第二条 Socket。若暂时无法共享同一 Native 连接，则第三轮不能宣称 UI 切换完成。

### 23.9 P5-T07：协议处理与服务端兼容

ClientCore/AccountSession 增加处理：

- `TokenLoginRq/TokenLoginRs`；
- `RefreshTokenRq/RefreshTokenRs`；
- `LogoutRq/LogoutRs`；
- 密码 `LoginRs` 的完整 TokenSession；
- `KickedOffline`；
- 认证失败、协议解析失败和连接关闭。

约束：

- 所有认证包只能在应用安全握手完成后通过 `AppEncryptedFrame` 发送。
- `requestId` 长度、Token 长度、signature 和响应枚举均在使用前校验。
- Login/Refresh 响应只接受当前 operationId 和 connection generation 对应的结果。
- 服务端协议保持向后兼容；如确需新增字段，必须使用 protobuf 新字段号，不能复用旧字段。
- Refresh 幂等语义以 `deviceId + requestId` 为准，客户端网络重试不得生成新 requestId。
- HTTP 鉴权读取 TokenManager 当前 Access Token；刷新中等待同一 Single-Flight，不自行刷新。

### 23.10 自动化测试范围

#### 23.10.1 C++ 单元测试

新增建议：

```text
client_core/tests/test_auth_state_machine.cpp
client_core/tests/test_token_manager.cpp
client_core/tests/test_account_session.cpp
client_core/tests/test_device_proof_service.cpp
```

至少覆盖：

- 每一条合法状态转换及所有非法事件。
- 相同密码登录重复提交复用 operationId。
- 不同账号并发提交返回 `AUTH_OPERATION_IN_PROGRESS`。
- 10/100 个并发取 Token 请求只触发一次 Refresh。
- Refresh 超时、断线后复用原 requestId。
- Refresh 成功原子轮换，旧 Token 被清零。
- Refresh reuse 清整族并唤醒全部等待者。
- 签名超时、失败、取消和迟到回调。
- old generation LoginRs/RefreshRs 不改变新状态。
- logout、kick、destroy 与网络回调竞态。
- 退避计时使用 fake clock，测试禁止真实等待 30 秒。

#### 23.10.2 Server/ClientCore E2E

至少覆盖：

1. 密码登录成功，返回完整 TokenSession。
2. 密码错误、手机号不存在和设备签名错误。
3. Access Token Login 成功、过期失败、设备不匹配失败。
4. Refresh 成功并轮换双 Token。
5. 相同 requestId 重试得到同一语义结果，不重复轮换。
6. 使用旧 Refresh Token + 新 requestId 触发 reuse，整个 family 被吊销。
7. 新连接同账号登录，旧连接收到 Kicked；客户端进入 `Kicked` 且不重连。
8. logout 当前设备、logout all devices。
9. 断线发生在 Refresh 请求发出前、服务端提交后响应前、响应到达后持久化前。
10. 所有认证包在抓包中不可见 Token、手机号、密码摘要和测试 marker。

#### 23.10.3 Android arm64 Instrumentation

在真实 App 进程覆盖：

- Android Keystore P-256 key 首次生成、重启复用、公钥稳定。
- 真实 socket 密码登录。
- 保存 Token 后杀进程，重启自动 Token Login。
- 人为缩短 Access 有效期，10 个并发请求只出现一次 Refresh。
- Refresh 中断网，恢复后沿用 requestId 完成。
- 连点登录 20 次，服务端只观察到一次有效认证 operation。
- 同账号第二设备登录，第一设备收到 Kicked 且不自动重连。
- logout 后 Token/KV/pending 清理，重启保持 `NeedLogin`。
- CheckJNI 无异常；HWASan/ASan 无 UAF、double free、泄漏或越界。

测试不得使用 `Assume` 把“服务端未启动、参数缺失、设备非 arm64”等门禁条件变成跳过后绿。

### 23.11 安全测试与审查清单

- [ ] TLS/SPKI 和应用安全握手失败时不进入认证状态机后续步骤。
- [ ] proof 明确绑定当前 `appSessionId`、operation、deviceId 和 credential。
- [ ] Android 正式路径私钥不可导出；软件私钥不能静默兜底。
- [ ] 登录、Token Login、Refresh、Logout 都有设备签名。
- [ ] Token、密码、密码摘要、签名输入不进入日志、异常、埋点、UI State。
- [ ] Token 内存 buffer 在轮换、退出和销毁时清零。
- [ ] Refresh reuse 清理完整 family 并停止自动重试。
- [ ] 迟到响应、迟到签名和旧 connection generation 不污染新会话。
- [ ] 被踢后禁止自动重连，避免与新设备形成踢线风暴。
- [ ] Single-Flight 由 C++ 内核保证，不依赖按钮 disabled 或 Kotlin `@Synchronized`。
- [ ] 所有等待 Refresh 的调用都能在成功、失败、取消、断线和 destroy 时结束。

### 23.12 第三轮交付物

预期代码：

```text
client_core/include/client_core/account/AccountTypes.h
client_core/include/client_core/account/AccountSession.h
client_core/src/account/AuthStateMachine.{h,cpp}
client_core/src/account/AccountSession.cpp
client_core/src/account/TokenManager.{h,cpp}
client_core/src/account/TokenStore.{h,cpp}
client_core/src/account/DeviceProofService.{h,cpp}
client_core/src/account/SecureString.{h,cpp}
client_core/tests/test_auth_state_machine.cpp
client_core/tests/test_token_manager.cpp
client_core/tests/test_account_session.cpp
jitong_android/app/src/main/cpp/...            正式认证 JNI + P-256 signer bridge
jitong_android/app/src/main/java/...           JitongSdk 认证 API + Android Keystore 适配
im_server/tests/test_auth_native_e2e.cpp        Native 完整认证链路
```

预期证据：

```text
outputs/kernel-round3-unit-test.log
outputs/kernel-round3-auth-e2e.log
outputs/kernel-round3-refresh-singleflight.log
outputs/kernel-round3-android-arm64.log
outputs/kernel-round3-checkjni.log
outputs/kernel-round3-asan-client.log
outputs/kernel-round3-asan-server.log
outputs/kernel-round3-packet-capture-note.md
outputs/kernel-round3-acceptance.md
```

### 23.13 第三轮验收门禁

#### 23.13.1 功能门禁

- [ ] C++ 密码登录、Token Login、Refresh、自动登录和 Logout 全部走通。
- [ ] 密码登录成功后 TokenSession 只由 TokenManager 接收和保存，不上抛 UI。
- [ ] App 杀进程重启后可自动 Token Login。
- [ ] Access 临期/过期且 Refresh 有效时可自动刷新后登录。
- [ ] Refresh 过期、reuse、KV 损坏时清理并进入 `NeedLogin`。
- [ ] 新设备登录后旧设备进入 `Kicked`，不自动重连。

#### 23.13.2 并发与生命周期门禁

- [ ] 连点登录 20 次只产生一个认证 operation。
- [ ] 100 个并发取 Token 请求只产生一个 Refresh 网络请求。
- [ ] Refresh 断线重试复用同一 requestId。
- [ ] logout/cancel/destroy 后所有 waiter、计时器、签名和网络回调确定结束。
- [ ] 旧连接迟到响应不能覆盖新连接状态。
- [ ] 普通、ASan+UBSan、Android CheckJNI/HWASan 无竞态导致的崩溃和内存错误。

#### 23.13.3 架构门禁

- [ ] `AccountSession` 是认证域唯一编排者。
- [ ] Android 平台代码只提供签名、Secure KV、网络状态等原子能力。
- [ ] Native 模式下 `MainViewModel` 不再判断 Token 有效期、发 Refresh 或编排认证重连。
- [ ] UI 不解析认证 protobuf，不持有 Token/requestId。
- [ ] Legacy 与 Native Backend 同一进程只启用一个，不产生第二条 Socket。
- [ ] 第二轮 test-only JNI 不被 UI 业务路径调用。

#### 23.13.4 安全门禁

- [ ] 四类认证操作的设备签名均绑定当前 appSessionId，并通过负向篡改测试。
- [ ] 错误证书/SPKI/服务端签名时不会发送任何认证帧。
- [ ] 抓包检索不到手机号、密码摘要、Access/Refresh Token 和唯一测试 marker。
- [ ] 日志与崩溃信息扫描不包含凭证。
- [ ] Refresh reuse 后旧 Access/Refresh Token 全部不可继续使用。

#### 23.13.5 回归门禁

- [ ] P1/P2 Native 生命周期、CheckJNI、HWASan 不退化。
- [ ] P3 FrameCodec、重连、万帧单写测试不退化。
- [ ] P4 TLS/SPKI、安全通道 Golden 和篡改/重放测试不退化。
- [ ] Legacy Android 登录、文本、图片、搜索基线仍可运行。
- [ ] Android arm64 真实环境完成“冷启动 → 自动登录/密码登录 → 被踢/退出”闭环。

### 23.14 第三轮最终判定规则

只有 23.13 全部门禁满足，第三轮才能标记为“完成”。以下情况只能判定“条件通过”：

- C++ 已实现 Token 协议，但 Android UI 仍由 `MainViewModel` 判断和发起 Refresh。
- 只有进程内 Mock，没有 Android arm64 到真实 `im_server` 的认证 E2E。
- Single-Flight 只依赖 UI 禁用按钮或 Kotlin 协调器，Native 可被并发绕过。
- Refresh 成功但 Token 轮换不是原子操作，崩溃后可能出现新旧 Token 混搭。
- Android 正式路径仍使用临时软件 P-256 私钥，或 Keystore 失败后静默降级。
- 被踢后仍自动重连，可能形成多设备互踢循环。
- Token/密码摘要出现在日志、JNI 状态、抓包或测试制品中。
- ASan/HWASan、CheckJNI、安全负向测试存在跳过或未解释失败。

本轮完成后再进入 P6：C++ SQLCipher 数据层。P6 开始前必须冻结 `AccountSession` 向数据库
提供的账号标识、Token 可用事件和登出清理语义，避免数据库迁移反向依赖 Android ViewModel。

---

## 24. 第五轮工作与验收范围：P6 C++ SQLCipher 数据底座

> 轮次说明：第四轮已用于 P5 生产适配、代码审查修复、Android Keystore 与加密
> TokenStore 收尾，因此下一轮按实际执行顺序记为第五轮。

### 24.1 本轮目标与边界

本轮目标是把 Android 当前 Room + SQLCipher 承担的本地数据能力下沉为可跨端复用的 C++
数据底座，为后续 P7 消息、P8 同步、P9 搜索、P10～P12 媒体迁移提供唯一持久化入口。

本轮必须完成：

1. SQLCipher 以真实 Android Native 依赖接入 `client_core`，arm64-v8a 与 x86_64 均可构建。
2. 建立版本化 Schema、MigrationRunner 和每账号独立加密数据库。
3. 建立单 Writer 线程/连接、只读连接池、异步命令队列和查询失效通知。
4. 建立 Android 数据库密钥平台桥；C++ 不生成、打印或经事件层上抛数据库明文密钥。
5. 建立 Room → Native 的可中断、可重复、可校验迁移器，但先写入影子 Native 数据库。
6. 建立 C++ 单元测试、并发测试、迁移测试和 Android 真机/AVD 加密验证。

本轮明确不做：

- 不把 `MainViewModel`、`ChatStore`、聊天 UI 切到 Native 数据库。
- 不让 Legacy Room 与 Native DB 同时承接在线消息写入，不做长期业务双写。
- 不迁移 P7 Outbox/Inbox 网络编排、P8 漫游补洞、P9 拼音检索策略或 P12 图片缓存策略。
- 不删除、覆盖或原地升级用户现有 Room 数据库；Native 数据库使用独立路径。
- 不启用 Native Backend，不创建新的业务 Socket。

只有等 P7～P12 的完整功能能力具备后，才在统一切换轮次中把 UI、网络和数据库所有权一起迁移。

### 24.2 目标架构与线程模型

```text
Android UI / Legacy Room（本轮仍为线上事实源）
             │ 只读分页导出 DTO；不传 sqlite/Room 对象
             ▼
      JNI Migration Bridge
             │ submit，不在 JNI/主线程等待磁盘
             ▼
┌──────────────────── client_core ────────────────────┐
│ NativeDatabase                                      │
│   ├─ DbCommandQueue ──> 单 Writer thread/connection │
│   │                       ├─ transaction             │
│   │                       └─ commit 后 invalidation  │
│   ├─ ReadPool ───────> 1～N readonly connections    │
│   ├─ SchemaManager / MigrationRunner                │
│   └─ ImportCheckpoint / Verification                │
└─────────────────────────────────────────────────────┘
             │
             ▼
 filesDir/native_db/account_<ownerId>.db（SQLCipher）
```

线程硬约束：

- 所有 INSERT/UPDATE/DELETE、Schema Migration 与 checkpoint 更新只允许 Writer 执行。
- 网络线程、JNI 线程和 Android 主线程只提交命令，不直接执行 SQL，也不等待长事务。
- 读连接必须以 readonly 打开；写连接不得借给查询调用方。
- Writer 提交事务后再发布版本号/invalidation，查询方不能观察到半事务状态。
- close/logout/destroy 必须停止接收新任务、完成或取消队列、唤醒 waiter、join 线程后关闭连接。
- 回调不得在持数据库互斥锁或 SQLite transaction 时进入 Java/UI，防止重入死锁。

### 24.3 P6-T01：SQLCipher Native 构建与能力探测

改动要求：

- 为 Android 两 ABI 构建/引入同版本 SQLCipher 静态库及头文件，并链接到 `client_core`。
- `CLIENT_CORE_WITH_SQLCIPHER` 与普通桌面 SQLite 构建分离；生产 Android 禁止误链系统 SQLite。
- 启动时检查 `PRAGMA cipher_version`，取不到版本视为 fail-close，不能创建“看似成功”的明文库。
- 打开顺序固定为：`sqlite3_open_v2` → `sqlite3_key`/raw key → cipher 参数 → 首次受保护查询。
- 错误密钥、空密钥、密钥长度不符、数据库头损坏都必须返回结构化错误，不能删除旧库重建。
- 新建 Native DB 的文件权限限制为应用私有；数据库路径不得由 UI 任意拼接。
- 在构建产物和运行时报告中记录 SQLCipher 版本与 ABI，但不得记录 key。

验收标准：

- [ ] arm64-v8a、x86_64 均完成编译、链接和 `.so` 装载。
- [ ] `PRAGMA cipher_version` 返回预期版本，且 Android 生产构建符号确认来自 SQLCipher。
- [ ] 正确 key 可重启读取；错误 key、空 key 均失败且原文件摘要不变。
- [ ] 普通 `sqlite3` CLI 无法读取 schema、正文、手机号、Token 或测试 marker。
- [ ] 数据库文件中 `strings`/十六进制检索不到唯一明文 marker。

### 24.4 P6-T02：DB Key、账号隔离与平台桥

密钥设计：

- 每个 `ownerId` 使用独立随机 32 字节数据库 key，不能由 userId、手机号或固定常量直接派生。
- Android 平台层只提供 `load/create/delete opaque key` 原子能力；数据库业务策略由 C++ 决定。
- key 由 Android Keystore 包装后持久化，Native 使用期间放入可清零 buffer，打开完成后尽快清零副本。
- 数据库路径只接受规范化后的数值 ownerId，例如
  `filesDir/native_db/account_<ownerId>.db`，拒绝 `..`、斜杠、空 ownerId 和越界值。
- 普通 logout 只关闭当前账号数据库，不删除聊天数据；“清除本机数据”必须是独立显式 API。
- 切换账号必须先完整关闭旧账号 Writer/ReadPool，再装载新账号 key 和数据库。
- key 丢失、Keystore 失效或解密失败返回 `DbKeyUnavailable`，保留密文 DB 供诊断/恢复，不静默重建。

验收标准：

- [ ] 同一账号重启后 key 和数据可恢复，不同账号的 key、路径和内容互不可读。
- [ ] Native/Kotlin 日志、AccountEvent、异常文本、崩溃报告中没有 key 或 SQLCipher raw-key pragma。
- [ ] 错误 ownerId/path traversal 输入在创建文件前被拒绝。
- [ ] logout、切账号、destroy 后 key buffer 清零，所有连接关闭。
- [ ] Keystore/KV 故障注入时 fail-close，旧数据库文件没有被覆盖、删除或截断。

### 24.5 P6-T03：版本化 Schema 与迁移事务

建议新增：

```text
client_core/include/client_core/storage/NativeDatabase.h
client_core/include/client_core/storage/DbTypes.h
client_core/include/client_core/storage/DbError.h
client_core/src/storage/NativeDatabase.cpp
client_core/src/storage/SchemaManager.cpp
client_core/src/storage/DbCommandQueue.cpp
client_core/src/storage/ReadPool.cpp
client_core/src/storage/migrations/001_initial.sql
```

初始 Schema 至少包含：

- `messages`：消息唯一事实、状态、正文/媒体引用、server time、conversation seq、local order。
- `conversations`：会话摘要、last seq、read seq、unread、最后消息引用。
- `friends`：账号维度好友资料与版本。
- `message_fts`：先建立与正文一一对应的 FTS4 表和重建能力，P9 再补拼音策略。
- `sync_watermarks`：会话同步水位、连续区间与补洞状态。
- `outbox`：待发送/发送中/失败任务与重试信息。
- `media_records`：原图、缩略图、hash、尺寸和缓存状态。
- `transfer_tasks`：上传/下载分片任务与断点状态。
- `migration_meta`：schema version、Room 导入 checkpoint、完成标志和校验摘要。

必须固化的字段/索引约束：

```text
UNIQUE(owner_id, msg_id)
INDEX messages(owner_id, peer_id, conversation_seq)
INDEX messages(owner_id, peer_id, server_time, msg_id)
INDEX messages(owner_id, status, local_order)
UNIQUE(owner_id, peer_id, conversation_seq) WHERE conversation_seq > 0
INDEX conversations(owner_id, last_message_time)
```

约束说明：

- `msg_id` 负责幂等，`conversation_seq` 负责会话顺序与补洞，二者不可互相替代。
- 同一 seq 的冲突必须返回数据一致性错误并保留证据，禁止 `INSERT OR REPLACE` 覆盖不同 msg_id。
- Migration 每一版都在单事务中完成；失败时 `user_version` 与 schema 保持旧版本。
- 禁止使用 `fallbackToDestructiveMigration` 或捕获异常后删库。
- 所有 SQL 使用绑定参数；表名、排序字段等动态部分必须来自枚举白名单。

验收标准：

- [ ] 空库可从 0 升到当前版本，连续升级和跨版本升级结果一致。
- [ ] 每个 Migration 中途故障均完整回滚，重启后可再次执行。
- [ ] msg_id 重复导入不增行；相同会话 seq 对应不同 msg_id 时明确报冲突。
- [ ] `EXPLAIN QUERY PLAN` 证明历史分页、msg_id 查重、outbox 查询命中目标索引。
- [ ] FTS 行数与可检索文本消息数一致，重建两次结果保持一致。

### 24.6 P6-T04：单 Writer、只读连接池与取消语义

API/实现要求：

- `DbCommandQueue` 使用有界队列；满载时返回 `QueueFull`，不能无限吃内存。
- 写命令携带 operation id/cancellation token；取消未开始任务不得执行，已开始事务按操作语义提交或回滚。
- Writer 对一条业务原子操作只开启一次事务，例如“消息 + FTS + 会话摘要 + checkpoint”。
- 查询连接池初始建议 2 条，可配置但有上限；每条连接只在一个查询作用域内独占。
- 查询结果必须复制为值对象后再离开连接作用域，不能向 JNI 暴露 `sqlite3_stmt*` 或裸指针。
- 支持 keyset pagination：历史消息按 `(conversation_seq,msg_id)` 或 `(server_time,msg_id)` 翻页，
  禁止深分页依赖大 OFFSET。
- invalidation 只携带表/会话/version，不携带整页消息；UI 以后通过 Query API 重新获取。

并发与生命周期验收：

- [ ] 8～16 个生产线程并发投递至少 10 万条写命令，无 SQLITE_BUSY、死锁、丢失和重复。
- [ ] 验证数据库实际只有一个可写 connection，所有写 SQL 的线程 id 相同。
- [ ] 多个只读查询可并行，任何 readonly connection 执行写 SQL 都被拒绝。
- [ ] Writer 事务提交前查询看不到半成品，提交后收到一次合并 invalidation。
- [ ] 队列满、取消、SQL 失败、close、logout、destroy 时每个任务都得到确定结果。
- [ ] TSAN 可用环境无数据竞争；ASan+UBSan 无 UAF、泄漏、越界或 double free。

### 24.7 P6-T05：Room → Native 可恢复导入

迁移采用“旧库只读导出、Native 幂等导入、校验通过后标记”的影子迁移：

1. Kotlin Room DAO 按稳定游标分页导出，不一次性把全库加载进内存。
2. DTO 只包含协议稳定字段；JNI 校验字符串长度、枚举、时间、seq、文件大小和 ownerId。
3. C++ Writer 每批在一个事务中按 `(owner_id,msg_id)` 幂等写入消息、FTS 和会话摘要。
4. 每批提交后更新 `migration_meta` checkpoint；进程被杀后从最后已提交游标恢复。
5. 全量导入后比较消息数、会话数、各会话 max/min seq、FTS 行数及抽样内容摘要。
6. 所有校验通过后原子写 `migration_completed=1`；失败保留旧 Room 为事实源并报告差异。

迁移安全边界：

- 不删除旧 Room 数据库，不修改旧表，不抢占当前 UI 的数据库所有权。
- 本轮测试迁移使用测试账号副本或测试夹具；不得用开发者真实聊天库做破坏性试验。
- 再次执行完整迁移结果必须相同；不能因重复导入增加未读数、会话数或 FTS 行。
- 遇到单条坏数据要记录不含正文的定位信息并整体判定失败，不得悄悄跳过后宣称完成。

验收标准：

- [ ] 0 条、1 条、分页边界、10 万条数据均可完成迁移。
- [ ] 在 0%、一批提交后、50%、最终标记前强杀进程，重启均可恢复并得到相同结果。
- [ ] 连续执行迁移两次，所有业务表计数、未读数、max_seq 和摘要不变化。
- [ ] 注入重复 msg_id、seq 冲突、非法 UTF-8、超长正文和损坏媒体元数据，错误可定位且不误标完成。
- [ ] 校验失败时 Legacy Room 仍能正常打开、搜索和展示。

### 24.8 正式 JNI/SDK 范围

本轮只暴露数据库生命周期、迁移和测试所需的稳定接口，不暴露任意 SQL：

```text
nativeOpenAccountDatabase(handle, ownerId, platformBridge)
nativeCloseAccountDatabase(handle)
nativeSubmitMigrationBatch(handle, batchDto, checkpoint)
nativeFinishMigration(handle, expectedSummary)
nativeGetMigrationState(handle)
nativeRunDatabaseSelfTest(handle)
```

约束：

- 禁止提供 `nativeExecSql(String)`、数据库裸句柄或 SQLCipher key getter。
- JNI 输入先做大小和范围校验；超大数组/字符串在分配前拒绝。
- Java 回调必须在事务提交和锁释放后发生。
- `NativeSdkHandle` 持有数据库组件并按“停止任务 → join → 关读连接 → 关 Writer → 清 key”析构。
- `JitongSdk` 本轮只增加迁移/自检入口，默认 Backend 和 UI 路径保持不变。

### 24.9 自动化测试和证据

#### 24.9.1 C++ 单元/并发测试

建议新增：

```text
client_core/tests/test_native_database.cpp
client_core/tests/test_schema_migrations.cpp
client_core/tests/test_db_command_queue.cpp
client_core/tests/test_read_pool.cpp
client_core/tests/test_room_import.cpp
```

必须覆盖 Schema/Migration 回滚、索引命中、幂等、seq 冲突、事务可见性、队列背压、取消、
close/destroy 竞态、10 万写入压力和 keyset pagination。

#### 24.9.2 Android arm64/x86_64

- 两 ABI 编译/链接；arm64 AVD 或真机执行真实 SQLCipher open/write/close/reopen。
- Android Keystore 包装 DB key 跨进程恢复。
- 错 key、KeyStore 失效、密文损坏 fail-close 且文件不被覆盖。
- Room 测试夹具迁移、强杀恢复、二次迁移幂等与摘要对账。
- CheckJNI 开启运行全部数据库 JNI 测试。

#### 24.9.3 安全与稳定性

- 普通 SQLite/strings 明文负向验证。
- 日志和测试制品扫描 DB key、消息 marker、Token，必须零命中。
- ASan+UBSan 跑全部新 C++ 测试；可用时增加 TSAN Writer/ReadPool 并发测试。
- Android 长稳：循环 open/import/query/close 100 次，句柄、线程、fd 数量回到基线。

本轮应输出独立证据，不与前四轮验收报告混写：

```text
outputs/kernel-round5-acceptance.md
outputs/kernel-round5-unit.log
outputs/kernel-round5-db-stress.log
outputs/kernel-round5-asan.log
outputs/kernel-round5-tsan.log                 # 环境不可用时记录原因，不伪造通过
outputs/kernel-round5-android-arm64.log
outputs/kernel-round5-android-x86_64-build.log
outputs/kernel-round5-checkjni.log
outputs/kernel-round5-room-migration.log
outputs/kernel-round5-encryption-note.md
```

### 24.10 第五轮最终验收门禁

#### 功能门禁

- [ ] Native SQLCipher 可按账号创建、关闭、重启恢复数据库。
- [ ] 初始 Schema、全部索引和版本化 Migration 可重复、可回滚。
- [ ] 消息、FTS、会话摘要和迁移 checkpoint 可在一个 Writer 事务中提交。
- [ ] Room 影子迁移可中断恢复，完整对账后才写完成标志。
- [ ] 迁移重复执行不增加消息、会话、FTS 或未读计数。

#### 架构门禁

- [ ] 只有 Writer connection/thread 可写，网络/JNI/UI 线程均不执行 SQL。
- [ ] ReadPool 只读且有界，查询结果不泄漏 SQLite 句柄。
- [ ] Android 只提供 key/文件等平台原子能力，不包含 Schema、索引、迁移策略。
- [ ] Native DB 使用独立影子路径；本轮不启用业务双写、不切 UI、不新建 Socket。
- [ ] JNI/SDK 不暴露任意 SQL、裸数据库指针或明文 key。

#### 安全门禁

- [ ] Android Native 生产构建确认链接 SQLCipher，`cipher_version` 有效。
- [ ] 普通 SQLite、strings、十六进制搜索均无法得到 schema 和唯一明文 marker。
- [ ] 空 key、错 key、key 丢失、密文损坏均 fail-close，原数据库不删除、不覆盖。
- [ ] 每账号独立随机 key；切账号、logout、destroy 后内存 key 副本清零。
- [ ] 日志、JNI 事件和验收制品不包含 DB key、Token 或消息正文。

#### 并发与生命周期门禁

- [ ] 10 万并发投递写入无死锁、SQLITE_BUSY、丢失或重复。
- [ ] 队列背压、取消、SQL 失败、close/destroy 后所有 waiter 确定结束。
- [ ] Writer commit 前无半事务可见，commit 后 invalidation 次数正确。
- [ ] ASan+UBSan 全绿；CheckJNI 全绿；线程/fd/Native handle 压测后回到基线。

#### 回归门禁

- [ ] P1/P2 生命周期、自检、CheckJNI 不退化。
- [ ] P3 Transport/FrameCodec/单写发送队列不退化。
- [ ] P4 TLS/SPKI/应用安全通道及负向测试不退化。
- [ ] P5 AccountSession、Keystore、TokenStore、Single-Flight 全部回归通过。
- [ ] Legacy Android 登录、文本、图片、搜索和旧 Room 数据仍正常运行。

### 24.11 判定与下一轮入口

只有 24.10 全部门禁满足，第五轮才能标记“完成”。以下情况只能判定“条件通过”：

- Android 仍链接普通 SQLite，或只在桌面模拟 SQLCipher。
- 只有同步 `SqliteStorage`，没有 Writer queue、readonly pool、取消与析构语义。
- 迁移只比总行数，不校验会话 seq、FTS、未读和抽样摘要。
- 使用破坏性迁移、错误 key 自动删库，或失败后仍写 `migration_completed`。
- 为了测试直接让 UI/Legacy 网络双写 Native DB。
- Android 测试被 `Assume` 跳过后仍标记通过。

第五轮完成后进入 P7“消息 Outbox/Inbox”。P7 只消费本轮稳定的 `NativeDatabase` 命令和查询
接口，不允许重新把 SQL、事务、去重或会话摘要逻辑搬回 JNI/Kotlin。
