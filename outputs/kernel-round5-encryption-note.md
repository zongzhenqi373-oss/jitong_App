# P6 加密底座说明与关键决策记录

> 对应 `outputs/jitong-qqnt-kernel-execution-plan.md` 第 24 章（P6-T01）与
> `outputs/kernel-round5-execution-plan.md` 的 S0/S1。
> 记录：SQLCipher 接入方式、链接策略决策、cipher 参数、体积基线、验证证据。

---

## 1. S0 决策记录：T01 链接策略 = **方案 A（复用 AAR 的 `libsqlcipher.so`）**

第 24 章 24.3 要求"优先复用同一版本、同一份 SQLCipher shared object；如果 AAR 不提供稳定的
Native 链接入口，才构建私有 SQLCipher"。S0 已用实测证伪/证实，结论：

### 1.1 判定依据（实测）

| 检查项 | arm64-v8a | x86_64 | 结论 |
|---|---|---|---|
| `SONAME` | `libsqlcipher.so` | 同 | 稳定入口 |
| `DT_NEEDED` | `libdl / liblog / libc / libm` | 同 | **无 libcrypto 依赖**（crypto 已静态编入） |
| 导出 `sqlite3_*` 符号数 | 294 | 294 | API 完整 |
| `sqlite3_key` / `sqlite3_key_v2` / `sqlite3_rekey` | 均有 | 均有 | **codec 已启用**（`SQLITE_HAS_CODEC`） |
| `sqlite3_open_v2 / prepare_v2 / step / column_text / exec / close / errmsg` | 均有 | 均有 | 满足全部所需 API |
| 编译+链接（`-Wl,--no-undefined`） | 通过 | 通过 | 无未定义符号 |

### 1.2 设备上实跑（arm64 AVD，Android 14）

```text
[cipher]   PRAGMA cipher_version -> 4.6.1 community
[ddl]      CREATE TABLE t(id INTEGER PRIMARY KEY, body TEXT) -> ok
[insert]   INSERT INTO t(body) VALUES('P6_MARKER_9F3A2B71') -> ok
[read-ok]  SELECT body FROM t WHERE id=1 -> P6_MARKER_9F3A2B71
[wrong-key] rc=26 (EXPECTED: 读取失败)
TRIAL RESULT: OK
```

**关键结论：**

1. **版本与 Room 完全一致**：`4.6.1 community` == `net.zetetic:sqlcipher-android:4.6.1`
   （`app/build.gradle.kts:172`）。
2. **加密真实生效**：错误 key 返回 `rc=26`（`SQLITE_NOTADB`），读取失败。
3. **key 使用约定已确定（见第 4 节）**：**32 字节 key 材料直接交给 `sqlite3_key`**（口令模式，
   由 KDF 派生实际密钥），与 Room 的 `SupportOpenHelperFactory(key)` 一致。
   ⚠️ **不要**使用 SQLCipher 的 raw-key 字面量 `x'<64hex>'`。
4. **进程内只有一份 SQLCipher**：我们不编入自己的副本，因此**不存在双库符号冲突**，
   24.3 的"符号隔离"验收天然满足（仍需用 `nm -D` 确认 `jitong_kernel.so` 不*导出* `sqlite3_*`）。

### 1.3 方案 A 的收益

- **体积增量为 0**：`libsqlcipher.so` 早已随 AAR 打进 APK（成本已付），我们只是新增 `DT_NEEDED`。
- **cipher 参数天然与 Room 一致**（同一实现、同一默认参数），无参数串扰风险。
- 无需新增 `scripts/build-sqlcipher-android.sh` 与预编译产物。

### 1.4 遗留风险与对策（S1 必须验证）

| 风险 | 对策 |
|---|---|
| Android linker namespace：`libsqlcipher.so` 由 Room 通过 `System.loadLibrary` 加载，我们的 `.so` 以 `DT_NEEDED` 依赖它 | S1 在设备/AVD 上验证：不显式 `System.loadLibrary("sqlcipher")` 也能加载 `jitong_kernel.so`（linker 自动解析依赖）；若不成立，退化为在 JNI 初始化时显式加载 |
| AAR 升级可能改变导出符号/版本 | 增加启动自检：`PRAGMA cipher_version` 必须匹配预期版本，否则 fail-close |
| 我们可能意外*导出* `sqlite3_*` | 编译加 `-fvisibility=hidden`；S1 用 `nm -D` 断言 `jitong_kernel.so` 不导出 `sqlite3_*` |

---

## 2. 体积基线（用于 24.3 体积增量验收）

| 项 | 字节 |
|---|---|
| `app-debug.apk` | 98,427,561 |
| `lib/arm64-v8a/libjitong_kernel.so` | 6,476,112 |
| `lib/x86_64/libjitong_kernel.so` | 6,720,808 |
| `lib/arm64-v8a/libsqlcipher.so`（已存在，非新增） | 5,797,528 |
| `lib/x86_64/libsqlcipher.so`（已存在，非新增） | 6,372,968 |

**方案 A 预期增量**：仅 `DT_NEEDED` 条目与若干导入符号，量级 < 数 KB。

---

## 3. 已实测的 cipher 参数（AAR 默认值 == Room 使用的参数）

来源：arm64 AVD（Android 14）上用 AAR 的 `libsqlcipher.so` 实跑探测（`trial2`），
即本工程 Room（`sqlcipher-android 4.6.1`）实际使用的默认值。已写入
`client_core/src/storage/CipherParams.h` 作为固化常量与启动自检依据。

| 参数 | 实测值 |
|---|---|
| `cipher_version` | `4.6.1 community` |
| `cipher_page_size` | `4096` |
| `page_size` | `4096` |
| `kdf_iter` | `256000` |
| `cipher_kdf_algorithm` | `PBKDF2_HMAC_SHA512` |
| `cipher_hmac_algorithm` | `HMAC_SHA512` |
| `cipher_use_hmac` | `1` |
| `cipher_plaintext_header_size` | `0` |
| `journal_mode` | `wal`（可用） |

## 3.1 已实测的编译特性（决定 T03 可行性）

`PRAGMA compile_options` 关键项：

```text
HAS_CODEC  THREADSAFE=1  ENABLE_FTS3  ENABLE_FTS4  ENABLE_FTS5
ENABLE_RTREE  ENABLE_STAT4  ENABLE_DBSTAT_VTAB  ENABLE_PREUPDATE_HOOK
DEFAULT_PAGE_SIZE=4096  MAX_PAGE_SIZE=65536  SYSTEM_MALLOC  TEMP_STORE=2
```

- **FTS4 可用**（T03 的 `messages_fts` 依赖）— 已验证：
  `CREATE VIRTUAL TABLE ... USING fts4(content,pinyin,initials,msg_id)` → 插入 →
  `MATCH 'nihaoshijie'` 命中 → `count(*)=1`。
- **WAL 可用**。
- 桌面侧 amalgamation 编译时需开启等价特性：`SQLITE_HAS_CODEC`、
  `SQLITE_ENABLE_FTS3`、`SQLITE_ENABLE_FTS4`、`SQLITE_THREADSAFE=1`、
  `SQLITE_TEMP_STORE=2`，以保证桌面单测与 Android 行为一致。

## 3.2 S1-3 双向互开验证：**已通过**

`androidTest` `RoomNativeFixtureTest`（arm64 AVD，Android 14），key 取自真实
`DbKeyManager.getOrCreateRealKey()`（测试专用 ownerId，32 字节）：

```text
Starting 3 tests on Pixel_7(AVD) - 14
Finished 3 tests on Pixel_7(AVD) - 14          # 全部通过
```

- [x] **Native 建 → Room 读**：`CipherDatabase` 建库写 marker，
      Room 的 `SupportOpenHelperFactory` 打开并读出 `NATIVE_FIXTURE_MARKER_3E7B`
- [x] **Room 建 → Native 读**：Room 建库写表，Native 只读打开成功，
      回报 `cipher=4.6.1 community`、`tables>=1`
- [x] **错误 key fail-close**：返回 `NotEncrypted`，且**文件字节完全不变**（不删库、不覆盖）

---

## 4. key 使用约定（**实测确定，勿改**）

### 4.1 结论

**把 32 字节 key 材料直接传给 `sqlite3_key(db, key32.data(), 32)`。**

即：key 被当作**口令（passphrase）**，由 SQLCipher 用 KDF
（`PBKDF2_HMAC_SHA512`，256000 轮）派生出实际的加密密钥。

这与 Room 侧 `net.zetetic:sqlcipher-android` 的
`SupportOpenHelperFactory(key: ByteArray)` 行为一致，因此同一份加密库可被两边互开。

### 4.2 被排除的方案（**重要教训**）

最初在 S0 的独立试验中，我用 SQLCipher 的 **raw-key 字面量** `x'<64 位 hex>'`：

```c
sqlite3_key(db, "x'000102...1f'", 67);   // ❌ 与 Room 不兼容
```

该形式在**单独自测**时完全正常（建库/读写/错 key 失败都对），因此一度被误认为正确约定。
但 S1-3 双向验证暴露了问题：

```text
Room 打开 Native 库 → SQLiteException: file is not a database (code 26)
Native 打开 Room 库 → err|NotEncrypted|... file is not a database
```

原因：`x'<hex>'` 走的是 SQLCipher 的 **raw-key 模式**（跳过 KDF、直接用该字节作密钥），
而 Room 走的是 **passphrase 模式**，两者派生出的实际密钥不同 → 互开必然失败。

**教训：单侧自测无法验证互操作性。**任何"参数/编码约定"都必须用
**对方实现的真实产物**做双向验证，不能只看自己能打开自己的库。

### 4.3 固化位置

- 实现：`client_core/src/storage/CipherDatabase.cpp` → `CipherDatabase::open()`
- 注释警示：`client_core/include/client_core/storage/CipherDatabase.h` 文件头
- 回归防护：`RoomNativeFixtureTest`（androidTest，设备）+ `test_cipher_database`（桌面）

---

## 4. 复现命令

```bash
# 提取 AAR/APK 中的 libsqlcipher.so
unzip -o -q app/build/outputs/apk/debug/app-debug.apk 'lib/*/libsqlcipher.so' -d /tmp/sqlcipher_probe

# 导出符号检查（两 ABI 均须 YES）
NB=$HOME/Library/Android/sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/darwin-x86_64/bin
$NB/llvm-nm -D --defined-only /tmp/sqlcipher_probe/lib/arm64-v8a/libsqlcipher.so \
  | grep -E ' T (sqlite3_open_v2|sqlite3_key|sqlite3_prepare_v2)$'

# 依赖与 SONAME
$NB/llvm-readelf -d /tmp/sqlcipher_probe/lib/arm64-v8a/libsqlcipher.so | grep -E 'NEEDED|SONAME'

# 编译链接（两 ABI）
$NB/aarch64-linux-android34-clang -DSQLITE_HAS_CODEC trial.c -o trial_arm64 \
  -L/tmp/sqlcipher_probe/lib/arm64-v8a -lsqlcipher -Wl,--no-undefined -llog

# 设备上实跑
adb -s emulator-5554 push libsqlcipher.so /data/local/tmp/
adb -s emulator-5554 push trial_arm64 /data/local/tmp/trial
adb -s emulator-5554 shell "chmod 755 /data/local/tmp/trial && cd /data/local/tmp && LD_LIBRARY_PATH=/data/local/tmp ./trial"
```
