# P0-T02 登录 / Token / 防重入 / 重连基线

> 事实来源：`jitong_android` 当前 Kotlin 实现。所有字段名、协议号、常量均为代码实测值。
> 脱敏：只记录字段名与类型，不记录任何真实取值。

## 0. 参与者与代码位置

| 角色 | 文件 | 职责 |
|---|---|---|
| UI | `ui/LoginScreen.kt` | 收集输入，按钮 `enabled = !authRunning` |
| 编排 | `ui/MainViewModel.kt` | `login()` / `startReconnect()` / `refreshTokenOnce()` / 事件收口 |
| 单飞 | `ui/AuthCoordinator.kt` | 同一时刻只允许一个认证流程 |
| 协议 | `net/ImClient.kt` | `connect()` / `login()` / `loginWithToken()` / `refreshToken()` |
| 安全 | `net/SecureChannel.kt` / `net/TlsPinning.kt` / `net/AppIdentityPins.kt` | 应用层握手、SPKI Pin、Ed25519 身份 Pin |
| 设备 | `net/DeviceProof.kt` / `data/crypto/DeviceIdentity.kt` | P-256 设备签名 |
| 存储 | `data/Prefs.kt`（MMKV）+ `data/crypto/TokenVault.kt` | TokenSession 加密 KV |

## 1. 密码登录

### 1.1 调用链

```text
LoginScreen:75  vm.login(tel, pass, remember)
  → MainViewModel.login()
      ├─ normalizeTel = tel.trim()
      ├─ validateCredentials()          # 手机号 ^1[3-9]\d{9}$ ；密码 length in 6..32 且非全空格
      ├─ if (auth.isRunning) return     # 单飞短路
      ├─ lastHash = sha256Hex(pass)     # 仅本地 DB 派生用，不作网络凭证
      └─ auth.submit { ... }
           ├─ Prefs.clearTokenSession()
           ├─ Prefs.pendingRefreshRequestId = null
           ├─ client.connect(DEFAULT_HOST)        # "10.0.2.2" : 24563
           ├─ auth.markAuthenticating()
           └─ client.login(normalizedTel, pass, Prefs.deviceId)
```

### 1.2 连接前置（**顺序不可交换**）

```text
SSLContext.getDefault() → sslSocket.connect(addr, 5000)
  → enabledProtocols = ["TLSv1.3"]
  → endpointIdentificationAlgorithm = "HTTPS"   # 域名校验
  → tcpNoDelay / keepAlive = true ；soTimeout = 0
  → TlsPinning.verify(session)                  # 任何凭证帧之前
  → SecureChannel.establish()                   # 应用层握手，见 §9
  → startHeartbeat() + readLoop()
```

### 1.3 协议

`LoginRq`（`LOGIN_RQ = 1002`）

| 字段 | 号 | 类型 | 取值来源 |
|---|---|---|---|
| `tel` | 1 | string | 用户输入（trim 后） |
| `pass` | 2 | string | `sha256Hex(pass)` |
| `device_id` | 3 | string | `Prefs.deviceId`（UUID，首次启动生成后复用） |
| `device_name` | 4 | string | `"${Build.MANUFACTURER} ${Build.MODEL}"` |
| `client_version` | 5 | string | `"android-0.5.0"` |
| `device_public_key` | 6 | bytes | `DeviceIdentity.publicKey()`（P-256） |
| `device_signature` | 7 | bytes | Keystore sign(DeviceProof.message(...)) |

`LoginRs`（`LOGIN_RS = 1003`）

| 字段 | 号 | 类型 |
|---|---|---|
| `userid` | 1 | int32 |
| `result` | 2 | int32 |
| `access_token` | 3 | string |
| `refresh_token` | 4 | string |
| `access_token_expire_at` | 5 | int64 |
| `refresh_token_expire_at` | 6 | int64 |
| `session_id` | 7 | string |

### 1.4 结果处理

`ImClient.dispatch()` 构造 `TokenSession(userId, sessionId, accessToken, refreshToken, accessExpiresAt, refreshExpiresAt)`；`result != LOGIN_SUCCESS` 时 `tokenSession = null`。

| result | 常量 | UI 行为 |
|---|---|---|
| 0 | `LOGIN_SUCCESS` | 存 Token、取消重连、按 `remember` 持久化账号密码、进入 `FriendList`、`openStore(myId)`、`roamConversations()`、`loadFriendRequests()` |
| 1 | `LOGIN_NOTEXIT` | 提示「用户不存在」 |
| 2 | `LOGIN_PASSERROR` | 提示「密码错误」 |
| 3 | `LOGIN_INVALID` | 提示「手机号或密码格式不正确」 |
| 4 | `LOGIN_RATE_LIMITED` | 提示「登录尝试次数过多」+ toast |

失败统一 `auth.onCompleted(false)`。

## 2. Token 登录

### 2.1 触发条件（**全链路仅两处，且只在重连路径**）

1. `startReconnect()` 内 `session.accessExpiresAt > nowSeconds + 30L` → `loginWithToken(session, deviceId)`
2. refresh 成功后 `loginWithToken(refreshed, deviceId)`

> **现状结论：Android 没有实现冷启动自动 Token 登录**（`Prefs` 注释明确「不做自动登录，回填后仍需用户手动点登录」）。
> 这与 V3 计划 §6.2「UI 只调用 `sdk.start()`」不同 —— **Native 内核需要新增此能力**，属于有意的基线扩展，在 P5 阶段记录。

### 2.2 协议

`TokenLoginRq`（`TOKEN_LOGIN_RQ = 1023`）

| 字段 | 号 | 类型 | 说明 |
|---|---|---|---|
| `access_token` | 1 | string | — |
| （2 跳过） | — | — | 字段号 2 未使用，不可复用 |
| `device_id` | 3 | string | — |
| `request_id` | 4 | string | `UUID.randomUUID()`，**即用即生成，不落盘、不复用** |
| `device_signature` | 5 | bytes | `DeviceProof.message("token-login", sessionId, deviceId, accessToken.toByteArray())`；**不传 publicKey** |

`TokenLoginRs`（`TOKEN_LOGIN_RS = 1024`）：`result(1,int32)`、`userid(2,int32)`、`access_token_expire_at(3,int64)`。
**不返回 token，不返回 session_id**（复用旧 sessionId）。

成功判定：`result == LOGIN_SUCCESS(0)` → `myId = rs.userid`，清重连状态，toast「已重新连接」，`roamConversations()`。

## 3. Refresh

### 3.1 触发点（**唯一**）

`MainViewModel.refreshTokenOnce(session)`，条件：

```text
session.accessExpiresAt <= nowSeconds + 30L     # 30s 安全窗口
&& session.refreshExpiresAt > nowSeconds
```

前置短路（不发请求，直接返回 null）：`refreshToken.isBlank()` 或 `refreshExpiresAt <= now` 或 `Prefs.loadTokenSession() == null`。

> **现状缺口**：HTTP 文件服务（`HttpMediaClient`）只取 accessToken，**不主动触发 refresh**；
> 没有定时刷新、没有 401 重试。P5 的 `TokenManager` 需补齐这几类触发（V3 §6.3），属有意扩展。

### 3.2 Single-Flight

```text
refreshMutex = Mutex()
refreshAwaiter: CompletableDeferred<TokenSession?>?
refreshRequestInFlight: Boolean
```

- `obtainRefreshDeferred()`：`refreshMutex.withLock` 内若 `refreshAwaiter != null` → **复用同一 Deferred**；否则新建、发帧。
- **等待必须在锁外**（锁内等待会死锁，因为 `complete` 也要同一把锁）。
- 发帧异常 → 锁内复位 `refreshRequestInFlight=false` / `refreshAwaiter=null` / `complete(null)`，锁外 throw。
- 统一收口：`TokenRefreshResult` 事件在**同一把锁内** `complete(success ? refreshed : null)`，所有等待者拿到同一结果。

### 3.3 requestId

- 生成：`Prefs.pendingRefreshRequestId ?: UUID.randomUUID().toString().also { Prefs.pendingRefreshRequestId = it }`
- 持久化：MMKV key `"pendingRefreshRequestId"`
- **断线时刻意保留**（`Disconnected` 分支只 cancel `refreshAwaiter`、复位 inFlight），重连后用同一 requestId 幂等重试
- 清空时机：refresh 成功 / refresh 失败 / 显式密码登录 `login()` / `Prefs.clearTokenSession()`
- 注意：`resetToLogin()` **不清** `pendingRefreshRequestId`（已记录为现状行为，P5 需确认是否有意）

### 3.4 协议

`RefreshTokenRq`（`TOKEN_REFRESH_RQ = 1025`）：`refresh_token`、`device_id`、`request_id`、`device_signature`
`RefreshTokenRs`（`TOKEN_REFRESH_RS = 1026`）：`result(1)`、`access_token`、`refresh_token`、`access_token_expire_at`、`refresh_token_expire_at`、`session_id`

成功（`REFRESH_TOKEN_SUCCESS = 0`）且本地仍有 session 时构造新 `TokenSession`（`userId` 取旧值，因为响应无 userid）。

### 3.5 轮换与吊销

- 服务端 `TokenService.rotateRefresh`：`m_refreshMtx` + `m_refreshResults[requestId]` 缓存（TTL 120s，上限 1024 条）
- **重用检测**：`findByRefreshHash` 失败 → `wasRefreshTokenUsed(oldHash, familyId)` 命中 → `revokeTokenFamily(familyId)`（`UPDATE auth_sessions SET revoked=1 WHERE family_id=?`）
- 服务端对被吊销的在线旧连接下发 `KICKED_OFFLINE(1012)` 并 `closeAfterWrite()`
- **客户端结果码只有 `REFRESH_TOKEN_SUCCESS=0` / `REFRESH_TOKEN_FAIL=1`**，无独立「家族吊销」码；任何非 0 走同一清理：
  `Prefs.pendingRefreshRequestId = null` → `Prefs.clearTokenSession()` → toast → `auth.onCompleted(false)` → `resetToLogin()`

## 4. Token 持久化

| 项 | 值 |
|---|---|
| 存储 | MMKV `defaultMMKV()` |
| key | `tokenSessionEnc`（bytes）、`pendingRefreshRequestId`（string）、`deviceId`（string）、`tel`/`passEnc`/`remember` |
| 加密 | `TokenVault.encrypt(plain)`，明文为 6 段 `\n` 拼接：`userId\n sessionId\n accessToken\n refreshToken\n accessExpiresAt\n refreshExpiresAt` |
| 解密失败 | `clearTokenSession()` 并返回 null |
| 段数校验 | `split('\n').size != 6` → 清除并返回 null |

## 5. 防重入

两层：

1. **认证级单飞** `AuthCoordinator.submit()`：`@Synchronized`，`if (isRunning || job?.isActive == true) return`。
   阶段：`Idle → Connecting → Authenticating → Authenticated / Failed`；`isRunning = (Connecting || Authenticating)`。
   覆盖「已提交但协程未调度」与「请求已发出、响应未回」两个窗口。
2. **VM 层** `MainViewModel.login()` 额外 `if (auth.isRunning) return`；`startReconnect()` 内 `if (!auth.isRunning)` 才发起 token 登录。

> UI 按钮 `enabled` 只是体验优化，**正确性由 `AuthCoordinator` 保证**。V3 §6.4 要求相同语义。

## 6. 重连与自动登录

```text
startReconnect():
  if (reconnecting) return
  if (Prefs.loadTokenSession() == null) { resetToLogin(); return }
  reconnecting = true
  attempt = 0 ; maxAttempts = 10
  loop:
    attempt++
    backoffMs = min(1000 * 2^(attempt-1), 15000)      # 1s,2s,4s,8s,15s,15s...
    delay(backoffMs)
    client.disconnect()                                # 清半开连接
    ok = client.connect(DEFAULT_HOST)
    if (ok && !auth.isRunning):
        session = loadTokenSession()
        if (session.accessExpiresAt > now + 30)  → loginWithToken(session)
        else if (session.refreshExpiresAt > now) → refreshTokenOnce(session) → loginWithToken(refreshed)
        else                                     → clearTokenSession() + resetToLogin()
    delay(3000)                                        # 每轮之间固定 3s
  用尽 10 次仍失败 → toast「重连失败，请重新登录」+ resetToLogin()
```

`Disconnected` 事件：`expectDisconnect == false && screen != Login` 且有 Token → `startReconnect()`；否则 toast「登录状态已失效」+ `resetToLogin()`。

## 7. DeviceProof

```text
canonical = "jitong-device-proof-v1" || LP(f1) || f1 || LP(f2) || f2 || ... || LP(f5) || f5
  LP(x) = 4 字节大端长度
  f1 = operation            ("password-login" | "token-login" | ...)
  f2 = appSessionId         (SecureChannel sessionId, 16B)
  f3 = deviceId             (UTF-8)
  f4 = credentialBinding    (password-login: "$tel\u0000$sha256Hex(pass)"；token-login: accessToken.toByteArray())
  f5 = publicKey            (password-login 传；token-login 传空 ByteArray)
签名算法：P-256（Keystore）
```

## 8. TLS Pinning

| 项 | 值 |
|---|---|
| 固定对象 | **SPKI SHA-256**（`X509Certificate.publicKey.encoded` 的 SHA-256），非整张证书 |
| 当前 pin | `sha256/co0kqhe8Yl91qzqL9q9XOEUKNgSZABAgJEE78595QSE=`（base64） |
| 校验时机 | TLS 握手后、**任何凭证帧之前** |
| 失败 | 抛 `SSLPeerUnverifiedException("服务端 SPKI 固定校验失败")` → 关闭连接，**不回退** |
| 无证书 | `SSLPeerUnverifiedException("服务端未提供可验证证书")` |

## 9. 应用层安全通道（SecureChannel）

### 9.1 常量

```text
APP_SECURITY_VERSION = 1
APP_CIPHER_SUITE_V1  = 1   (APP_CIPHER_X25519_ED25519_HKDF_SHA256_AES_256_GCM)
APP_X25519_KEY_LEN   = 32
APP_NONCE_LEN        = 32   (握手 nonce)
APP_RANDOM_ID_LEN    = 16
APP_SESSION_ID_LEN   = 16
APP_ED25519_SIGNATURE_LEN = 64
APP_FINISHED_LEN     = 32
APP_GCM_TAG_LEN      = 16
APP_GCM_NONCE_LEN    = 12   (帧 nonce 总长 = 4B prefix + 8B seq)
APP_MAX_HANDSHAKE_PAYLOAD = 1024
HANDSHAKE_TIMEOUT_MS = 15000
```

### 9.2 握手流程

```text
C: AppClientHello(1036) { version, clientEphemeralPublicKey(32B), clientNonce(32B),
                          clientRandomId(16B), cipherSuite }
S: AppServerHello(1037) { version, serverEphemeralPublicKey(32B), serverNonce(32B),
                          sessionId(16B), keyId, cipherSuite, signature(64B) }
   客户端校验：version / cipherSuite / 三个长度 / keyId != 0
   身份公钥：AppIdentityPins.publicKey(keyId)  # keyId=1 → Ed25519 公钥
   signatureInput = "jitong-app-handshake-v1" || u32(len(clientPayload)) || clientPayload
                    || u32(version) || serverPublic || serverNonce || sessionId
                    || u32(keyId) || u32(cipherSuite)
   ed25519Verify(identityPub, signatureInput, signature) 失败 → 关闭
   transcriptHash = SHA256(u32(len(clientPayload)) || clientPayload || u32(len(serverPayload)) || serverPayload)
   sharedSecret = X25519(ephemeralPriv, serverPublic)
   salt = SHA256(clientNonce || serverNonce)
   info = "jitong-app-channel-v1" || sessionId || transcriptHash
   material = HKDF-SHA256(sharedSecret, salt, info, 136)
     [0,32)   clientToServerKey
     [32,64)  serverToClientKey
     [64,68)  clientNoncePrefix
     [68,72)  serverNoncePrefix
     [72,104) clientFinishedKey
     [104,136) serverFinishedKey
   clientVerify = HMAC-SHA256(clientFinishedKey, transcriptHash)
C: AppFinished(1038) { verifyData = clientVerify }
S: AppFinished(1039) { verifyData }
   expected = HMAC-SHA256(serverFinishedKey, transcriptHash || clientVerify)
   常时比较，长度必须 == 32
```

### 9.3 业务帧保护

```text
encrypt(type, payload):
   type 不得落在 1036..1040
   sequence = ++sendSequence（从 1 开始；== Long.MAX_VALUE 时抛「序列号耗尽」）
   plaintext = le32(type) || payload
   nonce = clientNoncePrefix(4B) || u64be(sequence)(8B)      # 共 12B
   aad = "jitong-app-frame-v1" || u32be(version) || sessionId || u64be(sequence)
   → AES-256-GCM → AppEncryptedFrame{ version, sessionId, sequence, ciphertext, tag }
   → Frame(1040, ...)
   明文/nonce/aad 用后立即 fill(0)

decrypt(frame):
   frame.type 必须 == 1040，否则「收到明文业务帧」
   校验 version / 常时比较 sessionId / tag.size == 16 / ciphertext.size >= 4
   sequence 必须 == receiveSequence + 1（严格递增，无乱序容忍）
   内嵌 innerType 不得落在 1036..1040；plaintext.size >= 4
```

> **关键点**：sequence 严格 +1、收发 sequence 分离独立计数、AAD 含 sessionId+sequence、无重放窗口。

## 10. Golden 场景矩阵

> 每种场景给出：输入 → 状态变化序列 → 协议类型 → 最终结果。
> `S` 表示 `AuthPhase`，`N` 表示网络协议序列。

| ID | 场景 | 输入 | 状态变化 | 协议序列 | 期望结果 |
|---|---|---|---|---|---|
| A-01 | 密码登录成功 | 合法 tel/pass | `Idle→Connecting→Authenticating→Authenticated`；`Screen.Login→FriendList` | `LOGIN_RQ(1002)` → `LOGIN_RS(1003,result=0)` | TokenSession 落 MMKV；`pendingRefreshRequestId` 清空；进入好友列表 |
| A-02 | 密码错误 | 合法 tel / 错 pass | `→Authenticating→Failed` | `LOGIN_RQ` → `LOGIN_RS(result=2)` | 提示「密码错误」；不落 Token；`auth.onCompleted(false)`；可立即重试 |
| A-03 | 用户不存在 | 未注册 tel | `→Authenticating→Failed` | `LOGIN_RQ` → `LOGIN_RS(result=1)` | 提示「用户不存在」 |
| A-04 | 格式非法 | 非法 tel（非 `^1[3-9]\d{9}$`）或 pass 长度越界 | `Idle`（无网络） | 无 | 本地校验拦截，`_loginTip` 提示；**不建连** |
| A-05 | 限流 | 高频错误登录 | `→Authenticating→Failed` | `LOGIN_RQ` → `LOGIN_RS(result=4)` | 提示「登录尝试次数过多」+ toast |
| A-06 | Token 登录成功 | 有效 access（>30s 有效期） | `Connecting→Authenticating→Authenticated` | `TOKEN_LOGIN_RQ(1023)` → `TOKEN_LOGIN_RS(1024,result=0)` | toast「已重新连接」；触发 `ROAM_CONV_RQ` |
| A-07 | Token 过期 | access 进入 30s 窗口 | `→Authenticating`（挂起等 refresh） | 不发 `TOKEN_LOGIN_RQ`；先 `TOKEN_REFRESH_RQ(1025)` → `TOKEN_REFRESH_RS(1026,result=0)` → `TOKEN_LOGIN_RQ` | 轮换后登录成功；旧 refresh 作废 |
| A-08 | Refresh 过期 | `refreshExpiresAt <= now` | `→Failed` | 无 refresh 请求 | `clearTokenSession()` + `resetToLogin()`；回登录页 |
| A-09 | Refresh 重用（家族吊销） | 用已轮换的旧 refresh | `→Failed` | `TOKEN_REFRESH_RQ` → `result=1` | 客户端清 Token + `resetToLogin()`；服务端 `revokeTokenFamily` 并向旧连接发 `KICKED_OFFLINE(1012)` |
| A-10 | 被踢下线 | 另一设备登录同账号 | `Authenticated→Idle`；`Screen→Login` | 收到 `KICKED_OFFLINE(1012)` | `expectDisconnect=true`；`clearTokenSession()` + `clearCredentialsIfNotRemember()` + `disconnect()`；**不触发重连** |
| A-11 | 连点登录 | 200ms 内点击登录 5 次 | 仅第一次进入 `Connecting`，后续被短路 | **仅 1 组** `LOGIN_RQ` | 只产生一个认证链路、一条连接 |
| A-12 | 断线自动重连 | 服务端重启，Token 仍有效 | `reconnecting=true`；`loginTip`「正在重连（第 N 次）」 | 退避 1/2/4/8/15/15…s，最多 10 轮；每轮 `TOKEN_LOGIN_RQ` | ≤10 轮内恢复；恢复后 `reconnecting=false` |
| A-13 | 重连用尽 | 服务器持续不可用 | `→Login` | 10 轮后无 `LOGIN_RS` | toast「重连失败，请重新登录」+ `resetToLogin()` |
| A-14 | 断网期间 refresh | `Disconnected` 且 refresh 在飞 | `refreshAwaiter.cancel()`；inFlight=false | — | **保留** `pendingRefreshRequestId`，重连后复用同一 requestId 重试（服务端幂等缓存 TTL 120s） |
| A-15 | SPKI 不匹配 | 中间人证书 | 连接建立后立即断开 | TLS 握手后、**无任何应用层帧** | 抛 `SSLPeerUnverifiedException`；不重试到下一 pin；连接关闭 |
| A-16 | ServerHello 签名错误 | 伪造服务器 | 握手失败 | `APP_CLIENT_HELLO(1036)` → `APP_SERVER_HELLO(1037)` 后中止 | 抛「服务端应用身份签名验证失败」；无 `AppFinished`；连接关闭 |
| A-17 | 密文篡改 | 修改 ciphertext/tag | — | `APP_ENCRYPTED_FRAME(1040)` | AES-GCM 认证失败 → 关闭连接；**不解析业务包** |
| A-18 | 重放 sequence | 重发上一帧 | — | `APP_ENCRYPTED_FRAME(1040)` | `sequence != receiveSequence+1` → 关闭连接 |

## 11. Native 内核需补齐的差异（P5 阶段对照）

| 编号 | Android 现状 | V3 要求 | 处理 |
|---|---|---|---|
| D-01 | 无冷启动自动 Token 登录 | `sdk.start()` 自动选择 | **新增能力**（有意扩展） |
| D-02 | HTTP 文件服务不触发 refresh | 401 触发 refresh | **新增能力** |
| D-03 | 无定时/前台检查刷新 | App 回前台、网络恢复时检查 | **新增能力** |
| D-04 | `resetToLogin()` 不清 `pendingRefreshRequestId` | 状态机应明确 | P5 评审确认后统一 |
| D-05 | Token 登录不传 `device_public_key` | 与服务端一致 | 保持现状（与服务端对齐） |
