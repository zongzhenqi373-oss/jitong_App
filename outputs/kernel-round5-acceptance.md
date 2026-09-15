# 第五轮验收报告：P6 C++ SQLCipher 数据底座

> 对应规划文档 `outputs/jitong-qqnt-kernel-execution-plan.md` 第 24 章。
> 本报告为 P6 的最终验收与收口记录，证据见 `outputs/kernel-round5-*.log`。

## 1. 结论

**P6 判为完成**（§24.10 门禁全部满足）。唯一以桌面 TSAN + 锁序/并发压力作为替代证据的是
NDK TSAN（AVD 无法可靠运行，§24.9.3 已明确允许该替代路径）；OS 级「强杀进程」恢复的核心
语义由 checkpoint 持久化 + 关库重开续跑覆盖，需外部脚本协调的分阶段 instrumentation 记为
已知遗留（不影响完成判定，见 §5）。

## 2. 各任务完成情况

| 任务 | 内容 | 状态 |
|---|---|---|
| S0 | 可行性决策：方案 A（复用 AAR `libsqlcipher.so`）成立 | ✅ |
| S1 / T01 | SQLCipher Native 接入 + `CipherDatabase` + 参数固化 | ✅ |
| S2 / T02 | DB key、账号隔离、平台桥 + fail-close 改造 | ✅ |
| S3 / T03 | 版本化 Schema + `SchemaManager` + `001_initial.sql` | ✅ |
| S4 / T04 | `DbCommandQueue` + `ReadPool`（单 Writer / 只读池 / 取消） | ✅ |
| S5 / T05 | `MigrationImporter` + `MigrationExporter` + 影子迁移 JNI | ✅ |
| S6 | 正式 JNI/SDK（6 接口 + `JniDbKeyBridge` + `NativeDbKeyPlatform`） | ✅ |
| 收口 | 第二进程锁 / 数据健壮性 / TSAN / Android 长稳 | ✅（本轮补） |

## 3. 本轮收口新增内容

### 3.1 第二进程打开被拒（§24.7）
- 新增 `storage/ProcessLock.h`（header-only）：`flock` 排他锁作用于 `<dbPath>.lock` 旁路文件，
  同一时刻仅一个进程可持有；进程退出时内核自动释放。
- `NativeDatabase::open` 在解析路径后、打开加密库前 `tryAcquire`，失败即 `Locked` fail-close；
  `close` 释放锁。
- 新增 `test_process_lock`（7 项）：首次获取 / 同进程第二实例被拒 / **fork 子进程被拒** /
  释放后可再获取，全部通过。

### 3.2 数据健壮性（§24.7）
`test_room_import` 追加 3 组用例：
- 超长正文（1 MiB）导入成功且长度精确落库；
- 非法 UTF-8 正文不崩溃，整体成功或整体回滚（不残留半批）；
- 损坏媒体元数据（负值 file_size/尺寸）原样落库可定位。

### 3.3 TSAN 预研（§24.9.3）
- 桌面 TSAN（Apple clang 21 / macOS arm64）可用，冒烟程序成功检测 data race。
- `test_db_command_queue`（22 项）与 `test_read_pool` 在 TSAN 下运行**无 data race 报告**，
  作为并发正确性替代证据（NDK/AVD 无法可靠运行 TSAN）。
- 证据：`outputs/kernel-round5-tsan.log`。

### 3.4 Android 长稳（§24.9.3）
新增 `NativeDbLongRunTest`（arm64 AVD，3/3 通过）：
- 循环 open → selfTest → close **100 次**，fd 数量回到基线（锁/连接池/队列不泄漏）；
- 循环 open → import(写 checkpoint) → query → close **100 次**，导入路径无泄漏；
- 关库重开后锁已释放、Schema 与 checkpoint 持久。

### 3.5 迁移开关三态 + 清理影子库（§24.7）
- `MigrationImporter` 新增 `MigrationState{Disabled,ShadowImport,Verified}` + `setState/getState/
  beginImport/resetForReimport`；`finish` 对账通过自动置 `Verified`。
- `DatabasePaths::clearShadowDatabase`：只删影子库自身与 `-wal/-shm/.lock`，不碰生产 Room/媒体。
- 新增 `test_migration_switch`（27 项）：状态机流转 / 拒绝重复迁移 / 回滚 / 持久化 / 清理边界，全过。

### 3.6 媒体文件完整性对账（§24.7）
新增 `MediaFileIntegrityTest`（arm64 AVD，1/1 通过）：Room 夹具消息指向真实媒体文件，完整迁移后
逐文件对账 **inode / SHA-256 / 数量** 均不变，证明迁移器只搬路径字符串、不移动/复制/删除文件。

### 3.7 强杀恢复（§24.7）
新增 `test_kill_recovery`（9 项，桌面）：fork 子进程写一批 + checkpoint 后 `_exit`（不 close/不析构，
等价进程被强杀），父进程重开验证 **WAL 崩溃恢复**读到 checkpoint 且可续跑。
（Android 分阶段 kill 因 `filesDir` 跨 gradlew 调用不持久而改用桌面等价验证，已删除不可靠的 instrumentation。）

## 4. 验证结果汇总

| 项 | 结果 |
|---|---|
| 桌面 CTest | **21/21**（含 `process_lock`/`migration_switch`/`kill_recovery`） |
| 桌面 ASan+UBSan | **21/21**（`outputs/kernel-round5-asan.log`） |
| 桌面 TSAN | db_command_queue + read_pool 无 race（`outputs/kernel-round5-tsan.log`） |
| Android 两 ABI 构建 | 通过（arm64-v8a / x86_64） |
| `ShadowMigrationTest`（AVD） | 3/3 |
| `RoomNativeFixtureTest`（AVD） | 3/3 |
| `DbKeyManagerFailCloseTest`（AVD） | 6/6 |
| `NativeDbLongRunTest`（AVD） | 3/3 |
| `MediaFileIntegrityTest`（AVD） | 1/1 |

## 5. 已知遗留（不阻塞完成判定）

1. **NDK TSAN**：runtime 存在（`libclang_rt.tsan-aarch64-android.so` + `wrap.sh`），技术上可行，
   但需专门构建变体 + runtime 打包 + AVD 协调，工程成本高；按 §24.9.3 以桌面 TSAN 作为替代证据。
2. **迁移性能指标**（§24.7）：默认批次 500 已固定、10 万导入 1.9s，但峰值内存与 Android 主线程
   帧率未采样。
3. **校验失败时 Legacy 仍可搜索**（§24.7）：未删旧库/不切 UI 已保证，但未显式断言迁移失败后
   Legacy Room 的搜索能力。

## 6. 下一步

进入 P7「消息 Outbox/Inbox」。P7 只消费本轮稳定的 `NativeDatabase` 命令与查询接口，不允许把
SQL、事务、去重或会话摘要逻辑搬回 JNI/Kotlin。
