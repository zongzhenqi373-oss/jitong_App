# 第五轮（P6 C++ SQLCipher 数据底座）执行方案

> 来源：`outputs/jitong-qqnt-kernel-execution-plan.md` 第 24 章（P6）
> 定位：把第 24 章的目标/边界/验收**拆解为可执行的阶段、交付物与判定动作**
> 状态：待执行（S0 为首个动作，纯验证、不改动生产代码）
> 编写日期：2026-09-09

本文是第 24 章的**执行层展开**，不改变第 24 章的任何目标与门禁；
文中所有"已核实"事实均以实际命令输出为依据，作为方案决策的前提。

---

## 0. 文档定位与读法

| 文档 | 作用 |
|---|---|
| 规划文档 第 24 章 | 定义 **做什么 / 不做什么 / 验收门禁**（权威） |
| **本文** | 定义 **怎么排期、每步产出什么文件、先验证什么、如何判定** |
| `outputs/kernel-round5-acceptance.md` | 本轮结束时的**验收报告**（尚未产出） |

执行原则（沿用第 24 章 + 前几轮实践）：

1. **先证伪、后设计**：S0 用只读/极小试验消解最大不确定性，再进入不可逆设计。
2. **按阶段提交、逐段验收**：每阶段都必须保持桌面 CTest + ASan/UBSan 全绿、前四轮回归不退化。
3. **不伪造证据**：TSAN 等不可用时如实记录原因，不以"功能通过"替代。
4. **保持不变**：不启用 Native Backend、不切 UI、不新建 Socket、不删改 Room 库。

---

## 1. 已核实的事实基线（方案依据）

以下命令/文件已实际核对，规划与实现不得与之冲突。

| 项 | 核实结果 | 位置 |
|---|---|---|
| Android 现状 | Room v8 + `net.zetetic:sqlcipher-android:4.6.1` | `app/build.gradle.kts:172` |
| `libsqlcipher.so` 已在 APK | arm64-v8a 5,797,528B；x86_64 6,372,968B | `app/build/outputs/apk/debug/app-debug.apk` |
| 密钥模型 | 每账号随机 32B `realKey`；`PBKDF2WithHmacSHA256(passHash, 16B salt, 100000)` 派包装钥 → AES-GCM 包装落盘；**明确不依赖 Keystore** | `data/crypto/DbKeyManager.kt` |
| 现有破坏性行为 | unwrap 失败/key blob 丢失 → `deleteEncryptedDatabase()` 删库；`AppDatabase` 有 `fallbackToDestructiveMigration()` | `DbKeyManager.kt:45/48`、`AppDatabase.kt:111` |
| Room FTS | `messages_fts` 为 FTS4 独立存储表，列 = `content/pinyin/initials/msgId` | `data/db/Entities.kt:63` |
| Room 实体 | `MessageEntity`(30+ 字段，含 media/thumbnail/largeThumbnail 与路径)、`MessageFtsEntity`、`ConversationEntity` | `data/db/Entities.kt` |
| C++ 现状 | `IStorage` 6 个**同步**方法（无 msgId/seq/status/FTS/事务/错误码）；`SqliteStorage` 用系统 sqlite、3 表、无版本无 key；`ClientCore` 在 **asio io 线程同步调用** | `include/client_core/IStorage.h`、`src/SqliteStorage.cpp` |
| 开关现状 | `CLIENT_CORE_WITH_SQLITE` Android **FORCE OFF**，注释即"P6 再开" | `app/src/main/cpp/CMakeLists.txt:82` |
| 交叉编译范式 | `build-openssl-android.sh`、`build-protobuf-android.sh`；预编译布局 `third_party/android/<abi>/{include,lib}`（已有 `libcrypto.a`/`libssl.a`） | `scripts/`、`app/src/main/cpp/third_party/` |
| 源码可获取 | SQLCipher `v4.6.1` tarball `http 200`；`/usr/bin/tclsh` 具备；git/curl/cmake/ninja 均在 | 实测 |

**由事实推出的两条硬约束（贯穿全文）：**

- 进程内**已有一套 SQLCipher**（Room 的 `libsqlcipher.so`），Native 不是唯一一套 → 符号隔离是 T01 前置条件。
- **没有 passHash 就解不开消息库**是现有设计意图 → P6 必须复用该语义，**不得**改成 Keystore key，也不得拿 Token 派生 key。

---

## 2. 总体执行顺序

```text
S0 可行性决策试验（可证伪，半天）── 决定 T01 走 方案A / 方案B
        │
        ▼
S1 P6-T01 SQLCipher Native 接入 ──► S2 P6-T02 密钥/账号隔离/平台桥
                                          │
                        ┌─────────────────┴─────────────────┐
                        ▼                                   ▼
        S3 P6-T03 Schema/Migration              S4 P6-T04 Writer/ReadPool/Queue/Cancel
                        └─────────────────┬─────────────────┘
                                          ▼
                        S5 P6-T05 Room → Native 影子迁移
                                          ▼
                        S6 JNI/SDK + Android 两 ABI 验证
                                          ▼
                        S7 测试 / 证据 / 门禁收口
```

- **可并行**：S3 与 S4（先定接口，再并行实现）；S5 的 Kotlin 导出侧在 S3 定完 DTO 后即可并行。
- **关键路径**：S0 → S1 → S2 → S3/S4 → S5 → S6。
- **强前置**：S1 中的"Room fixture ↔ Native fixture 互开"不过，不得进入 S2。

---

## 3. S0 可行性决策试验（最先执行）

**目标**：判定 T01 走**方案 A（复用 AAR 的 `libsqlcipher.so`）**还是**方案 B（自建私有 SQLCipher）**。第 24 章 24.3 明确"优先复用同一版本、同一份 shared object"，因此 A 为默认优选，但必须先证伪。

### 3.1 试验步骤

```bash
# 1) 取出 AAR 自带的 libsqlcipher.so（APK 中已确认存在，两 ABI 都有）
cd jitong_android
unzip -o app/build/outputs/apk/debug/app-debug.apk \
      'lib/arm64-v8a/libsqlcipher.so' 'lib/x86_64/libsqlcipher.so' -d /tmp/sqlcipher_probe

# 2) 检查是否导出完整 C API + codec（关键：sqlite3_key 是否存在）
NDK=$HOME/Library/Android/sdk/ndk/27.3.13750724
NM=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-nm
$NM -D --defined-only /tmp/sqlcipher_probe/lib/arm64-v8a/libsqlcipher.so \
  | grep -E ' T (sqlite3_open_v2|sqlite3_key|sqlite3_prepare_v2|sqlite3_step|sqlite3_column_text|sqlite3_exec)$'

# 3) 试链接：用 v4.6.1 源码头文件（仅头文件，不编译库）链接到该 .so
#    写一个 sqlite3_open_v2 + sqlite3_key + 建表 + 读写的极小 target，两 ABI 各一次

# 4) 体积基线（方案 A 增量应为 0）
ls -l app/build/outputs/apk/debug/app-debug.apk
unzip -l app/build/outputs/apk/debug/app-debug.apk | grep jitong_kernel
```

### 3.2 判定与分叉

| 方案 | 触发条件 | 做法 | 优点 | 代价/风险 |
|---|---|---|---|---|
| **A（优选）** | 2) 导出的 API 完整且含 `sqlite3_key`，且 3) 试链接+装载通过 | 不自建库；用 v4.6.1 源码头文件编译，运行时依赖 Room 已加载的 `libsqlcipher.so` | **零体积增量**；与 Room 同版本同 cipher 参数；天然只有一份 SQLCipher，无符号冲突 | 依赖 AAR 导出符号与链接命名空间；AAR 升级需回归 |
| **B（回退）** | 上述任一不成立 | 自建私有静态库 + `jt_sqlite3_*` 前缀重命名 + hidden visibility + linker version script | 完全自主可控 | 新增数 MB/ABI；需新增 `scripts/build-sqlcipher-android.sh`；必须过"双库无符号抢占"验收 |

**产出**：一份决策记录（结论 + `nm -D` 证据 + 体积基线），写入 `outputs/kernel-round5-encryption-note.md` 的开头。

---

## 4. S1 — P6-T01 SQLCipher Native 接入

**依赖**：S0 结论。**阻塞**：不过不进 S2。

### 4.1 交付物

| 文件 | 内容 |
|---|---|
| `client_core/CMakeLists.txt` | 新增独立开关 `CLIENT_CORE_WITH_SQLCIPHER`；与 `CLIENT_CORE_WITH_SQLITE` 完全分离（桌面 `WITH_SQLITE` 保持 ON、Android 保持 OFF） |
| `client_core/include/client_core/storage/CipherDatabase.h`<br>`client_core/src/storage/CipherDatabase.cpp` | 固定打开顺序 `open_v2 → key → 固化 cipher 参数 → 首个受保护查询`；结构化错误 |
| `client_core/src/storage/CipherParams.h` | 固化 `page_size / kdf_iter / KDF / HMAC / page HMAC`（**由 Room 4.6.1 fixture 实测得到，禁止凭印象填写**）+ 启动自检 |
| （仅方案 B）`scripts/build-sqlcipher-android.sh`<br>`third_party/android/<abi>/{include,lib}` | 照抄 `build-openssl-android.sh` 范式；`jt_sqlite3_*` 前缀 + hidden visibility + version script |

### 4.2 必须最先完成的一件事（最高优先级）

用现有 `DbKeyManager` 生成的同账号 `realKey`，验证：

- **Room 4.6.1 建的加密 fixture 可被 Native 只读打开**
- **Native 建的兼容性 fixture 可被 Room 打开**

> 原因：raw key 的编码约定（`x'<64hex>'` 字符串 vs 32 字节 blob）是此类集成最经典的坑。
> 这一步不过，后续全部工作建立在错误假设上。它与 24.3 验收第 4 条一致，但必须**前置**到 S1 开头。

### 4.3 验收（对应 24.3）

- [ ] arm64-v8a / x86_64 均完成编译、链接、`.so` 装载
- [ ] `PRAGMA cipher_version` 返回预期版本；生产构建符号确认来自 SQLCipher
- [ ] `nm -D jitong_kernel.so` 不导出裸 `sqlite3_*`；Room 与 Native DB 同进程交替/同时打开读写正常，无符号抢占、参数串扰、崩溃
- [ ] Room fixture ↔ Native fixture 可用同一 `realKey` **互相打开**（影子业务库仍独立路径，不并发写同一文件）
- [ ] 正确 key 可重启读取；错误 key、空 key 均失败且**原文件摘要不变**
- [ ] 普通 `sqlite3` CLI 无法读取 schema/正文/手机号/Token/marker；`strings` 检索不到唯一明文 marker
- [ ] 输出两 ABI 体积增量；超预算先评审

---

## 5. S2 — P6-T02 DB Key、账号隔离、平台桥

### 5.1 交付物

**C++ 侧**
- `IPlatformKeyBridge { loadOrCreate(ownerId) -> SecureBuffer; delete(ownerId) }`：只接受"本次开库所需的 32 字节副本"，**不持久化、不回传、不记录**，开库/失败后清零
- 路径规范化：仅 `filesDir/native_db/account_<ownerId>.db`，拒绝 `..`/斜杠/空 ownerId/越界（创建文件前拒绝）
- 状态与错误码：`Locked / NeedDatabaseUnlock`；`DbKeyUnavailable / DbKeyMismatch / SchemaUnsupported`
- 切账号：先完整关闭旧账号 Writer/ReadPool，再装载新账号；logout/destroy 清零 key buffer

**Kotlin 侧（改造现有，非新建）**
- `DbKeyManager.getOrCreateRealKey()`：去掉 `deleteEncryptedDatabase()` 自动删库 → **fail-close**，保留密文 DB 与 key blob
- `AppDatabase`：去掉 `fallbackToDestructiveMigration()`
- 新增**显式**"清除本机数据并重建"API（单独立项、单独测试）

### 5.2 语义硬约束

- Native DB 与同账号 Room **共用同一份 `realKey`**，但**使用不同数据库文件**。
- **Token 冷启动只能恢复网络身份**，不能解开 PBKDF2 包装的 DB key：无 `passHash` 时 Native DB 保持 `Locked/NeedDatabaseUnlock`，**不得**用 Access/Refresh Token 派生 key，也**不得**静默回退 Keystore。
- 普通 logout 只关闭库，**不删聊天数据**。

### 5.3 验收（对应 24.4）

- [ ] 同账号重启后 key+数据可恢复；不同账号 key/路径/内容互不可读
- [ ] 日志/AccountEvent/异常/崩溃报告中无 key 或 raw-key pragma
- [ ] 错误 ownerId / path traversal 在**创建文件前**被拒绝
- [ ] logout/切账号/destroy 后 key buffer 清零、连接全关
- [ ] key blob 丢失/损坏/passHash 不匹配时 fail-close，DB 与 blob 未被覆盖/删除/截断
- [ ] 用现有 `DbKeyManager` 的 realKey，Room fixture 与 Native 兼容性读取通过
- [ ] Token-only 冷启动完成网络认证但 DB 保持锁定且文件不变；成功密码登录后可打开
- [ ] 确认不再继承 `deleteEncryptedDatabase()` / `fallbackToDestructiveMigration()` 的自动删库行为

---

## 6. S3 — P6-T03 版本化 Schema 与迁移

### 6.1 交付物（路径按 24.5）

```text
client_core/include/client_core/storage/NativeDatabase.h
client_core/include/client_core/storage/DbTypes.h
client_core/include/client_core/storage/DbError.h
client_core/src/storage/NativeDatabase.cpp
client_core/src/storage/SchemaManager.cpp
client_core/src/storage/migrations/001_initial.sql
```

表：`messages / conversations / friends / message_fts / sync_watermarks / outbox / media_records / transfer_tasks / migration_meta`

### 6.2 建表前必须先做的一件事（易错点）

从 `MessageFtsEntity` 的 `@Fts4` 注解读取 `tokenize` 参数并**原样固化**到 Native 建表语句：

```bash
grep -n -A6 "Fts4" jitong_android/app/src/main/java/com/jitong/im/data/db/Entities.kt
```

> tokenizer 不一致会直接导致 24.5 最后一条验收（"正文存在但拼音搜不到"）失败。

### 6.3 必须固化的约束

```text
UNIQUE(owner_id, msg_id)
INDEX messages(owner_id, peer_id, conversation_seq)
INDEX messages(owner_id, peer_id, server_time, msg_id)
INDEX messages(owner_id, status, local_order)
UNIQUE(owner_id, peer_id, conversation_seq) WHERE conversation_seq > 0
INDEX conversations(owner_id, last_message_time)
```

- `msg_id` 负责幂等，`conversation_seq` 负责顺序与补洞，**二者不可替代**。
- 同 seq 对应不同 msg_id → 返回数据一致性错误并保留证据；**禁止 `INSERT OR REPLACE`**。
- 每个 Migration 单事务；失败时 `user_version` 与 schema 保持旧版本。
- **禁止**破坏性迁移 / 捕获异常后删库。
- 全部 SQL 使用绑定参数；表名/排序字段等动态部分来自**枚举白名单**。
- `message_fts` 必须保留 `content/pinyin/initials/msg_id` 四列并导入现有拼音数据。

### 6.4 验收（对应 24.5）

- [ ] 空库 0→N 连续升级与跨版本升级结果一致
- [ ] 每个 Migration 中途故障完整回滚，重启可再次执行
- [ ] msg_id 重复导入不增行；同会话 seq 冲突明确报错
- [ ] `EXPLAIN QUERY PLAN` 证明历史分页/msg_id 查重/outbox 查询命中目标索引
- [ ] FTS 行数与可检索文本消息数一致；重建两次结果一致
- [ ] 中文正文/全拼/首字母 fixture 与当前 Room 搜索结果对账（**无"正文存在但拼音搜不到"倒退**）

---

## 7. S4 — P6-T04 单 Writer、只读连接池、取消语义

### 7.1 交付物

- `client_core/src/storage/DbCommandQueue.cpp`：有界队列，满载返回 `QueueFull`
- `client_core/src/storage/ReadPool.cpp`：初始 2 条 readonly、可配有上限、单查询作用域独占
- operation id + cancellation token；结果复制为值对象（**不向 JNI 暴露 `sqlite3_stmt*` 或裸指针**）
- keyset pagination：`(conversation_seq,msg_id)` / `(server_time,msg_id)`，**禁止大 OFFSET 深分页**
- invalidation 仅 `{table, conversation, version}`，**在 commit 成功且锁释放后**发送

### 7.2 实现时重点控制的四点

1. **取消语义**：只在事务**开始前**检查 token；已开始的事务**不中途取消**，按操作语义整体提交/回滚（防止半事务）。
2. **批语义**：明确"一条业务原子操作 = 一个事务"（消息+FTS+会话摘要+checkpoint），10 万写压测按批提交，统一吞吐与验收口径。
3. **只读连接执行写 SQL 必须被拒绝**（用 `sqlite3_stmt_readonly` 或 `SQLITE_READONLY` 校验）。
4. **析构顺序**（严格按 24.8）：停收新任务 → 唤醒/完成 waiter → 取消或跑完队列 → join → 关读连接 → 关 Writer → 清 key。

### 7.3 边界（写进注释与断言）

`NativeDatabase/DbCommandQueue/ReadPool` 是**独立组件**：本轮**不得**塞进 `SqliteStorage`，**不得**调用 `ClientCore::setStorage()` 替换生产路径（否则 io 线程仍会同步等待 SQL）。旧 `IStorage` 仅保留桌面 Legacy/兼容测试用途，P7 才接线。

### 7.4 验收（对应 24.6）

- [ ] 8~16 线程并发投递 ≥10 万写命令，无 `SQLITE_BUSY`/死锁/丢失/重复
- [ ] **实际只有一个可写 connection**（所有写 SQL 线程 id 相同）
- [ ] 多只读查询可并行；readonly 连接执行写 SQL 被拒绝
- [ ] commit 前查询看不到半成品；commit 后一次合并 invalidation
- [ ] commit 前注入慢查询/延迟，Android 主线程仍可响应；rollback 不发送 invalidation
- [ ] 队列满/取消/SQL 失败/close/logout/destroy 时每个任务都有确定结果
- [ ] ASan+UBSan 无 UAF/泄漏/越界/double free；TSAN 可行则无数据竞争

---

## 8. S5 — P6-T05 Room → Native 可恢复导入（影子迁移）

### 8.1 交付物

- Kotlin `MigrationExporter.kt`：Room DAO **游标分页导出** DTO（含 `pinyin/initials`），不全量载入内存
- JNI/C++：`nativeSubmitMigrationBatch` 每批**单事务**按 `(owner_id,msg_id)` 幂等写（消息 + FTS + 会话摘要 + checkpoint）
- 对账：消息数、会话数、各会话 max/min seq、FTS 行数、抽样内容摘要 → 原子写 `migration_completed=1`
- 背压：默认批次在 **200~500** 中压测选定；JNI 只允许**一个有界批次在途**（credit/backpressure）
- 开关：`disabled → shadow_import → verified`，可停用/续跑/回滚影子数据

### 8.2 安全边界（必须写进测试断言）

- 不删除/修改 Room 库，不抢占 UI 数据所有权
- 测试用**测试夹具或账号副本**，禁止用真实聊天库做破坏性试验
- 坏数据记录**不含正文**的定位信息并**整体失败**，不得跳过后宣称完成
- `mediaPath/localPath/thumbnailPath/largeThumbnailPath` **仅作字符串导入**；不移动/复制/删除媒体文件（P10~P12 前所有权仍属 Legacy）
- Native DB **仅主进程**可打开（进程名校验或文件锁）
- "清理影子库"**不得**触碰生产 Room 与媒体文件

### 8.3 验收（对应 24.7）

- [ ] 0 条 / 1 条 / 分页边界 / 10 万条均可完成
- [ ] 在 0%、一批提交后、50%、最终标记前**强杀进程**，重启均可恢复且结果相同
- [ ] 连续执行两次，业务表计数/未读数/max_seq/摘要**不变化**
- [ ] 注入重复 msg_id、seq 冲突、非法 UTF-8、超长正文、损坏媒体元数据 → 错误可定位且不误标完成
- [ ] 校验失败时 Legacy Room 仍能正常打开/搜索/展示
- [ ] 默认批次、峰值内存、主线程卡顿达预算；迁移期间 Legacy 主线程读写不受明显影响
- [ ] 媒体路径逐字段对账，且**文件 inode/hash/数量不变**
- [ ] 第二进程打开 Native DB 被确定拒绝；迁移开关可停用/续跑/回滚

---

## 9. S6 — 正式 JNI / SDK（严格按 24.8）

仅暴露 6 个稳定接口，**禁止** `nativeExecSql(String)`、裸数据库句柄、SQLCipher key getter：

```text
nativeOpenAccountDatabase(handle, ownerId, platformBridge)
nativeCloseAccountDatabase(handle)
nativeSubmitMigrationBatch(handle, batchDto, checkpoint)
nativeFinishMigration(handle, expectedSummary)
nativeGetMigrationState(handle)
nativeRunDatabaseSelfTest(handle)
```

交付：
- `jitong_android/app/src/main/cpp/im_db_jni.cpp`
- `NativeBindings` 增加对应 `external` 声明
- `JitongSdk` **只增加**迁移/自检入口（默认 Backend 与 UI 路径保持不变）
- `NativeSdkHandle` 持有 DB 组件并按「停任务 → join → 关读连接 → 关 Writer → 清 key」析构
- JNI 输入先做大小/范围校验（**分配前拒绝**超大输入）；Java 回调在事务提交与锁释放后发生

Android 验证：两 ABI 编译链接；arm64 AVD/真机 open-write-close-reopen；Room+Native 双库共存冒烟（`nm -D` 佐证）；CheckJNI 全绿；循环 open/import/query/close 100 次后 fd/线程/句柄回基线。

---

## 10. S7 — 测试与证据

### 10.1 C++ 测试（新增）

```text
client_core/tests/test_native_database.cpp
client_core/tests/test_schema_migrations.cpp
client_core/tests/test_db_command_queue.cpp
client_core/tests/test_read_pool.cpp
client_core/tests/test_room_import.cpp
```

覆盖：Schema/Migration 回滚、索引命中、幂等、seq 冲突、事务可见性、队列背压、取消、close/destroy 竞态、10 万写压力、keyset pagination。

### 10.2 证据输出（独立，不与前四轮混写）

```text
outputs/kernel-round5-acceptance.md
outputs/kernel-round5-unit.log
outputs/kernel-round5-db-stress.log
outputs/kernel-round5-asan.log
outputs/kernel-round5-tsan.log                 # 不可用则记录原因，不伪造通过
outputs/kernel-round5-android-arm64.log
outputs/kernel-round5-android-x86_64-build.log
outputs/kernel-round5-checkjni.log
outputs/kernel-round5-room-migration.log
outputs/kernel-round5-encryption-note.md       # 开头放 S0 决策记录与体积基线
```

安全与稳定性：普通 SQLite/`strings` 负向验证；日志与制品扫描 DB key/消息 marker/Token 必为零命中；NDK TSAN 可行性**编码前预研**并记录。

---

## 11. 风险登记与对策

| 风险 | 影响 | 对策 | 阶段 |
|---|---|---|---|
| raw key 编码约定不一致 | Room↔Native 互开失败，**全轮阻塞** | 双 fixture 互开验证前置为 S1 第一件事 | S1 |
| FTS4 `tokenizer` 不一致 | 拼音/中文搜索对账失败 | 建表前从 `@Fts4` 注读取并固化 | S3 |
| 方案 A 链接命名空间/导出符号不全 | T01 返工 | S0 用 `nm -D` + 试链验证；不成立即切 B（脚本范式已备） | S0 |
| 改造 `DbKeyManager` 去掉自动删库 | 触碰现有生产行为 | 单独小步提交 + 专项测试；由显式"清除"API 覆盖 | S2 |
| 双 SQLCipher 符号冲突（仅方案 B） | Room 误调我方库→参数不符→失败/损坏 | 前缀重命名 + hidden visibility + `nm -D` 验收 | S1 |
| 10 万写吞吐/背压 | 压测超时或 OOM | 分批事务 + 明确批语义；记录吞吐/峰值内存/主线程卡顿 | S4/S5 |
| TSAN 在 NDK/macOS 不可用 | 并发证据缺口 | 编码前预研；不可用则保留桌面 TSAN 或锁序+并发压力替代证据，**不伪造** | S7 |

---

## 12. 里程碑与提交节奏

| 里程碑 | 内容 | 收敛的不确定性 |
|---|---|---|
| **M1** | S0 + S1 | SQLCipher 能否在 Android 真正打开加密库（最大不确定性） |
| **M2** | S2 + S3 | 密钥/隔离/Schema 定版（Schema 定版后变更成本高，此处应加重评审） |
| **M3** | S4 | 并发与生命周期 |
| **M4** | S5 + S6 | 迁移与 Android 验证 |
| **M5** | S7 | 证据与门禁收口 |

每次提交需保证：桌面 CTest 全绿 + ASan/UBSan 全绿 + 24.10 回归门禁（P1/P2 生命周期、P3 Transport、P4 安全通道、P5 AccountSession/TokenStore/Single-Flight、Legacy Android 登录/文本/图片/搜索）不退化。

---

## 13. 决策记录

| # | 决策 | 结论 | 依据 |
|---|---|---|---|
| D1 | DB key 是否改为 Keystore | **否**，复用 `DbKeyManager` 的 passHash 包装语义 | 24.1/24.4；`DbKeyManager.kt` 明确不依赖 Keystore |
| D2 | T01 链接策略 | **优先方案 A**（复用 AAR `.so`）；S0 证伪后可能切 B | 24.3"优先复用同一份 shared object" |
| D3 | 是否替换 `IStorage`/`SqliteStorage` | **否**，NativeDatabase 独立存在，P7 接线 | 24.1/24.6 |
| D4 | 是否保留 FTS 拼音 | **是**，原样导入 `pinyin/initials` | 24.1/24.5；Room FTS4 已含 |
| D5 | 是否触碰媒体文件 | **否**，仅导路径字符串 | 24.7；`MessageEntity` 只存元数据 |

---

## 附：关键命令速查

```bash
# 取 Room 的 libsqlcipher.so
unzip -o app/build/outputs/apk/debug/app-debug.apk 'lib/*/libsqlcipher.so' -d /tmp/sqlcipher_probe

# 检查导出符号（方案 A 判定依据）
$NM -D --defined-only <so> | grep -E ' T (sqlite3_open_v2|sqlite3_key|sqlite3_prepare_v2)$'

# 我方 .so 不得导出裸 sqlite3_*（方案 B 时必须）
$NM -D jitong_kernel.so | grep 'sqlite3_'   # 期望：无输出

# 读取 Room FTS4 tokenizer（S3 建表前）
grep -n -A6 "Fts4" app/src/main/java/com/jitong/im/data/db/Entities.kt

# 桌面回归
ctest --test-dir build/client-core-desktop --output-on-failure
bash scripts/native_sanitizer_test.sh --client-only
```
