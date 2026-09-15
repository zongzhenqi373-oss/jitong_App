# 第七轮修复与补全方案

日期：2026-09-11。范围：当前第七轮新增代码的剩余缺陷，以及 G0～G9 尚未完成的生产接入。
本文件是待执行方案，不是验收报告；现有 `kernel-round7-asan.log` 的 31 项通过只代表该日志对应构建。

## 1. 基线与完成定义

当前已有 MessageService、NativeRepository、SyncTracker、ClientRuntime 生命周期、设备密钥包装和局部测试。
Native Schema 实际为 v3：001 初始表、002 FTS identity、003 P7 状态机表。Room 仍为 v8。
SDK 主要开放认证、快照迁移、Runtime 生命周期；Android 消息、搜索、媒体仍主要使用 Legacy。

已修部分不重复推倒：消息 ID 已加入随机数；接收上下文已可传入；已读重算已使用有效水位；重复草稿
已避免重复 FTS；媒体变体已使用 DTO 真实 MIME 和宽高；Native key bridge 已允许无 passHash 进入设备路径。

完成条件分两级：

- 修复完成：F01～F05 的已知缺陷消除，回归通过，基础组件可供后续业务使用。
- 第七轮完成：F06～F11 同时完成，Android 全部现有业务经 Native SDK 运行，在线迁移和单向切换有证据。

分片上传、精确区间补洞、多设备已读保持增强项关闭；不要求为了通过本轮新增服务端业务。不能把增强项
写成已交付，也不能因其关闭阻塞既有秒传、整文件上传和 Range 下载。

## 2. 工作包与依赖

| ID | 工作 | 优先级 | 前置 | 完成证据 |
|---|---|---|---|---|
| F01 | 持久化发号、Outbox 恢复与回执 | P1 | 无 | 重启、并发、重复回执测试 |
| F02 | Runtime 生命周期和执行器 | P1 | 无 | 确定性并发测试、JNI 销毁测试 |
| F03 | 双包装升级和故障恢复 | P1 | 无 | Android Keystore 矩阵 |
| F04 | 完整消息/媒体事务与会话查询 | P1 | F01 | DTO 往返、分页与回滚测试 |
| F05 | 文档、Schema、恢复决策统一 | P1 | 无 | 状态表与代码常量对齐 |
| F06 | SDK 接通账号、消息、同步、好友、AI | P1 | F01～F05 | 双端网络 E2E |
| F07 | Native 搜索与定位 | P1 | F04、F06 | Legacy/Native 差分 |
| F08 | Native 图片、文件和缓存 | P1 | F03、F04、F06 | 媒体 E2E、像素与流量证据 |
| F09 | Room change log、snapshot、delta | P1 | F04、F05 | 持续写迁移与强杀测试 |
| F10 | 单向 cutover 和薄 UI | P1 | F06～F09 | 启动恢复、依赖扫描、灰度 |
| F11 | 全量验收与报告 | P1 | F01～F10 | 新构建完整测试与日志 |

每个工作包独立提交。F01～F09 在测试/影子路径验收，F10 才允许选择 Native 作为业务事实源。

## 3. F01：发号、Outbox 与回执

代码：`client_core/src/message/MessageService.cpp`、`client_core/include/client_core/message/MessageService.h`、
`client_core/src/storage/NativeRepository.cpp`、Native migrations、对应 tests。

问题：`setLocalSeq` 只是可调用方法，没有生产恢复入口；重启后 local_order 重复会碰撞 Outbox 主键。
Outbox 的 packet_type=0、payload=正文尚无完整恢复契约。Ack 仅凭 status>0 判断终态，没有明确协议结果映射。

实施：

1. 增加账号本地序号表，由 Writer 在创建草稿同一事务内分配 local_order 并推进计数。升级时从 messages、
   outbox 两表最大值初始化；不要由 Service 先读 MAX 再自行加一。事务回滚时不暴露未提交草稿。
2. 消息 ID 使用经过验证的 128 位随机 UUID 实现或等价方案；测试注入生成器。保留同一 operation 的原
   msgId，网络重试不能重新生成 ID。墙钟只用于展示，不作为唯一性或单调性的保证。
3. Outbox 保存明确版本的业务发送数据：协议号、msgId、接收方、消息类型及完整 payload。不要保存 GCM
   密文和旧连接 sequence；重连后使用新安全会话重新加密。
4. 新增 pending 查询、领取/发送状态、retryAt、attempt、错误域和显式取消；设置有界并发、指数退避和
   jitter。重启只恢复未终结任务；超时“提交结果未知”通过 operationId/msgId 查状态后再决策。
5. 将服务端结果码映射为明确领域结果：在线成功、离线已存储、永久拒绝、可重试失败。只在确认服务端
   已接受或明确永久终结时处理 Outbox；重复响应不得重复增加 attempt。已存在有效 seq 冲突需报一致性错误。

验收：服务重建且旧 Outbox 未清空后继续发送；跨进程重启和同账号并发发号；DB commit 后发送前强杀；
服务端已存储但 Ack 丢失；重复成功/重复失败/成功后迟到失败；每次最终只产生一个逻辑消息。

## 4. F02：Runtime 生命周期与执行器

代码：`client_core/{include/client_core,src}/runtime/ClientRuntime.*`、DbCommandQueue、
`jitong_android/app/src/main/cpp/im_runtime_jni.cpp`、`native_sdk_handle.*`。

问题：m_db 部分访问已加锁，但 setDatabase/raw database getter 仍缺少完整所有权保护；start 在 Stopping
期间仍可能启动，destroy 完成后又覆盖新一代状态。Running 目前不证明数据库或业务服务可用。

实施：

1. 固定状态转换：Idle/Stopped→Starting→Running；Running→Stopping→Stopped；任意非终态→Destroying→Destroyed。
   Stopping/Destroying 不接受 start；Destroyed 永不可复活。重复调用返回既有操作状态，不能提前宣称完成。
2. 用单一控制执行序列承载生命周期命令，或等价的带操作 generation 的互斥状态机。长耗时关闭在锁外执行，
   返回时核对生命周期 generation；destroy 设置不可逆关闭标志后再释放锁。
3. 所有 m_db 访问经锁内 shared_ptr 快照，删除可跨线程使用的裸指针 getter；setDatabase 仅在未启动时允许，
   校验账号归属。JNI 重复 create 同账号返回既有 Runtime；换账号必须完成旧实例停止后才能替换。
4. Runtime 组装账号、消息、同步、好友、AI、搜索、媒体服务。区分 RuntimeStarted、AccountOnline、
   DatabaseReady、SyncReady，不能把创建空 NativeDatabase 对象当作业务 Ready。
5. completion executor 归 Runtime；事件在 commit 后发布。IO/Writer 不等待 UI collector，invalidation 按
   domain 合并最大 dbVersion；命令结果进有界 registry，容量满时拒绝新命令，不丢已接受命令的终态。
6. 销毁顺序：拒绝新命令→取消网络/任务→终结在途操作→drain/barrier→关闭库→释放 observer/global ref。
   drain 不在被等待线程执行；stop/logout 后旧 generation 不可污染新状态。

验收：用 latch/barrier 固定 start-stop、start-destroy、重复 destroy、JNI create-destroy 交错；不以随机
压力代替确定性测试。断言无 Running/Destroyed 矛盾、无连接复活、无泄漏或悬挂 operation。运行 ASan/UBSan，
有可用环境时运行 TSAN，Android 跑 CheckJNI。

## 5. F03：双包装解锁补全

代码：`DbKeyManager.kt`、`DeviceKeyWrapper.kt`、`NativeDbKeyPlatform.kt`、账号启动编排与 Android tests。

实施：

1. 把“只解锁”和“首次创建”拆成不同接口。无 passHash、无可用 deviceWrapper 时不得用空摘要创建密码包装。
2. passwordWrapper 与 deviceWrapper 独立尝试：其中一份丢失不能提前跳过另一份。两者均失败且旧库存在时
   返回 LockedWithData，保留库和全部 blob；没有旧库但已有损坏 blob，也不得自动覆盖证据。
3. 密码成功解锁后在账号锁/文件锁内验证 deviceWrapper，缺失或损坏均可用已获得的同一 realKey 修复。
   不只判断 exists。设备路径成功且密码包装缺失时，保留可用设备包装，等待显式密码重包装流程。
4. wrapper 增加 magic/version/ownerId/keyId/type 与 AAD，支持读取旧格式后安全升级；AEAD 成功后仍检查
   realKey 恰为32字节。临时文件同目录写入、fsync、原子替换，失败保留旧文件；读路径禁止创建新 Keystore key。
5. 密码变化只轮换 wrapper，不替换 realKey；设备包装修复失败不能把已成功的密码解锁误判为成功持久化。
6. 将 NeedsPasswordUnlock/LockedWithData 等类型化错误传过 JNI 到账号/UI。确定 Keystore 锁屏参数及后台
   访问语义，并用真机验证；不能由 setUserAuthenticationRequired(false) 推导“只在解锁时可用”。

验收：老用户升级；密码包装缺失但设备包装正常；设备包装损坏后密码修复；双损坏；Keystore alias 丢失；
无密码首次建库；轮换和原子替换中强杀。断言旧库 hash 不变、没有生成错误 realKey、没有静默空库。

## 6. F04：消息/媒体字段和会话投影

代码：NativeRepository、Dtos、Schema migration、查询 Repository 与测试。

实施：

1. 建立唯一的完整 MessageDto 绑定/读取函数，覆盖原图、小/大缩略图的 fileId、hash、尺寸、MIME、大小、
   localPath 等。文本、媒体、迁移和历史查询共用映射，避免新增路径再次漏字段。
2. 原始远端元数据与本地缓存分开：messages 保存兼容协议卡片，media_variants 保存缓存变体，media_refs
   保存引用；定义以哪张表为权威，不能查询时随意择一。媒体首次插入同事务维护会话、未读、必要 FTS 和引用。
3. 重复 msgId 不覆盖不可变 sender/peer/type；补齐缺失元数据，冲突返回一致性错误。不能用 INSERT OR REPLACE
   把已就绪缓存误重置成 pending。媒体与文本收到重复 Push 时不重复未读。
4. 会话增加明确 lastMsgId 与本地展示时间/排序键。本地新发送可立即把会话置顶；Ack 只修正所属消息，
   迟到 Ack 不覆盖后来消息摘要。同秒多消息按 seq/local_order 和稳定兜底键排序，不单靠时间戳比较。
5. 已读水位和未读重算统一使用有效 readSeq。前台接收不增未读时同时定义何时推进已读水位，防止后续重算
   重新计入；历史回填不应改变本地已读事实。read domain 的 key 明确为 peerId 或 conversationId，不能混用。
6. 增加版本化 keyset 查询契约；seq=0 与已确认消息合并时定义锚点，Ack 改 seq 后分页刷新不重不漏。

验收：完整 DTO 写入→关闭库→重开→读取逐字段相等；三档媒体可重新定位下载；故障注入任何一步失败全部
回滚；同秒消息、迟到 Ack、历史回填、当前会话接收后重算未读、并发分页场景均有 Golden。

## 7. F05：Schema 与 ADR 修订

保留已经存在的 001/002/003，不把 v3 改回 v2。新增结构使用下一版本 migration，并验证 0→最新、1→最新、
2→最新、3→最新。实施计划、三份 ADR、功能矩阵和测试断言统一实际版本，不能覆盖已经应用的迁移脚本。

Cutover ADR 将 NO_WRITE 回滚统一为显式人工操作。恢复决策必须覆盖 DB、KV 任一侧 DIRTY、缺失、损坏和
不同 epoch：任一可信侧表明已切换/产生新写时，禁止自动选择旧 Room；无法确定时进入修复状态。
“从服务端重建”只恢复服务端保留数据，本地未发 Outbox、已读状态和本地独有文件不能保证恢复，必须保留
原 Native 数据并明确恢复范围。固定矩阵放入机器可读 fixture，文档表由 fixture 对照校验。

验收：恢复矩阵所有分支对应可执行测试；无“NO_WRITE 不一致自动 Legacy”残留；新增迁移失败按已定义
事务边界回滚，保留原数据。

## 8. F06：业务服务与 SDK 接入

这部分是尚未完成的功能接线，不应仅用修复单元测试替代。

- Runtime 将 transport 解析事件转换为完整 DTO，再交 MessageService；Ack、Push、离线、漫游共用合并
  规则，区分来源上下文。Outgoing commit 成功才回显并调发送器；重连恢复 Outbox。
- SyncTracker 接入 SyncService：收到并提交消息后推进 arrived/contiguous；持久化 gaps/重试水位并在
  重启恢复。实现 beforeSeq 分页预算、时间预算和 single-flight；服务端保留期以外的历史不能无限补洞，
  需要明确不可恢复区间/同步起点，不能永远要求从 seq=1 补齐。
- FriendService 接好友资料、申请列表、同意/拒绝、删除和上下线，网络重同步与本地状态对账。
- AiSuggestionService 接 requestId、会话快照、tone、超时、取消及迟到响应过滤；上下文组装以当前服务端
  协议真实职责为准，不在客户端重复实现不存在的协议字段。
- 账号服务接注册与现有密码/Token/注销流程；SDK 开放消息、好友、同步和 AI API，所有业务回调带账号和
  generation；logout/allDevices 语义保持与 Legacy 一致。

验收：新 SDK 路径两端真实互发；在线/离线/断线重试；同时发送顺序一致；好友流程、AI 取消、Token 冷启动
和换号无串号。每项有从 UI intent 到网络再到数据库/状态的证据。

## 9. F07：搜索、拼音与定位

新增 SearchService 和查询 API。选定 C++ 拼音实现及字典版本，保持 Legacy 全拼、首字母和边界匹配语义；
FTS/LIKE 使用参数绑定，按 owner/conversation 隔离。高亮范围统一 UTF-16 code unit 或显式可转换编码，覆盖
emoji、组合字符和混合文本。SearchHit 带 msgId/conversationId 和定位锚点。

验收：中文、n/ni/nh、全拼、多词、LIKE 特殊字符、空词、100条上限和跨账号过滤；Legacy/Native 差分结果
与高亮相同；点搜索结果可加载目标附近历史；10万/100万条性能记录。

## 10. F08：媒体 Native 化

新增 MediaService、HTTP 平台/传输接口、Codec 接口及受控 worker 调度。C++ 负责任务状态、并发预算、
重试、取消、秒传策略、缓存和清晰度；Android 提供 URI/权限、文件访问和 Codec 原子操作。

兼容现有 preflight/proof、流式上传、Range 下载。校验 challenge offset/length，限制分配；Range 206
验证 Content-Range 与预期 offset，200 则重新下载，416 先验证本地文件完整性。401 经账号 single-flight
刷新后有限重试，403不无限重试。下载 hash 成功才原子发布；取消后不发布迟到结果。

Codec 返回真实 MIME、尺寸、方向和色彩处理结果。UI 根据宽高占位，缓存原图→大图→小图→占位，按
可视区触发预取；任务池按 CPU/阻塞 IO 的资源预算控制，不能在网络事件线程做 hash/编码。

验收：三档缓存组合、AVIF/PNG/JPEG、ICC/EXIF/透明和超长图；旧服务端兼容；秒传命中/失败；取消、401、
Range异常、低磁盘、URI撤权、重启恢复；以标准参考解码结果做像素/色差验证，并记录实测带宽与存储。

## 11. F09：在线迁移

新增 Room MIGRATION_8_9、change log Entity/DAO、LegacyWriteGateway；在旧业务事务内写权威变更，FTS
作为派生索引重建，不独立回放。静态枚举全部写入口，加测试 hook 检测漏记。

快照采用明确一致性方案：固定 snapshot high-water，并使用一致读快照或带 sourceVersion 的分页策略；
变更日志保存足够的版本化 after-image/delete tombstone，不能只记 key 后读取已变化的“当前行”。同一源
事务的多实体变化携带 txId，Native 原子回放整组并更新 checkpoint，禁止逐流推进造成部分事务可见。

账号登录/好友可由服务端重取；媒体按文件引用登记，不擅自搬删文件；在途 Legacy 任务在切换前排空、取消
或显式转为可验证恢复任务。最终关闭写闸门前先做大部分对账，只在停写窗口完成末尾 delta 与增量校验；
预算超限安全放弃此次切换并恢复 Legacy 写入，不能长时间卡 UI。

验收：snapshot期间持续插入/更新/删除/markRead/下载；日志重复、缺口、倒序、GC边界；每步强杀；支持的
Room旧版本逐一升级；checkpoint和媒体引用正确；停写P95/P99≤200ms有真实设备证据。

## 12. F10：单向 cutover 与 Android 薄 UI

实现 journal 存储、带认证 KV 镜像、统一 RecoveryResolver 和启动选择器。必须先写并持久化“可能产生
Native写”的单向承诺，再允许 Native 建连、发送或业务写入；首次业务事务与DB dirty标记同事务。KV更新
与DB事务没有跨库原子性，按F05矩阵保守恢复，允许提前禁止回退，不允许漏记后误回旧库。

Native启动失败通过类型化错误进入解锁/修复页；.so无法加载且账号已有切换承诺时也不能自动回Legacy。
MainViewModel和页面仅调用SDK；移除Native路径下ImClient/ChatStore/Room/HttpMediaClient直接依赖。Legacy
保留只读恢复副本，数据清理另行显式执行。

验收：journal每次写入/fsync前后强杀；DIRTY后收到新消息再制造启动失败；.so加载失败；双账号一个已迁移
一个未迁移；只存在一个业务长连接和一个写事实源；UI全功能回归；Release/R8/JNI/ABI加载检查。

## 13. F11：测试与报告

从当前源码重新配置、编译后运行测试，记录commit及dirty diff标识，不能只运行历史二进制。

必选：F01～F10确定性回归、C++ Golden/协议异常、SQLCipher迁移、ASan+UBSan、Android arm64 CheckJNI、
双端E2E、迁移强杀、两小时资源长稳、Release冒烟、搜索/消息延迟和媒体带宽基准。
条件性：TSAN、x86_64运行；不可用写NOT_AVAILABLE和替代证据，不能写PASS。ASan不能代替业务一致性或
数据竞争检查，四个基础测试通过不等于全量业务通过。

扩充 `kernel-round7-feature-parity.md`：每项增加实现文件、SDK入口、测试名、日志、状态，不只列计划。
生成 `kernel-round7-acceptance.md` 作为独立第七轮报告，列全部工作包结论、失败/跳过数、设备/ABI/命令。
关联最新asan、checkjni、e2e、migration-cutover、performance、media-bandwidth和security-review证据。

## 14. 执行与最终判定

建议提交顺序：F05 → F01/F02/F03 → F04 → F06 → F07/F08 → F09 → F10 → F11。
斜线表示可独立推进的工作包，不代表生产并行启用两套业务后端。

只有F01～F11必选用例全部通过、Native UI实际接通、在线迁移可恢复且不存在静默回退，才能判定第七轮PASS。
仅修完已知缺陷时应写“基础缺陷已修复，业务迁移待完成”；任何尚未连接的SDK、服务、表和测试工具均不得
按注释或文件存在性宣称功能完成。

## 15. 本次执行记录（2026-09-11）

本次已实施 F01～F05 的一批基础修复，具体完成边界和剩余项见 `kernel-round7-fix-acceptance.md`。
当前源码 C++ ASan/UBSan 33/33、TSAN 定向2/2、Android arm64 CheckJNI 24/24通过。
**整体仍为 PARTIAL**：F02/F03/F04仍有执行器、平台贯通及查询契约工作；F06～F10尚未完成生产接线。
禁止据此开启全量切换或把此记录当作第七轮完整PASS报告。
