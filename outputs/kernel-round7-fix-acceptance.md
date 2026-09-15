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
