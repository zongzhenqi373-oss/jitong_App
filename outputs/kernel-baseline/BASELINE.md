# 内核迁移基线（P0）

> 本目录是「QQNT 式薄 UI + 厚内核」迁移的**行为事实源**。
> 后续每个阶段（P3~P13）都必须以这里记录的 Android 现状为 Golden Test 对照：
> **Native 内核的行为必须与之一致；若必须不一致，要先修改本文件并说明原因。**

## 1. 基线用途

| 阶段 | 依赖的基线文件 |
|---|---|
| P4 安全通道 | `auth-baseline.md` §7~§8（TLS Pinning / AppSecureChannel） |
| P5 账户 Token | `auth-baseline.md` §1~§6 |
| P7 消息收发 | `message-baseline.md` §1~§3 |
| P8 漫游补洞 | `message-baseline.md` §4~§5 |
| P9 搜索 | `search-baseline.md` + `search-golden.json` |
| P10~P12 媒体 | `media-baseline.md` |

## 2. 基线采集环境

| 项 | 值 | 来源 |
|---|---|---|
| 客户端 | `jitong_android`，`versionName 0.5.0` | `app/build.gradle.kts` |
| `client_version` 上报值 | `android-0.5.0` | `ImClient.login()` |
| 协议事实源 | `protocol/im.proto`（Android/C++/Server 共用） | 单一事实源 |
| TCP 端口 | `24563` | `Protocol.TCP_PORT` |
| HTTPS 文件服务端口 | `24564` | `Protocol.HTTPS_FILE_PORT` |
| 默认服务端地址 | `10.0.2.2`（模拟器回环宿主机） | `ImClient.DEFAULT_HOST` |
| 帧格式 | `[4B 大端包长(含协议号)][4B 小端协议号][pb payload]` | `Frame.kt` / `Protocol.kt` |
| 单包上限 | `MAX_PACK_LEN = 10 * 1024 * 1024` | `Protocol.kt` |
| 应用层安全版本 | `APP_SECURITY_VERSION = 1` | `Protocol.kt` |

服务端版本需与迁移时运行的 `im_server` 提交对齐，执行基线前记录：

```text
im_server commit : <git rev-parse --short HEAD>
采集日期         : YYYY-MM-DD
采集人           :
```

## 3. 测试账号（**只写占位符，禁止提交真实凭证**）

基线文档与 Golden Case 中一律使用占位符，实际值只在本地 `local-secrets.env`（已 gitignore）中配置：

```text
TEST_TEL_A        = +86-138-0000-0001   # 发送方（写成带分隔符形式，避免被脱敏扫描误报）
TEST_TEL_B        = +86-138-0000-0002   # 接收方
TEST_PASSWORD     = ******              # 6~32 位，满足 validateCredentials 的长度规则
TEST_NICK_A       = baseline_a
TEST_NICK_B       = baseline_b
```

> 代码中校验规则：`^1[3-9]\d{9}$`（去分隔符后为 11 位），密码 `length in 6..32` 且非全空格。

本地私有配置文件（**不得提交**）：

```bash
# outputs/kernel-baseline/local-secrets.env
export TEST_TEL_A=...
export TEST_TEL_B=...
export TEST_PASSWORD=...
```

## 4. 数据清理方法

每次执行基线前必须回到干净状态，否则 seq/幂等/未读计数会被污染：

```bash
# 1) 停止服务端
pkill -f im_server

# 2) 清理服务端数据（按实际部署调整路径）
rm -f data/im.db data/im.db-wal data/im.db-shm
# 或保留库结构只清业务表：
#   DELETE FROM messages; DELETE FROM conversations;
#   DELETE FROM auth_sessions; DELETE FROM auth_refresh_history;

# 3) 重启服务端
cmake --build build/im_server && ./build/im_server/im_server

# 4) 客户端清状态
adb shell pm clear com.jitong.im          # 清 MMKV（Token/deviceId）+ Room + 媒体缓存
adb shell rm -rf /sdcard/Android/data/com.jitong.im/files/media
```

> `adb shell pm clear` 会重置 `deviceId`。若用例依赖固定 deviceId（DeviceProof 相关），
> 改用「应用内退出登录」并保留 MMKV，然后在基线记录里注明 deviceId 是否复用。

## 5. 脱敏约定（**硬门禁**）

以下内容**禁止**出现在 `outputs/kernel-baseline/`、`client_core/tests/golden/`、任何提交中：

```text
- 真实手机号、昵称
- access_token / refresh_token / session_id 明文
- 密码明文或 sha256(pass) 结果
- 设备私钥、Ed25519/P-256 私钥、Keystore 导出物
- 数据库解密密钥（DB key / wrapped key）
- 消息正文原文（Golden Case 只用「语义占位正文」或固定无意义样本）
```

**例外（这些本来就是公开值，允许记录）**：

```text
- SPKI pin（证书公钥哈希，随 App 分发，见 TlsPinning.CURRENT_PIN）
- 服务端 Ed25519 身份公钥（见 AppIdentityPins）
- AVIF/libavif 等第三方库的公开标识
```

允许记录的内容：

```text
- 字段名、字段号、类型、方向
- 协议号、结果码、常量数值
- 状态迁移序列（如 SENDING → DELIVERED）
- 结构化的「是否命中 / 命中条数 / 高亮区间下标」这类无量纲判定
```

检查方式（提交前必跑）：

```bash
cd <repo>
# 1) 手机号（11 位连续数字）
! grep -rnE '1[3-9][0-9]{9}' outputs/kernel-baseline client_core/tests/golden
# 2) 疑似 token：32 位以上连续串，排除文件路径（含 /）与已白名单的公开 pin/公钥/常量名
grep -rnoE '[A-Za-z0-9+/=_-]{32,}' outputs/kernel-baseline client_core/tests/golden \
  | grep -v '/' \
  | grep -vE 'co0kqhe8Yl91qzqL9q9XOEUKNgSZABAgJEE78595QSE=|hdThT8vmFpBUsYmo7jHa86Knht\+cZFyulsdRYHNPUHI=|APP_CIPHER_X25519_ED25519_HKDF_SHA256_AES_256_GCM'
echo "baseline scan: OK"
```

## 6. 目录说明

```text
client_core/tests/golden/            内核侧 Golden 测试（C++），读下面同一份用例
jitong_android/app/src/androidTest/  Android 侧 Instrumented 基线导出/回归测试
outputs/kernel-baseline/
├── BASELINE.md          本文件
├── auth-baseline.md     P0-T02 登录 / Token / 防重入 / 重连
├── message-baseline.md  P0-T03 消息 / 漫游 / 幂等 / 排序
├── search-baseline.md   P0-T04 搜索策略与高亮规则
├── search-golden.json   P0-T04 ≥50 条固定搜索用例（可重复执行）
└── media-baseline.md    P0-T05 图片 / 缩略图 / 秒传 / 传输
```

## 7. 基线的更新规则

1. **先改 Android 再改基线**：任何行为变更先在 Kotlin 侧验证，再同步本目录。
2. **Golden 用例只增不改**：`search-golden.json` 的 `id` 一经分配不得复用；行为变更时新增用例并标记 `supersedes`。
3. **Native 与基线不一致即失败**：不允许「顺手改基线让它过」，必须在评审中确认是有意变更。
