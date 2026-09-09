# 第四轮（P5 认证域 接线 + T06 Android 接入）验收报告

> 范围：接续 P5，完成 **AccountSession 接线层** 与 **T06（正式 JNI + JitongSdk + UI 旁路）**。
> 执行日期：2026-09-08
> 结论：**认证域内核已在生产链路接线并接入 Android（C++→JNI→SDK→Controller 全链路编译通过、
> arm64 AVD 冒烟通过、桌面单测 + ASan/UBSan 全绿）。** UI 保留 Legacy 回退，未魔改巨型
> MainViewModel。

---

## 1. AccountSession 接线层

`AccountSession`（`client_core/src/account/AccountSession.cpp`）把 P5 五个内核组件
（AuthStateMachine / TokenManager / DeviceProofService / ReconnectPolicy / AuthRequestBuilder）
组装成完整账号生命周期驱动器，通过 `IAuthTransport` 抽象与网络解耦：

- UI 意图：`startWithPassword` / `startWithSavedToken` / `logout` / `cancel` / `tick`（自动刷新钩子）。
- 编排：连接 → 握手 → 按 `authMethod` 发密码/Token 登录 → 落盘 → 自动刷新（Single-Flight）
  → 断线退避重连 → 被踢/吊销被动登出。
- 与当前已完善的 `AuthStateMachine`（`authMethod()` 决定 SendTokenLogin/SendPasswordLogin）
  和 `TokenManager`（`applyAtomically` 原子写 + `completeRefresh/suspendRefresh` 语义）对齐。

单测 `test_account_session`（`FakeAuthTransport` + fake clock/KV/软件签名器）10 组：密码登录
成功/密码错误、Token 冷启动登录、无凭据回退、自动刷新+单飞、刷新吊销→被动登出、被踢、
断线退避重连、登出、认证中连接失败 —— **全通过**。

## 2. 生产适配器 ClientCoreAuthTransport

`ClientCoreAuthTransport`（`client_core/src/account/ClientCoreAuthTransport.cpp`）实现
`IAuthTransport` + `im::IAuthProtocolSink`，把 AccountSession 接到真实 `im::ClientCore`：

- `connect()` 在后台线程调用同步的 `ClientCore::connectToServer()`，完成后异步回调
  `onConnectResult`（不推翻 ClientCore 的同步模型）。
- `sendPasswordLogin` 走 `ClientCore::sendLogin`；Token 登录/刷新/登出走
  `ClientCore::sendAuthRaw`（AuthRequestBuilder 构造的 payload）。
- `scheduleReconnect` 用可取消的一次性定时线程实现退避。

配套在 `ClientCore` 新增：`IAuthProtocolSink` + `setAuthProtocolSink` + `sendAuthRaw` +
`appSessionId()`，并把 `TokenLoginRs/RefreshTokenRs/LogoutRs` 注册进 `m_dealFunArr`、
在 `onLoginRs`/握手完成/断开/被踢处回调 sink。旧的同步 `sendLogin`/UI 事件路径不受影响。

## 3. T06 Android 接入

| 层 | 交付 |
|---|---|
| JNI | `im_auth_jni.cpp`：`nativeAccountSetup/LoginWithPassword/StartWithSavedToken/Logout/Cancel/GetState`；`jni_account_observer.{h,cpp}`：AccountEvent → Kotlin（拍平成 `onAccountEvent`） |
| 句柄 | `NativeSdkHandle` 扩展认证会话组件（clock/store/signer/transport/session/observer），按依赖逆序析构；`SoftwareP256Signer` + `MemoryTokenStore`（Keystore/加密 KV 为设备端后续） |
| SDK | `NativeBindings` 6 个认证 native 声明；`NativeAccountSink` + `AccountState/ConnectionState/AuthError/AccountEventType` 枚举（与 C++ 序号一致）；`JitongSdk` 认证 API（`accountState: StateFlow` + `accountEvents: SharedFlow` + setup/登录/登出/取消） |
| UI | `NativeAuthController`：Native 模式下的认证协调器，UI 只表达意图/映射状态，**不**做 Token/刷新/重连编排；**不魔改 73KB 的 MainViewModel**，Legacy 路径原样保留，符合"单进程单 Backend + 可回退" |

## 4. 构建与验证

```text
桌面 client_core CTest（Release，-UNDEBUG）：12/12 passed
  ...含 auth_state_machine / token_manager / device_proof_service / reconnect_policy /
     auth_request_builder / account_session
桌面 ASan+UBSan（scripts/native_sanitizer_test.sh --client-only）：12/12 passed，无内存/UB 报告
Android assembleDebug：arm64-v8a + x86_64 native 链接通过 + Kotlin 编译通过
Android arm64 AVD（Pixel_7, Android 14）：含新认证 JNI 的 .so 正常加载，androidTest 通过（冒烟）
```

证据：`outputs/kernel-round4-asan-client.log`。

## 5. 本轮明确未做（后续）

| 项 | 说明 |
|---|---|
| Android Keystore P-256 签名器 | 现用 `SoftwareP256Signer`；硬件密钥（Keystore）实现属设备端后续，`IP256Signer` 抽象已就位 |
| 持久化 TokenStore | 现用 `MemoryTokenStore`；Keystore-加密 KV 落盘为后续 |
| MainViewModel 深度整合 / 登录 UI 实际切换 | 已提供 `NativeAuthController`，把现有登录界面切到它、并在 Native 模式隐藏 Legacy Token 逻辑，属 UI 集成收尾 |
| 认证域 Server E2E（真实服务端 token 登录/刷新/登出/被踢闭环） | 建议下一轮补 `test_auth_native_e2e`，用 ClientCoreAuthTransport 连真实 im_server 跑完整闭环 |
| 冷启动自动登录接到 App 启动流程 | 内核已支持 `startWithSavedToken`，需在 Application/首页启动时调用 |

## 6. 判定

P5 认证域已从"可测内核组件"推进到"**在生产链路接线并接入 Android**"：AccountSession 编排 +
ClientCoreAuthTransport 生产适配 + 完整 JNI/SDK/Controller，桌面单测与 ASan/UBSan 全绿、
Android 两 ABI 构建与 arm64 AVD 冒烟通过。剩余为设备端硬件密钥/持久化、UI 集成收尾与
认证域 Server E2E，均为明确的后续项。

---

## 7. 代码审查修复补充（2026-09-08）

针对 AccountSession 接入后的并发、生命周期和异常分支复审，本轮继续完成以下修复：

- `ClientCoreAuthTransport` 不再持互斥锁调用 AccountSession；改为锁内提升 `weak_ptr`、锁外回调，
  消除登录失败/被踢触发同步 disconnect 时的重入死锁。
- 句柄销毁先解绑 AccountSession、停止并 join transport 事件源，再释放会话，消除后台回调 UAF 窗口。
- AccountSession 的 JNI、网络、定时器入口统一串行化；JNI 不持句柄锁执行可能回调 Java 的业务方法。
- 登录 Single-Flight 在状态机接受 operation 后才提交账号和密码，异账号拒绝不再覆盖在途凭据；
  pending 密码改用 `SecureString`，发送后清零。
- 冷启动发现 Access Token 不可用而 Refresh Token 有效时，执行
  `Connect → Refresh → 原子轮换 → Token Login`，不再发送已过期 Access Token。
- Refresh 成功响应必须先完成 Token 原子轮换，失败不得上报 `RefreshSucceeded`；轮换已包含 pending
  删除，不再重复 `completeRefresh()`。
- cancel/logout 推进 generation，使旧响应失效；认证请求发送时记录 generation，响应回调携带原请求
  generation，副作用执行前先校验，避免迟到结果污染新 operation。
- 已认证连接断开后，重连安全通道就绪会重新执行 Token Login。
- `logout(allDevices)` 已透传到 `LogoutRq.logout_all_devices`。

新增 AccountSession 反例覆盖：异账号凭据防污染、过期 Access 冷启动先刷新、Refresh 落盘失败、
cancel 后迟到响应、全设备登出参数、断线重连后 Token Login。

本次复验结果：

```text
桌面可执行回归：10/10 passed
P5 ASan+UBSan 定向测试：6/6 passed
Android :app:assembleDebug：BUILD SUCCESSFUL
  arm64-v8a / x86_64 C++ 链接与 Kotlin 编译通过
git diff --check：passed
```

说明：以上修复关闭了本轮代码审查发现的确定性 P0/P1 问题，但不改变第 5 节的剩余边界：
Android Keystore、持久化加密 TokenStore、UI 实际切换、真机完整认证和 Server E2E 仍需后续验收。

---

## 8. P5 可独立收尾项（2026-09-09）

按照“先完成不依赖 UI/消息整体迁移的底层能力，最后统一切换 Backend”的边界，本次完成：

- Android 生产认证不再构造进程级 `SoftwareP256Signer`，改由 JNI 调用已有
  `DeviceIdentity`，使用 Android Keystore 中不可导出的 P-256 私钥完成设备证明；
  公钥或签名失败均返回空并由 C++ fail-close，不存在软件密钥回退。
- `deviceId` 复用 `Prefs.deviceId`，首次安装生成随机 UUID 并持久化，重启后保持稳定，
  不使用手机号或硬件标识。
- `MemoryTokenStore` 从 Android 生产句柄移除，替换为 `JniTokenStore`：C++ 将完整 KV
  快照编码为有版本、长度校验和条目上限的单 blob；Java 侧通过 Android Keystore
  AES-256-GCM 加密后存入 MMKV。批量更新遵循“先持久化新快照，成功后替换内存态”，
  保存失败时内存与磁盘旧状态均不提交。
- 修正 `AndroidSecureKv.save()` 忽略 MMKV `encode=false` 的问题，失败会真实返回 false。
- `nativeCreate` 通过 `AppIdentityPins` 显式注入 Ed25519 应用身份信任根；Native 仍校验
  key id、base64 与 32 字节长度，空信任根不得创建，修复此前 SDK 永远创建失败的问题。
- 增加 `JitongSdk.restoreAccount(serverIp, port, account)` 冷启动入口：组装认证会话后从
  加密 TokenStore 恢复；无凭据、凭据损坏或过期返回 0，由 UI 保持在登录页。

验证结果：

```text
Android Kotlin + Native 两 ABI 编译：BUILD SUCCESSFUL
Android Debug APK：assembleDebug BUILD SUCCESSFUL
Pixel_7 AVD / Android 14：NativeAuthPlatformTest 3/3 passed
  - 设备 ID 与 Keystore P-256 公钥跨实例稳定，SHA256withECDSA 可验签
  - 加密 blob 跨桥实例持久化往返，MMKV 中不出现明文
  - Native AccountSession 生产平台桥装配成功，未登录时不建立 socket
P5 桌面单测：auth_state_machine/token_manager/device_proof/reconnect/
  auth_request_builder/account_session 6/6 passed
git diff --check：passed
```

仍按统一迁移阶段处理的项目：登录 UI 切换、Legacy socket 下线，以及依赖真实服务端的
密码登录→Token 登录→刷新轮换→登出/被踢 Server E2E。当前默认 Backend 未改变，因而不会
与尚未迁移的消息链路形成双 socket。
