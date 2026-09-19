# 第七轮修复：阶段验收记录

日期：2026-09-11。基线：`c6d6c16` 加当前工作区未提交改动。
对应方案：`kernel-round7-fix-plan.md`。本报告只覆盖本次已实施的修复，不替代第七轮完整验收。

## 结论

本次基础修复回归通过；**第七轮整体仍为 PARTIAL，不能标记完成或启用全量 Native 切换**。
F06 已完成文本消息的 Native 持久发送骨架，但真实在线 ACK/重连自动泵与 UI 切换仍未闭环；F08～F10
的媒体迁移、在线 delta 和启动 cutover 尚未完成；F07 已补到
Native 搜索核心、JNI/SDK 调用及真机数据链路，但 UI 切换、拼音逐字高亮映射和性能差分尚未完成。
本次没有提交或推送代码，没有清理用户业务数据库。

## 已实施

### F01：持久发号、Outbox 与回执

- 新增 migration 004，保留 001/002/003。序号表从 messages/outbox 最大值初始化；Writer 在草稿事务内
  分配并推进序号。消息/Outbox/FTS/会话摘要一起提交，Service 重建不再重用内存计数器。
- 消息 ID 改为 OpenSSL RAND_bytes 生成的 128 位随机标识。允许重试使用原 msgId；提交失败/超时时仍
  返回已分配的 msgId，供查询提交结果，避免未知提交结果被当作新消息重发。
- Outbox 保存实际 ChatInfoRq protobuf、协议号和 payload_version，不保存旧安全连接的密文/sequence。
- 新增 findMessage、claimOutbox、finishOutboxAttempt、cancelOutbox。领取在 Writer 事务内加租约和 attempt；
  领取上限64、租约上限3600秒。按 attempt 去重失败响应，取消状态不自动恢复；旧 v0 载荷领取时从消息重建。
- onProtocolAck 显式映射实际协议：0→在线已送达，1→离线转存，2/4→永久拒绝，3→退避重试；未知结果拒绝。
  ChatInfoRs 没有 serverTime，不以收包时间伪造。成功/重复回执保留已有 seq，冲突 seq 报错。
- 此处是持久队列和回执接口闭环，**尚未接入生产 Socket 自动驱动领取/重连重发**；永久拒绝/取消的 UI
  呈现也须由后续 SDK 接线完成。租约到期重发可能发生网络重复，逻辑去重仍依赖同一 msgId。

### F02：Runtime 生命周期

- 生命周期命令串行化；销毁阶段明确为 Destroying；关闭期间 start 返回失败，终态不复活。
- database() 返回锁内取得的 shared_ptr；禁止运行中替换，校验已打开数据库的 owner。
- 修复 stop 后假运行：stop 仅停止业务服务并保留已打开 SQLCipher 库，restart 继续使用同一 Ready 实例；
  logout 才关闭并解除数据库引用，要求下次登录通过密钥桥重新打开。JNI 重新开库会同步注入已停止 Runtime，
  显式 close 会先停止 Runtime 并解除引用，旧库在句柄锁外 drain/close。
- JNI 重复创建同账号 Runtime 复用旧实例；不在句柄锁内替换并销毁运行中的 Runtime；跨账号替换拒绝。
- operationId 包含进程内实例序号、账号、generation 和命令序号。接收新命令前预留有界容量；停止后
  未终结命令保留取消结果，迟到回调不能覆盖；失效事件必须携带当前 generation。
- databaseReady 与 Runtime Running 分开。Running **仍不等于账号在线或同步就绪**。
- 尚缺：Runtime-owned completion executor、完整服务装配与业务任务取消/drain。现有 DbCommandQueue
  completion executor 仍是进程级共享实现，不能宣称其已迁入 Runtime。

### F03：双包装与解锁

- 新增只解锁接口 unlockRealKey；无密码首次访问不创建包装；密码包装缺失时仍尝试设备包装。
- 密码解锁成功后，在原账号/文件锁内检查并修复缺失或损坏的设备包装，使用同一 realKey。
- 密码/设备包装新增带 owner、type、version 的认证头，作为 AES-GCM AAD；兼容旧格式。密码成功解开
  旧包装后原子重包装，不更换 realKey。设备包装的读取不再创建 Keystore alias；明文要求恰为32字节。
- Success 增加 deviceWrapperReady/passwordUpgradePending，不把设备包装写入失败冒充持久化成功。
- 更正锁屏语义：setUserAuthenticationRequired(false) 不证明“锁屏时不可用”。
- 尚缺：独立 keyId 元数据、显式密码轮换 API、JNI→账号/UI 类型化错误贯通、真机锁屏矩阵和包装替换强杀。
  设备包装存在意味着本地可解锁，不替代服务端登录鉴权。

### F04：消息与媒体事务

- MessageMediaColumns 统一媒体字段绑定/读取，保留原图和大小缩略图全部 fileId、hash、尺寸、大小、路径。
- 媒体入库共用 mergeIncoming；消息、FTS、会话摘要、未读、水位、媒体引用一起提交。重复 msgId
  验证会话/peer/方向/类型/seq，不允许悄悄改消息身份。
- 缓存变体不再 INSERT OR REPLACE；迟到 pending 不会把 ready 变体清空。
- conversations 新增 last_msg_id；本地草稿使用展示时间置顶，不冒充服务端时间；回执仅更新所属摘要。
- 前台接收推进已读水位，后续旧已读重算不重新产生未读；水位镜像使用有效 readSeq。
- 新增会话历史 keyset 分页，按 server_time/conversation_seq/local_order/msg_id 完整排序键翻页；同毫秒消息
  不漏不重，翻页期间插入新消息不造成 OFFSET 漂移，返回 hasMore 与下一页游标。
- 新增 JTHP v1 JNI 长度前缀二进制协议及 JitongSdk.loadHistory；完整搬运消息身份、状态、拼音和原图/大小
  缩略图的 fileId/path/hash/尺寸/大小等字段。游标由 SDK 从页尾生成，UI 不自行拼装。
- 尚缺：会话列表/搜索定位 around-anchor 查询与完整 Golden；所有远端非空媒体字段冲突的统一诊断；迁移路径与消息路径
  全部映射去重；媒体文件的实际下载/缓存状态机仍属 F08。

### F07：Native 本地搜索（阶段完成）

- 新增 SearchService，通过 message_fts_identity 与 messages 联表，始终绑定 ownerId，并支持会话过滤和100条上限；
  全部参数使用 sqlite bind，不拼接用户输入。
- 单个英文字母只匹配拼音首字母串，`n` 可命中“你/年”，不会因为 `zhen/zheng` 内含 n 而误命中“真/正”；
  多字母支持正文、全拼和首字母子串。
- 正文直接命中返回 UTF-16 高亮范围；单字母/首字母串利用 initials 与原文可索引字符的一一对应关系映射
  回汉字范围，`n` 命中“你”可直接加粗；含 emoji 时已验证 Kotlin/Compose 下标不偏移。
- 新增版本化、小端、长度前缀的 JNI 二进制结果协议，避免正文换行或分隔符导致边界错乱；Kotlin 严格校验
  magic、version、长度、UTF-8、数量及高亮范围后才产生 SearchHit。
- JitongSdk 已开放 searchMessages。Android 14 arm64 真机链路验证 Room 导出、Native 加密库写入、JNI 搜索
  和结果解码，5/5 通过。
- 尚缺：UI 搜索页切到该 SDK；连续全拼命中目前只能返回消息，无法精确加粗对应汉字，因为现有索引只保存
  无音节边界的聚合 pinyin，没有“拼音 token → 原文 UTF-16 区间”映射；还未做 Legacy/Native 大样本 Golden
  与百万级性能。

### F06：文本消息生产接线（阶段完成）

- Runtime 复用句柄内唯一 `ClientCore`，不另建 Socket；新增结构化消息/回执桥，完整传递 msgId、会话 seq、
  服务端时间及媒体元数据，不再依赖旧的文本摘要回调重建业务消息。
- SDK 的文本发送先在 Native Writer 事务内提交消息、会话摘要、FTS 与持久 Outbox，再尝试把 Outbox 中原始
  protobuf 交给现有安全连接；断网时保持 SENDING，不提前谎报成功，重试沿用原 msgId。
- 回执按 `msgId + attempt` 更新 Outbox/消息状态，并在更新前核验消息 owner、方向和 peer，避免错误或迟到
  回执串改其他消息；文本按协议字节上限 fail-fast。
- Native 提供合并型消息失效版本和一次性 operation 终态消费接口。UI 收到版本后重新查询快照，不让网络/DB
  Writer 阻塞 Kotlin collector；操作在 ACK 到达前保持 pending。
- Runtime 新增自有 Outbox 泵线程，认证状态机事件直接控制发送门禁：只有 `Authenticated + Connected` 才能
  领取 Outbox；登录成功、Token 重认证成功和新消息提交都会唤醒，认证期间/断线/被踢/登出立即关闸。
  服务端临时错误按持久化 `next_retry_at` 精确定时唤醒；发出后仅在30秒租约到期仍无 ACK 时重试，空闲时不做
  一秒轮询，避免移动端无效唤醒。stop/logout/destroy 会唤醒并 join，不把定时重试放进 UI。
- `RoamConvRs/RoamMsgRs` 已从旧 UI 回调并行接入结构化消息桥；会话预览与历史批次均复用 Native Inbox 事务。
  依据协议中的 from/to 与当前 owner 判断方向，既支持“我发的历史”也支持“我收到的历史”；响应 peer 与消息
  身份不一致时拒绝，历史上下文不增加未读，重复批次继续由 msgId 幂等合并。
- SDK 新增会话漫游和按 `beforeSeq` 分页漫游意图接口，只有认证且连接完成才允许发包；响应不返回 UI 自行拼装，
  直接写入 Native 数据库后发布合并型失效版本。
- Runtime 已装配 `SyncService`，Push、ACK、离线补发和漫游返回的 seq 统一进入 Tracker。Tracker 首次访问会
  通过 `(owner, conversation, seq)` 索引扫描真实 messages，并压缩成已到达区间；因此重启后本地已有
  1、2、5 时恢复为 contiguous=2、gap=[3,4]，不会把 maxseen=5 错当成连续水位。
- 发现缺洞后使用现有 `beforeSeq` 漫游协议补齐；按会话 single-flight，一次只发一个请求，一轮最多5页、
  每页最多100条。返回 peer 身份不一致拒绝，空页/到末页仍有缺洞时持久化指数退避。
- 补洞请求现有独立5秒 deadline：响应缺失时释放该会话 single-flight，清理本轮页数，持久化失败次数并按
  `next_retry_at` 重新唤醒；退避状态在 Runtime 重启后也会从数据库恢复并重新挂定时器。未认证或 Socket
  断开时只保留 pending，不把“当前不可发送”误算成请求失败。stop/logout/destroy 会停止并 join 定时线程，
  不留下跨 generation 回调。
- Android 端当前临时复用既有 `PinyinIndex` 生成发送消息的派生搜索列。它不属于权威消息字段，也不代表拼音
  能力已经下沉 C++；远端消息的 Native 拼音生成仍待统一实现。
- Runtime 的数据失效和命令终态已由 C++ 主动推送到 JNI：失效事件携带 domain/dbVersion/generation/owner，
  在 SDK 合并成只读 `StateFlow<Map<DataDomain, Long>>`；命令终态进入无界可靠 Channel 暴露为 Flow。
  Kotlin 以 owner+generation 丢弃旧账号迟到事件，UI 不再需要轮询 `consumeInvalidation/consumeOperation`。
  原 consume API 暂时保留用于灰度兼容和诊断，不再是新 UI 的推荐路径。
- Native Repository 新增账号会话快照查询，按 `last_message_time, conversation_id` 稳定倒序并保持 owner
  隔离；JNI 使用 JTCL v1 长度前缀二进制返回，SDK 严格校验 magic/version/count/UTF-8/剩余字节。
  `NativeMessageController` 只把 Conversations/Messages 版本变化映射成会话及当前会话历史 StateFlow/Flow，
  不在 UI 维护 seq、ACK、重试或拼分页游标。发送、Push、漫游、ACK 现在同时发布 Messages 和
  Conversations 失效，修复“消息已变但会话摘要不刷新”的事件域遗漏。
- 已读意图也已下沉：`NativeMessageController → JitongSdk → JNI → ClientRuntime → MessageService → Repository`
  原子更新 read watermark/unread，完成后发布 Conversations 新版本并通过 operationId exactly-once 返回终态。
  由此 Native 文本 UI 所需的会话快照、当前页历史、发送、已读和完成通知已经形成闭环；控制器不持有
  数据库、网络或重试状态。
- 尚缺：真实双端在线 Push/ACK/离线/漫游 E2E、具体页面对 `dataVersions`/`operationEvents` 的订阅与快照重查，
  以及接收消息拼音索引的内核统一实现。deadline/退避已增加仅在 Native 测试目标开放的假传输入口，
  确定性模拟“请求被接受但服务端永不响应”，覆盖超时、释放 single-flight、持久化退避和再次请求；
  该用例仍不等同于真实网络 E2E。

### F05：恢复规则和文档

- 修订 Native Schema 实际版本为4，Room 当前仍8、目标9；不改写旧迁移。
- 新增纯函数 RecoveryResolver：两侧状态/epoch/owner 不一致、损坏、单侧缺失均 Repair；两侧都缺失
  但已有 Native 库也 Repair。任何一侧表明已提交，绝不自动 Legacy。
- 更正 ADR/功能矩阵中“NO_WRITE 不一致自动回 Legacy”“PREPARED 不一致继续 Legacy”的冲突描述。
- Resolver 尚未接入 Android 启动选择器。真实 journal/认证 KV、强杀和恢复页均需 F10；本次测试只证明
  决策函数，不能证明磁盘持久化或跨存储原子性。

## 实际测试

| 检查 | 结果 | 证据与范围 |
|---|---|---|
| 当前源码重新配置/编译 C++ | PASS | client_core/build-sanitize，Debug，C/C++ 均开启 address,undefined |
| CTest 全量 ASan + UBSan | **37/37 PASS**（35项沙箱内 + 2项授权环回复跑） | 原33项见 kernel-round7-fix-asan-final.log；新增 Runtime/Repository/Search 已纳入 |
| 十万条 Writer 投递 | PASS，所属用例89.55秒 | 上述全量测试内 db_command_queue；未降低条数 |
| 独立 TSAN 编译/运行 | **2/2 PASS** | kernel-round7-fix-tsan.log：client_runtime、message_service；不是全库TSAN验收 |
| Outbox 泵定向 TSAN | **1/1 PASS** | 2026-09-14 重新配置 build-round7-tsan 后运行 client_runtime；覆盖 start/stop/logout/destroy 并发与泵线程 join |
| Sync 接线定向 ASan/UBSan + TSAN | PASS | sync_tracker、sync_service、client_runtime；覆盖重启从真实 seq 区间恢复、gap 持久化、双向漫游幂等和有界请求状态 |
| 补洞无响应 ASan/UBSan + TSAN | PASS | client_runtime 使用 test-only 假传输吞掉响应；覆盖20ms测试 deadline、失败持久化、约1秒退避后第二次请求，以及 stop/destroy join；生产 deadline 仍为5秒 |
| Runtime 事件桥 ASan/UBSan + TSAN | PASS | client_runtime 验证 invalidation 元数据、operation exactly-once 主动通知及重复终结抑制；Android 双 ABI 编译通过；Pixel 7 Android 14 arm64 RuntimeFixTest **1/1 PASS**，包含 JNI 方法签名/GlobalRef 真实绑定 |
| 会话快照与薄消息适配 | PASS | native_repository 验证摘要排序/owner 隔离；Pixel 7 Android 14 arm64 ShadowMigrationTest **7/7 PASS**，覆盖 JTCL 解码、历史分页、消息/会话 StateFlow 主动版本更新与断网先落库 |
| 文本 UI Native 闭环 | PASS（API/状态映射） | ClientRuntime ASan/UBSan+TSAN 验证 markRead 原子提交、unread=0 与 operation 终态；Pixel 7 Android 14 arm64 ShadowMigrationTest+RuntimeFixTest **8/8 PASS**；现有 Compose 页面正式 cutover 仍受账号启动/迁移开关约束 |
| Android Debug / AndroidTest APK | PASS | Gradle assembleDebug/assembleDebugAndroidTest；包含 arm64-v8a 与 x86_64 Native 编译链接 |
| Android 14 arm64 AVD，CheckJNI=1 | **24/24 PASS** | kernel-round7-fix-checkjni.log；15解锁、1Runtime JNI、8生命周期 |
| Native Repository + Runtime + Search 定向 ASan/UBSan | PASS | 历史分页稳定性；漫游双向身份、错误 peer 拒绝、重复批次幂等；首字母、全拼、owner/会话隔离、emoji UTF-16 高亮 |
| Android 14 arm64 搜索/历史/离线发送 E2E | **7/7 PASS** | ShadowMigrationTest；Room→Native SQLCipher→JNI/SDK；两页游标、媒体字段、Runtime stop/restart、先落库后发、失效版本与 pending 终态 |
| git diff --check | PASS | 仅空白检查，不代表未跟踪文件和业务逻辑已全审查 |

新增确定性回归包括：旧Outbox保留后重开数据库、8个Service并发发号、protobuf载荷可恢复、租约不重复领取、
重复失败/迟到attempt/取消、协议结果0映射、媒体字段重启往返、前台未读重算、身份与seq冲突、Writer阻塞
期间destroy/start交错、容量预留、跨Runtime ID隔离、1/2/3→4迁移和迁移中段失败回滚、144组恢复组合。

Android新增覆盖：空密码/只解锁不建库、密码包装丢失、损坏设备包装修复、别名丢失不重建、旧密码包装升级
且realKey不变、跨账号搬移包装拒绝、重复Runtime创建不替换、换账号拒绝。Native .so 加载由 RuntimeFixTest
硬断言，不用 assume 跳过掩盖加载失败。

首次扩大回归的问题和处理：

1. 沙箱禁止回环 bind，使 integration/transport 失败；授权后重跑，最终全量通过。
2. 全局60秒超时短于十万条写入，改为300秒总上限，实测89.55秒；同时移除测试中的 detached producer，
   显式 join 后再核对完成计数。未更改生产写入条数或通过断言。
3. NativeLifecycleTest 缺少 MMKV 初始化，压力测试仍传空信任根；补齐初始化并使压力入口显式传入
   AppIdentityPins 配置，**没有放宽生产空信任根拒绝规则**。首次24项失败3项，修复后24项全通过。
4. 2026-09-13 回归时，多级链测试根证书因只生成2天而过期，导致正确链也被 OpenSSL 拒绝。将测试根证书
   有效期改为与叶证书一致的365天并重新生成；生产证书验证和 pinning 规则未放宽，transport 复跑通过。
5. SDK 历史测试首次因该数据库测试类未初始化 MMKV 而失败；补齐与 Application 启动一致的初始化后，
   7项 Room→Native→JNI/SDK 用例全通过，未绕开正式 JitongSdk。

## 复现

```sh
cmake -S client_core -B client_core/build-sanitize
cmake --build client_core/build-sanitize -j 4
ctest --test-dir client_core/build-sanitize --output-on-failure --timeout 300
ctest --test-dir client_core/build-round7-tsan --output-on-failure -R '^(client_runtime|message_service)$'
```

上述构建目录缓存包含 sanitizer 参数；在全新目录复现必须显式指定 CMAKE_C_FLAGS/CMAKE_CXX_FLAGS
与 CLIENT_CORE_WITH_SQLCIPHER=ON。ASan/UBSan 与 TSAN 使用不同目录，禁止同时启用二者。
Android测试入口：`com.jitong.im.core.DbKeyManagerFailCloseTest,com.jitong.im.core.RuntimeFixTest,com.jitong.im.core.NativeLifecycleTest`。

## 剩余验收边界

F06、F08～F10 未因本次测试通过而完成：文本消息的持久发送、自动重连、现有漫游、有界缺洞拉取及请求超时唤醒已接线，可控无响应传输测试已通过，但尚欠真实在线双端闭环；账号/好友/AI 的生产 SDK 接线、Native媒体、Room v9 change
log/LegacyWriteGateway、snapshot+delta、一写源切换和薄UI仍须逐项实现。F07 还欠 UI 接线、连续全拼原文位置映射、
搜索结果 around-anchor 定位、Legacy/Native 大样本差分与性能证据。
F11 仍缺真实两端网络E2E、迁移/cutover强杀、两小时长稳、Release APK冒烟、搜索性能及媒体流量/色彩评估。
本次 x86_64 仅编译，不是运行；未执行 Android ASan/HWASan；桌面ASan不等同Android内存安全验证。

## 2026-09-15：文本 UI Native 适配器收口

- `NativeMessageController` 已改为页面只观察 `ConversationPage` 快照并提交发送、已读、翻页意图；首页和
  后续页都使用 Native 返回的 opaque cursor，合并按 `msgId` 去重并依据内核顺序键稳定排序。
- 消息/会话 `dbVersion` 变化会在 IO dispatcher 重新读取 Native 快照；离开会话会取消订阅，避免页面反复
  进入后在 ViewModel 生命周期内积累 collector。
- 修复 operation completion 的并发消费缺陷：SDK 的可靠 Channel 现在只由控制器统一消费并按 operationId
  暂存，页面在 ACK 已经返回后再订阅也不会丢结果；不同按钮不再通过多个 `filter` collector 互相抢事件。
- Android 14 arm64 AVD 新增真实链路用例：Room fixture → Native SQLCipher → JitongSdk →
  NativeMessageController，验证会话首屏、历史稳定排序、Native 断网先落库发送以及失效通知驱动页面刷新。
  `ShadowMigrationTest` **8/8 PASS**；Debug 和 AndroidTest Kotlin 编译通过。
- 生产 Backend 仍未打开：账号、好友和文本必须在同一次 cutover 中共享一条 Native Socket。当前若只把
  Compose 的 `send()` 单点改到 Native，会与仍服务好友页的 Kotlin `ImClient` 形成双连接，因此禁止这种
  伪切换。下一步必须先补齐 Native 好友 SDK/UI 入口，再执行账号+好友+文本的原子页面切换。

## 2026-09-15：Native 好友快照接线（全量切换前置）

- `ClientRuntime` 正式装配 `FriendService`，与 Message/Sync 共用同一个账号 Repository；stop/logout/destroy
  同步解除服务，查询在 Runtime 锁内只复制 `shared_ptr`，数据库读取在锁外执行。
- 新增好友和好友申请的版本化长度前缀 JNI 协议（`JTFD/JTFR v1`），Kotlin 对 magic、version、数量、
  UTF-8、枚举范围和剩余字节执行 fail-close 校验；不暴露 SQL、数据库句柄或 C++ 指针。
- `JitongSdk.loadFriends/loadFriendRequests` 已可提供 Native 快照，Presence 由 FriendService 合并到返回值，
  不污染持久化好友事实。
- Desktop ASan/UBSan `client_runtime` **1/1 PASS**；Android arm64+x86_64 Debug/AndroidTest APK 构建通过；
  Pixel 7 Android 14 上 `sdkHistory_decodesStableCursorAndAllMediaFields` **1/1 PASS**，并实际调用两个新增 JNI。
- 已继续补齐生产协议桥：`ClientCore` 新增弱引用 `IFriendProtocolSink`，FRIEND_INFO、ADD_FRIEND_RQ、
  FRIEND_OFFLINE、FRIEND_REQUEST_LIST_RS 和 DELETE_FRIEND_RS 统一进入 Runtime；此前虽在 proto/协议号中
  存在但完全未注册的 1033/1035 响应现已注册分发。申请列表、添加、同意/拒绝、删除意图均复用同一安全连接。
- Runtime 把好友资料/申请幂等落库，Presence 只留内存，删除只在成功回执后修改事实并发布 Friends 版本；
  `NativeFriendController` 只订阅快照和表达 refresh/add/answer/delete 意图。
- 新增回归覆盖 FriendInfo、Presence 下线、申请重复去重、删除成功及 Friends invalidation；Desktop
  ASan/UBSan `client_runtime` **1/1 PASS**，Android arm64/x86_64 Debug 构建和 AndroidTest 编译通过。
- 剩余好友收口：Compose 的现有 `Friend`/`FriendRequestItem` 展示模型需要切到 `NativeFriendController`，并补
  真实服务端添加/同意/删除双端 E2E；该 UI 切换必须与 Native 账号启动和文本页一起原子启用。

## 2026-09-15：账号态与 Runtime 晚绑定修复

- 修复创建顺序竞态：密码/Token 登录可能先于账号库解锁和 `ClientRuntime` 创建完成，而账号事件不重放；
  旧实现会令后创建的 Runtime 永久保持“未认证”，持久 Outbox 即使连接正常也无法发送。
- `nativeCreateRuntime` 挂载同一个 `ClientCore` 后，会在句柄锁外读取当前 `AccountSession` 状态和安全连接
  状态，向 Runtime 补齐发送资格。实时账号事件仍走既有 observer，因此“先登录后建 Runtime”和“先建
  Runtime 后登录”结果一致。
- 句柄锁内只挂载对象并复制 `shared_ptr`，账号状态查询和 Outbox 唤醒均在锁外，避免回调重入扩大临界区。
- Android Debug 双 ABI Native 编译、APK 组装及 Debug AndroidTest Kotlin 编译通过；Pixel 7 Android 14
  上 `RuntimeFixTest + ShadowMigrationTest` **9/9 PASS**。

## 2026-09-15：Native 账号启动编排与换号生命周期

- 新增 `NativeKernelHost` 作为 Android 平台宿主：UI 只提交 setup、密码/Token 登录、登出和取消意图；登录
  成功后由宿主在 IO dispatcher 串行完成账号 SQLCipher 解锁、同一 `ClientCore` 的 Runtime 创建/启动、
  消息与好友薄控制器发布以及首轮漫游/好友同步。
- 密码原文只进入 Native 认证调用；Kotlin 仅在 operation 在途期间保留 SHA-256 摘要用于本地库包装密钥，
  开库后立即清除。Token 冷启动不伪造空密码，严格走设备包装密钥，失败即关闭整个 Native 句柄。
- 启动任一步失败都执行 fail-close，不向页面发布半初始化控制器。账号事件订阅用 `UNDISPATCHED` 建立，
  随后在 IO dispatcher 处理，既关闭极快回执的订阅窗口，也不在主线程执行数据库初始化。
- 新增 `nativeDestroyRuntime`：锁内仅从句柄摘除 `shared_ptr`，锁外 drain 线程并清协议 sink；认证句柄和唯一
  `ClientCore` 保留，因此登出后可创建不同 owner 的 Runtime，不需要第二条 Socket。
- Android 双 ABI Debug/AndroidTest 编译通过；Pixel 7 Android 14 新增换号生命周期回归，
  `RuntimeFixTest` **2/2 PASS**。

## 2026-09-15：Room v9 在线增量捕获（cutover 第一阶段）

- Room schema 升至 9，新增 `legacy_change_log` 与 `(ownerId, changeSeq)` 索引；消息、会话的 INSERT、
  UPDATE、DELETE 由数据库触发器写全局单调日志，DELETE 保留 tombstone。除 Kotlin gateway 外再加数据库
  级兜底，因此旧 DAO writer 或原始 SQL 也不能静默绕过捕获。
- 触发器同时由 `MIGRATION_8_9` 和新库 `onCreate/onOpen` 安装，避免“升级库有、全新安装没有”的分叉。
- `MigrationExporter.nextChanges` 支持按 owner/checkpoint 有界分页并返回 high-water；明确全局序号允许被其它
  owner 穿插，只校验严格递增，不能误用 conversation seq 或错误要求同 owner 序号连续加一。
- Pixel 7 Android 14 `LegacyChangeLogTest` **1/1 PASS**：覆盖消息/会话增改删、tombstone、high-water、分页
  导出与确认后 GC；Kotlin/AndroidTest 编译通过。
- 尚未宣告 cutover 完成：下一阶段仍需把 delta UPSERT/DELETE 应用到 Native 单 Writer、checkpoint 同事务
  提交，并实现 DB journal + 带 MAC KV 镜像和启动恢复选择器；完成前 Native 默认开关保持关闭。

## 2026-09-16：Room v10 strict delta → Native Writer 原子闭环

- Room schema 从 v9 升至 v10，新增 `MIGRATION_9_10`，不改写既有 8→9。`legacy_change_log` 新增
  `payloadVersion/payload`；v10 触发器在消息/会话权威写事务内固化完整 JSON 快照，DELETE 只保留空 payload
  tombstone。升级时清理无法严格重放的 v9 无快照日志，后续由全量 snapshot 建立新基线；新库和旧库升级均
  drop/recreate 同一套 v10 触发器。
- 修复真实 Room schema 的 snapshot 查询：`MessageEntity` 没有 `localOrder` 列，现改用自增 `id AS localOrder`，
  不再依赖测试夹具中的伪列。
- `MigrationExporter` 新增 JTDL v1 小端长度前缀编码，只编码触发器已固化的历史 payload，禁止导出时回读
  当前业务表。正文消息写 FTS 后在同一 Room 事务 touch 一次消息，使最终 UPSERT 快照包含 pinyin/initials。
- Native 新增 `LegacyDeltaChange`、`MigrationImporter::applyLegacyDeltaBatch` 与
  `NativeDatabase::submitLegacyDeltaBatch`。入口只在 `ShadowImport` 状态、owner 匹配时开放；同 owner 页内
  `changeSeq` 必须严格递增，允许其他 owner 穿插全局序号。UPSERT 全字段覆盖消息/会话并重建 FTS；DELETE
  清消息、FTS identity/FTS、Outbox、媒体引用并重算会话摘要；会话 DELETE 不隐式删除历史消息。
- 数据变更和 `migration_checkpoint(epoch,'legacy_delta')` 只通过 Native 单 Writer，在同一个
  `BEGIN IMMEDIATE/COMMIT` 事务提交；坏 payload、owner 混入、倒序、跨 checkpoint 批次任一失败时全部回滚。
  整批已经落在 committed checkpoint 之前的重放幂等返回当前水位。
- JNI 新增 `nativeSubmitLegacyDeltaBatch` 和 `nativeGetLegacyDeltaCheckpoint`，严格校验 magic/version/count、
  单字段 1MiB、整批 8MiB、尾随字节；异常不越过 JNI 边界。Writer 超时返回 `TimedOutButMayCommit`。
- Kotlin 新增 `LegacyDeltaApplier`：冷启动以 Native 持久 checkpoint 为恢复事实源；只有明确收到
  `ok|checkpoint=N` 后才 GC Room change log，超时/确定失败均保留 tombstone。空页只有在
  `highWater <= checkpoint` 时才算追平，关闭页查询与 high-water 查询竞态造成的误切窗口。
- Desktop ASan/UBSan 新增 `legacy_delta`：消息/会话 UPSERT/UPDATE/DELETE、FTS、checkpoint、幂等重放、
  坏 payload、owner 混入、倒序与跨水位拒绝均通过；全量 CTest **38/38 PASS**。
- Android arm64+x86_64 Debug 与 AndroidTest APK 构建通过；Pixel 7 Android 14 上
  `LegacyChangeLogTest + ShadowMigrationTest` **10/10 PASS**，覆盖 v10 快照/tombstone、JTDL 编码、
  JNI→Native Writer 原子提交、搜索可见、明确成功后 GC 及重复批次幂等。
- 当前仍不宣告 cutover 完成：DB cutover journal、带 MAC 的 KV 镜像、冷启动恢复选择器和 Compose 原子切换
  尚未实现；Native 默认开关继续关闭。下一步进入步骤 2（journal + KV 镜像 + selector）。

## 2026-09-16：Cutover journal、认证镜像与恢复选择器

- `NativeDatabase` 新增最新 journal 查询和严格单向推进：`Missing→LEGACY_ACTIVE→PREPARED→NO_WRITE→DIRTY`；
  拒绝跳级、回退、换 epoch，以及同状态但内容不一致的伪幂等重放。写入只走 Native 单 Writer。
- JNI/Kotlin 新增 journal 查询/推进接口；Android E2E 已实际推进到 PREPARED、解析证据并验证跳过
  NO_WRITE 被拒绝。Desktop ASan/UBSan `legacy_delta` 覆盖完整状态链和负向路径，**1/1 PASS**。
- 新增 `CutoverMirrorStore`：Android Keystore 生成不可导出的 HmacSHA256 key，MAC 绑定 owner、epoch、state、
  high-water、schema、keyId、summary、updatedAt；持久化使用同步 `commit()`。篡改任一字段返回 Corrupt。
- 新增 `CutoverRecoverySelector`，语义与 C++ `RecoveryResolver` 对齐：双缺失且无 Native 库才允许 Legacy；
  PREPARED 一致可继续 Legacy；NO_WRITE/DIRTY 一致且 Native 可加载才选 Native；缺失、损坏、owner/epoch/
  state 分歧一律 Repair，DIRTY 永不回 Room。
- Pixel 7 Android 14 `CutoverMirrorStoreTest` **1/1 PASS**；包含真实 Keystore HMAC、防篡改和 Repair 断言。
- 仍待步骤 4 收口：snapshot 前建立 delta baseline、最终 Room 停写闸门、snapshot+delta 总协调器、两侧
  journal 写入次序的强杀测试，以及进程启动处冻结 `KernelBackendSelector`。默认开关仍关闭。

## 2026-09-16：Snapshot delta baseline 缺口修复

- 新增 `MigrationImporter/NativeDatabase.seedLegacyDeltaCheckpoint`：只在 `shadow_import`、owner 匹配且 epoch
  尚无 `legacy_delta` checkpoint 时首次写 baseline；相同 baseline 重试幂等，不同 baseline 明确拒绝。
- baseline 与状态校验只走 Native 单 Writer 的同一事务，JNI 返回明确 committed checkpoint；超时按结果未知
  处理，调用方可用相同参数重放，不能自行前移水位。
- JitongSdk 已提供 seed/get/submit delta 及 journal 查询/推进门面，后续总协调器不需要暴露 native handle。
- Desktop ASan/UBSan `legacy_delta` **1/1 PASS**；Pixel 7 Android 14 `ShadowMigrationTest` **9/9 PASS**，
  覆盖 baseline 首写、相同重放、不同值拒绝以及后续 delta/JNI/journal 完整链路。
- 步骤 4 当前只剩最终停写闸门与总协调器。不能仅靠 `SupportSQLiteDatabase.beginTransaction()` 长时间锁库：
  必须先建立可持久恢复的一写源 fence，防止 NO_WRITE 后 Legacy writer 在事务释放瞬间再次写入。

## 2026-09-16：步骤 1–4 完整收口（baseline / journal / selector / coordinator）

- Room schema 升至 v11，新增按 owner 持久化的 `legacy_cutover_control`。数据库级 `BEFORE INSERT/UPDATE/DELETE`
  触发器在 fence 开启后直接 `RAISE(ABORT, 'legacy_write_frozen')`，不依赖 Kotlin DAO 自觉遵守；不同
  owner 不受影响。该闸门在进程被杀后仍存在，避免 PREPARED/NO_WRITE 窗口重新发生 Legacy 写。
- 新增 `CutoverMigrationCoordinator`，严格按“记录 LEGACY 双证据 → 固定 delta baseline → 分页 snapshot →
  在线 delta 追平 → 持久停写 → 最终 delta 追平 → Native 摘要校验 → PREPARED DB/镜像 →
  NO_WRITE DB/镜像”的顺序执行。只有 Native 明确提交 checkpoint 后才 GC Room change log。
- 修复协调器可重试语义：PREPARED 之前失败会撤销 fence；下次以同 epoch 续跑时，必须校验
  Native journal 与 HMAC 镜像的 state/high-water/schema/keyId/summary/updatedAt 全部一致，并沿用
  首次 baseline，不会跳过失败期间新产生的 Legacy 写。任一证据不一致立即 Repair。
- `CutoverRecoverySelector` 作为恢复决策纯函数：双缺失且 Native 库不存在才允许 Legacy；一致
  PREPARED 保持 Legacy 以完成切换；一致 NO_WRITE/DIRTY 且 Native 可打开才选 Native；单边缺失、MAC
  损坏、元数据分歧或 Native 不可用都进 Repair，绝不静默回退。最终生效后要求重启进程，
  `KernelBackendSelector` 仍保持进程内只初始化一次、禁止热切换。
- Android Pixel 7 / API 34：`ShadowMigrationTest` **10/10 PASS**，包含真实 Room snapshot/delta、JNI、Native
  SQLCipher、journal、HMAC 镜像、NO_WRITE 选择和切换后 Legacy 写阻断；`LegacyChangeLogTest` **1/1 PASS**；
  `CutoverMirrorStoreTest` **1/1 PASS**；三组联合运行 **12/12 PASS**。Debug Kotlin 与 AndroidTest Kotlin 全量重编通过。
- Desktop ASan+UBSan 全量 **38/38 PASS**。其中 `integration/transport` 需绑定回环端口，首次在受限
  sandbox 中被系统拒绝，放开本地端口权限后 **2/2 PASS**；这不是代码或 sanitizer 失败。

## 2026-09-16：正式冷启动接线与 Native 文本薄 UI

- 新增 `JitongApplication`：Activity 创建前初始化 MMKV，根据认证镜像确定 owner，用设备包装密钥
  尝试打开 Native SQLCipher 库，联合 DB journal、HMAC 镜像、库存在性和 Native 自检做唯一的
  `Legacy / Native / Repair` 决策，再一次性冻结 `KernelBackendSelector`。
- 补齐 PREPARED 强杀恢复：该状态下 Room 已停写，禁止启动普通 Legacy UI。双证据一致且
  Native 自检通过时，冷启动幂等补完 `PREPARED→NO_WRITE`；DB 或镜像任一提交失败都
  fail-close 进 Repair，不能回到旧 writer。
- `MainActivity` 改为互斥入口：只有 Legacy 决策才实例化 `MainViewModel/ImClient/Room`；Native 决策
  只创建 `NativeKernelHost` 和 Native 薄 UI，杜绝“选了 Native 又暗中启动 Legacy”的双连接/双写。
- 新增 `NativeAppRoot`：登录、Token 恢复、会话列表、历史文本分页、文本发送和退出均只通过
  `NativeKernelHost/NativeMessageController` 表达意图；UI 不解析游标、不管 ACK/重试、不直连数据库。
- Android Debug/AndroidTest 全量编译通过；Pixel 7 / API 34 上迁移、HMAC、Runtime 回归 **13/13 PASS**，
  包含停写闸门的联合回归总计 **14/14 PASS**；Debug APK 冷启动成功，`MainActivity` 进程稳定存活。
- 本次完成的是启动互斥与文本主链路；图片/文件、好友申请完整交互和与旧 Compose
  视觉等价的页面仍需后续在 Native 分支补齐，不允许为了复用旧页面而重启 `MainViewModel`。

## 2026-09-16：DIRTY transaction hook 与 KV 镜像同步闭环

- `NativeDatabase` 新增业务写唯一入口 `submitBusinessSync`：journal=NO_WRITE 时，首次业务事务在
  **同一事务、业务 SQL 之前**原子推进 DIRTY；PREPARED/LEGACY_ACTIVE 状态的业务写直接 fail-close 拒绝；
  业务事务失败整体回滚，不会误推进 DIRTY。`NativeRepository::runTx` 已全部改走该入口；迁移/import/
  journal 管理仍走 `submitSync`，不会在 cutover 前误标脏。
- 新增 `setCutoverDirtyListener`：仅在本次事务真正把 NO_WRITE→DIRTY 提交成功后，于调用方线程触发一次，
  携带提交后的完整 journal 快照（含 updatedAt）；已是 DIRTY 的后续业务写不重复通知。监听器失败不影响
  DB 已提交事实——强杀窗口由冷启动双证据校验 fail-close 为 Repair，绝不回退 Room。
- JNI 新增 DIRTY 镜像桥：`nativeOpenAccountDatabase` 在 Java 线程缓存 `NativeBindings` 全局类/方法引用，
  监听器经 `AttachCurrentThread` 按需附着后回调 Kotlin（覆盖内核网络线程驱动的来消息落库场景）。
- Kotlin 新增 `NativeBindings.onCutoverDirtyFromNative` + `cutoverDirtyHandler` 委托；
  `JitongApplication.onCreate` 注入 handler，把证据写入 `CutoverMirrorStore`（Keystore HMAC）。
  镜像字段与 DB journal 完全一致（state/highWater/schema/keyId/summary/updatedAt），
  `CutoverRecoverySelector.resolve` 双证据一致 → Native，不再因镜像滞后误进 Repair。
- Desktop ASan/UBSan `legacy_delta` 新增 4 条断言（监听器一次性触发、快照一致、重复业务写不重复通知、
  失败事务不触发），全量 **38/38 PASS**。
- Pixel 7 Android 14 新增 `CutoverDirtyMirrorTest` **1/1 PASS**：PREPARED 期业务写被拒且不写镜像；
  NO_WRITE 后首次 sendText 原子推进 DIRTY、镜像同步、双证据 resolve=Native、第二次写不重复推进。
  cutover 相关联合回归（DirtyMirror + ShadowMigration + LegacyChangeLog + RuntimeFix）**15/15 PASS**；
  Android arm64/x86_64 Debug 与 AndroidTest APK 构建通过。
- 剩余：Compose 薄 UI 完整切换（图片/文件/好友申请交互）、静态依赖门禁（禁止页面直连 Room/ImClient）、
  LegacyWriteGateway 静态 writer 枚举、真实服务端 E2E（含强杀矩阵）。Native 默认开关仍由冷启动
  双证据决策驱动，未强制全量开启。

## 2026-09-16：Native 薄 UI 补全与静态依赖门禁

- 新增 `NativeServerConfig`（core 包）：Native 分支的服务端地址/端口常量，替代对
  `ImClient.DEFAULT_HOST`/`Protocol.TCP_PORT` 的常量级依赖；新增本地单测 `NativeServerConfigTest`
  守护两处常量与 Legacy 不漂移。
- 密码哈希工具 `sha256Hex/sha256HexOfStream` 从 `net` 包迁至 `util` 包：它是纯密码学原语，
  Native 分支（NativeKernelHost passHash）也需要，但不允许依赖 Legacy 网络包。全量引用点
  （NativeKernelHost/MainViewModel/ImClient/HttpMediaClient/ProtocolCodecTest）已同步。
- Native 聊天页补齐两项交互：「漫游历史」按钮（beforeSeq 由 NativeMessageController 从当前最早
  已分配 seq 的消息推导，UI 不接触游标语义；结果经 dataVersions 自动刷新）；发送失败反馈
  （本地落库失败立即提示；已提交消息经 `awaitOperation` 等待网络终态，失败/超时提示
  「已保存可重发」，消息本体不丢）。
- 新增 Gradle 静态依赖门禁 `verifyNativeUiDependencies`（挂入 `preBuild`，任何构建都会被拦截）：
  1. 所有 `Native*.kt` 禁止 import `data.db`/`data.ChatStore`/`net.*`/`MainViewModel`；
  2. `data.db`（Room writer）import 白名单：`data/ChatStore.kt`、`ui/MainViewModel.kt`（data/db 自身除外）；
  3. `net.*`（Legacy 网络入口）import 白名单：`ui/MainViewModel.kt`、`ui/FriendListScreen.kt`。
  新增违规引用直接编译失败，必须显式改白名单并说明理由。门禁首次运行即抓到并修复了
  NativeKernelHost → net.sha256Hex 的真实违规。
- 验证：门禁 PASS（扫描全量 Kotlin 文件）；`:app:testDebugUnitTest` PASS（含新增常量一致性单测）；
  Android 双 ABI Debug/AndroidTest 构建通过；Pixel 7 Android 14 cutover 联合回归
  （DirtyMirror + ShadowMigration + LegacyChangeLog + RuntimeFix）**15/15 PASS**。
- 剩余：Native 注册（需 C++ AccountSession 增加注册编排，属账号域扩展，单独一轮）；
  真实服务端 E2E（含强杀矩阵）；媒体/AI Native 生产接线的剩余部分。

## 2026-09-17：Native 一次性注册链路

- 定位澄清：注册是**先于认证的一次性请求/响应**，不进入 AccountSession 会话生命周期，
  不需要扩展 AuthStateMachine；复用句柄唯一 ClientCore 连接（薄 UI 厚内核边界不破）。
- `NativeSdkHandle` 在 `nativeAccountSetup` 时记录服务端 ip/port；新增 JNI
  `nativeAccountRegister`：仅账号空闲（无会话或 LoggedOut/LoggedOutKicked）可发起，
  避免与 AccountSession 的 connect/登录并发竞争同一连接；连接（TLS+应用层握手）与
  `ClientCore::sendRegister`（内部 SHA-256，不明文上链路）在后台线程执行；
  本地连接失败经 `NativeEventSink.onRegisterResult(0)` 上抛（0 非协议结果码）。
- Kotlin 链路：`NativeBindings.nativeAccountRegister` → `JitongSdk.register` →
  `NativeKernelHost.register` + `registerResults` 事件流（host 订阅 sdk.events 的
  RegisterResult，与账号事件并行）。
- Native 登录页补齐注册页签（昵称/手机号/密码）：1=成功切回登录页，2=昵称占用，
  3=手机号已注册，0=连接失败；本地拒绝（账号忙/参数非法）立即提示。
- 静态依赖门禁、双 ABI 构建通过；Pixel 7 Android 14 新增 `NativeRegisterTest`
  **2/2 PASS**（空参数本地拒绝；无服务端时连接失败正确上抛 result=0 全链路）。
  cutover 联合回归 **15/15 PASS**。
- 真实注册往返（result=1/2/3 服务端响应）依赖可达服务端，归入真实服务端 E2E 范围。
- 剩余：真实服务端 E2E（含强杀矩阵与注册/登录/消息/漫游全链路）；媒体/AI Native
  生产接线的剩余部分（缩略图管线已接入；秒传 proof 在本节之后补齐，仍需真实流量验证）。

## 2026-09-17：Native 媒体秒传与 Range 续传

- `ClientCore::uploadMedia` 与服务端 `/api/v1/upload/preflight`、`/api/v1/upload/proof/{challenge_id}` 对齐：
  流式计算 SHA-256，命中时校验随机片段范围并提交至多 64 KiB 持有证明；未命中时整文件流式上传。
  两条路径均核对服务端回执摘要，并限制返回的 `file_id` 字符集。
- `ClientCore::downloadMedia` 对已有 `.part` 附带 `Range` 与 `If-Range`；仅在 `206` 且
  `Content-Range` 起点匹配时追加。服务端返回 `200` 时从头覆盖。网络失败保留部分文件，
  `NativeMediaController` 在下载成功但 SHA-256 校验失败时清除坏文件。
- 验证：Android `:app:assembleDebug` 双 ABI 构建通过；Desktop ASan/UBSan ClientCore 构建通过；
  CTest 36/38 在沙箱内通过，另外两项因沙箱禁止 TCP `bind`，经授权在沙箱外重跑 2/2 通过。
- 尚未验证：与真实文件服务器的命中/未命中秒传和中断恢复端到端流量、取消发送后的
  后台任务停止、媒体授权与服务端持久化的一致性。当前不能把本节称为生产 E2E 验收完成。

## 2026-09-17：媒体真实服务端回环 E2E 补证

- 扩展 `im_server/tests/test_e2e.cpp`：进程内启动真实 TLS TCP/HTTPS 文件服务，真实
  `ClientCore` 注册/登录并以好友身份上传媒体；首传为 preflight 未命中后整文件上传，
  消息落库建立摘要索引；二次上传同一字节以普通文件 MIME 发起，断言返回新 `file_id`、
  归属仍是原发送方与接收方、物理路径与首次消息一致。该跨 MIME 路径断言能排除
  “再次整文件上传但服务端图片内容寻址去重”造成的假阳性，证明走了 proof 命中路径。
- 接收者完整下载与预置 1000 字节 `.part` 的续传均逐字节比对；另用故意损坏的
  1000 字节前缀作为 Range 探针，验证下载后坏前缀保留、正确尾部追加，从结果上
  区分 `206` 续传与 `200` 覆盖。第三方持有 `file_id` 仍不可下载，非法 ID 本地拒绝。
- `cmake --build im_server/build-sanitize --target test_e2e -j4` 通过；授权本机回环监听后
  `ctest --test-dir im_server/build-sanitize -R '^e2e$' --output-on-failure` **1/1 PASS**。
- 边界：这验证的是本机真实服务端协议往返，不是 Android 真机弱网、强杀和取消任务
  恢复；也未测量实际线上带宽或服务端磁盘节省。上述项目继续保持未验收。

## 2026-09-17：Native 媒体协作式取消（阶段性）

- ClientCore 媒体 API 增加取消谓词：流式 SHA-256、秒传 proof 前、上传 provider/
  进度回调、下载响应/字节回调均检查取消；取消后不返回成功的 `file_id` 或下载成功态。
- 每个 Android 媒体任务通过 JNI 注册独立 `operationId → atomic flag`；UI 的取消按钮先
  置 Native flag，再取消 Kotlin Job。句柄销毁时统一置位并清表，已进入 JNI 的调用持
  `shared_ptr` 保证 flag 生命周期。图片的三次上传共用同一任务 flag，每次上传后检查
  Job，取消后不会继续到消息卡片提交；文件 SAF 拷贝按 64 KiB 检查取消，失败清理
  未完成的本地副本。
- 同一消息的预览/原图下载在 Kotlin 控制器中串行，避免同时写同一 `.part`；关闭原图
  弹窗可取消当前下载。下载取消保留 `.part` 供下次请求 Range 续传；重启后若 `.part`
  已经通过期望 SHA-256 则直接提升为完整文件。偏移超界收到 `416` 时删除坏 part
  并有界从头重试一次，防止每次访问都卡在同一个无效偏移。
- 验证：Android 双 ABI Debug/AndroidTest APK 构建通过；Pixel 7 Android 14
  `RuntimeFixTest` 3/3 PASS（含新增 JNI 任务 id 独立性、结束与销毁后安全调用）；
  服务端回环 `test_e2e` 1/1 PASS（含取消前置拒绝、流式哈希中途取消、秒传、Range
  与超界 `416` 恢复）。
- **尚未完成**：跨进程持久化的媒体 `transfer_tasks` 恢复调度、真实弱网下“传到一半
  点击取消”的真机验证、服务端孤儿上传清理/有效期、上传任务的可重试状态机。
  因此本节是协作式取消的阶段验收，不代表完整 P7 媒体任务闭环。

## 2026-09-17：下载任务 SQLCipher 持久化与冷启动恢复（阶段性）

- 复用现有 `transfer_tasks`（当前 Native Schema 实际最新为 v6，不新增迁移）：
  `NativeRepository` 通过业务单 Writer 事务创建下载任务、记录 offset/失败、提交完成/取消终态；
  `generation` 拒绝旧任务迟到完成，新任务领取同一 `file_id` 时原子终结旧任务。
  创建前必须由同账号消息引用该 `file_id`，且对应 SHA-256 为 64 位十六进制；
  恢复快照里的摘要/总大小从消息权威字段关联读取，不信任任务行自报值。
- JNI/SDK 仅暴露受限的创建、终结、恢复快照 API；恢复列表采用长度前缀二进制并严格解码。
  Android 的图片/文件下载在请求前写任务，网络失败保留 `.part` 与可恢复状态，摘要校验和
  rename 后才提交完成；取消写终态，冷启动不重新领取已取消任务。
- `NativeMediaController` 启动时核对恢复路径必须是本账号 app `native_media` 下该
  `file_id` 对应的 `.part`，优先提升已有完整文件；至多主动续传 2 个已有部分文件，
  其他任务由消息可见时按需领取；不存在部分文件或路径不合法的旧任务标记为
  `abandoned` 终态，避免每次启动反复扫描。同一消息的恢复与 UI 请求串行。
- 补充异常收口：前台下载抛异常时持久化失败态并反馈 UI；冷启动恢复的单个下载抛异常时
  只标记该任务失败，不让异常直接中断其余任务扫描；协程取消仍写入取消终态。
- 验证：Desktop ASan/UBSan `native_repository` 1/1 PASS（含关库重开、错误 file_id、
  新 generation 接管、旧完成拒绝、取消后恢复列表为空）；Android 双 ABI Debug/
  AndroidTest 构建通过；Pixel 7 Android 14 媒体草稿+下载任务 JNI 用例 1/1 PASS。
- **未完成**：上传草稿的持久化重试与消息卡片幂等提交（服务端当前只有整文件上传/秒传，
  没有分片上传协议）；真实弱网强杀后自动续传的设备级验证、下载任务超量排队/退避、
  服务端孤儿上传清理。因此仍不能把 P7 媒体任务整体标为 PASS。

> 后续状态更正：上段“没有分片上传协议/上传草稿未完成”是当时阶段事实，已被后续
> [媒体断点续传验收](media-resumable-upload-acceptance.md)覆盖；当前剩余项以该报告
> 第五、六节及最新代码为准，不能把旧阶段描述当作当前缺口。

## 2026-09-17：Cutover 最终停写前静默门禁（未启用生产切换）

- `CutoverMigrationCoordinator.execute` 现在要求显式 `LegacyQuiescence` 适配器；
  在线 snapshot 和 delta 首轮追平后，只有旧网络事件入口停止、在飞 Room/媒体写入排空
  并返回 true，才允许设置 Room 最终停写闸门。返回 false 保持可重试且不进入 fence。
- 这是必要前置条件，不是生产迁移入口：目前 `ImClient.disconnect()` 只关闭连接并取消
  心跳，不 join readLoop；`MainViewModel` 中还有分散的写库协程。尚无可证明完成的
  stop-and-drain 实现，因此没有为 UI 添加可点击的不可逆切换按钮，也没有运行真实账号迁移。
- Android `compileDebugKotlin`、`testDebugUnitTest` 通过；强制全量重编后 Pixel 7
  Android 14 的 `cutoverCoordinator_freezesLegacyAndCommitsNoWrite` 1/1 PASS。
  该用例显式提供“无 Legacy Socket/后台 writer”的测试静默证明；它不能替代生产
  stop-and-drain 或真实账号强杀端到端验收。

## 2026-09-18：Legacy stop-and-drain 的网络半程

- `ImClient.freezeNetworkForCutover()` 单向冻结后拒绝新连接；关闭现有 Socket，等待
  正在执行的握手归零，再次关闭竞态建立的连接，并 join 读循环与心跳。超时返回 false，
  不给迁移协调器伪造静默成功。
- Pixel 7 Android 14 `LegacyCutoverNetworkTest` 1/1 PASS：冻结后本地拒绝 connect。
- **未闭环**：MainViewModel 分散的异步 Room/媒体写任务尚无统一 drain 与拒新入口；
  因此该网络接口不得单独作为 `LegacyQuiescence.stopAndDrain()` 的 true 证明，生产
  cutover 入口仍未开放。该项属于既定“切换”门禁，不新增迁移范围。

## 2026-09-19：冷启动 cutover 执行器与隔离账号验收

- 增加显式预约、Legacy 网络冻结、事件入口停止、ViewModel 子任务排空、媒体任务与待 ACK
  消息门禁；只有上述条件全部满足才关闭 Room、持久化预约并退出进程。运行期不热切换后端。
- 冷启动存在预约时不创建 `MainViewModel`/`ImClient`，由 `ColdStartCutoverRunner` 打开旧 Room、
  迁移 Native、提交 journal 与 HMAC 镜像；失败进入 Repair，保留旧库且不自动回退。
- 隔离账号/隔离文件根设备测试首次发现并修复：首次 cutover 未创建 `native_kernel` 父目录，
  导致 Native 进程锁文件无法创建。修复后 `ColdStartCutoverRunnerTest` **1/1 PASS**，验证真实
  SQLCipher Room 数据迁移、NO_WRITE 双证据及切换后旧库拒写；不读取或修改现有账号数据。
- `CutoverTaskDrainTest` 覆盖已有任务及其派生任务排空、媒体未结束/超时拒绝；Android 编译、
  单元测试、静态依赖门禁通过，网络冻结与协调器设备测试此前均通过。
- 入口仍只在 debuggable 包显示。生产开放前仍需真实服务端隔离账号的进程级强杀矩阵。

### 事实更正：Native AI 尚未生产接线

- `AiSuggestionService` 当前是经过单测的独立 C++ 状态机，但 `ClientCore` 尚未注册
  `AI_REPLY_RS` handler，也没有 AI 请求/取消发送 API；`ClientRuntime` 未持有该服务，JNI/SDK/
  `NativeAppRoot` 均无生产入口。因此 AI 不能标为“仅差 UI”，而是端到端生产接线未完成。
- 在 ClientCore→Runtime→JNI/SDK→Compose 完成并通过真实服务端响应、取消/超时/迟到抑制前，
  第七轮整体继续为 **PARTIAL / CHANGES REQUIRED**，不得开放全量生产切换。
