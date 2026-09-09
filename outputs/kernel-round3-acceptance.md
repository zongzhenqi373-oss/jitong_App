# 第三轮（P5 认证域）C++ 内核层验收报告

> 范围：执行计划第 23 章 P5「认证逻辑迁移到 C++ 内核」。本轮按约定只做 **C++ 内核层
> （T01–T05, T07）+ 单元测试**，不含 Android UI 切换（T06）与真机 instrumentation。
> 执行日期：2026-09-08　结论：**P5 基础组件阶段完成；AccountSession/ClientCore 接线及
> Android 端到端门禁尚未完成。基础组件定向单测普通 + ASan+UBSan 全绿。**

---

## 1. 交付的内核模块

全部为纯 C++、无网络/线程耦合的可测单元（时间、存储、随机、签名器均通过接口注入）。

| 任务 | 模块 | 文件 |
|---|---|---|
| T01/T02 | 认证类型 + 状态机 + Single-Flight | `include/client_core/AccountTypes.h`、`AuthStateMachine.h`；`src/account/AccountTypes.cpp`、`AuthStateMachine.cpp` |
| T04 | SecureString + TokenManager（可注入时钟/KV） | `include/client_core/SecureString.h`、`TokenManager.h`；`src/account/TokenManager.cpp` |
| T03 | DeviceProofService + IP256Signer 抽象 | `include/client_core/DeviceProofService.h`；`src/account/DeviceProofService.cpp` |
| T05 | ReconnectPolicy（退避 + 可注入 jitter） | `include/client_core/ReconnectPolicy.h`；`src/account/ReconnectPolicy.cpp` |
| T07 | AuthRequestBuilder（Token 登录/刷新/登出请求） | `include/client_core/AuthRequestBuilder.h`；`src/account/AuthRequestBuilder.cpp` |

## 2. 关键设计要点（对齐 QQNT「瘦 UI / 厚内核」）

- **决策与副作用分离**：`AuthStateMachine` 只产出「下一状态 + 动作列表（Connect/SendLogin/
  ScheduleReconnect/EmitEvent/...）」，真正的网络与定时交给会话层执行，故状态机可完整确定性单测。
- **Single-Flight**：
  - 登录：同账号重复请求复用同一 `operationId`，异账号请求返回
    `RejectOperation(OperationInProgress)`，不打断在途操作。
  - 刷新：`TokenManager::beginRefresh()` in-flight 期间复用同一 `request_id`，且该 id **持久化**，
    使「发出刷新 → 断线/重启 → 重连」仍用同一个 request_id，服务端可幂等处理、不误判复用攻击。
- **Refresh 终态分离**：成功/明确终态清 pending，网络断线调用 `suspendRefresh()` 保留
  `request_id`；request_id 持久化失败时 fail-close，不允许发包。
- **generation 防迟到**：每次新认证/刷新操作 `generation++`，迟到的旧结果按 generation 丢弃。
- **原子轮换契约**：`ITokenStore::applyAtomically` 要求全部提交或完全不改变；
  `TokenManager::adopt/rotate` 通过该接口一次写入 TokenSession，刷新轮换同时删除 pending。
- **认证方式固定**：每个登录 operation 保存 `Password/AccessToken` 方法，安全连接就绪后产生
  对应请求；Refreshing 也纳入登录 Single-Flight，重连后重新产出 `SendRefresh`。
- **吊销清 family**：refresh 复用/过期时 `revokeFamily()` 清空该账号全部持久化凭据。
- **被踢/登出禁重连**：进入 `LoggedOutKicked` 或登出后 `autoReconnectAllowed=false`，
  `ReconnectPolicy::disable()`，断线不再自动重连。
- **设备证明与服务端逐字节一致**：`DeviceProofService` 复刻服务端
  `im_server/src/auth/DeviceProof.cpp` 的 `message` 与 `AuthHandler::verifyDevice` 的四类
  operation 绑定（见下表），仅 `password-login` 在 message 内绑定公钥。

| operation | credentialBinding | 绑定公钥 |
|---|---|---|
| password-login | `tel + '\0' + sha256Hex(pass)` | 是 |
| token-login | `access_token` | 否 |
| token-refresh | `refresh_token + '\0' + request_id` | 否 |
| logout | `refresh_token + '\0' + (all?"1":"0")` | 否 |

- **敏感数据**：`SecureString` 析构/覆盖前 `OPENSSL_cleanse` 清零 token 明文。
- **退避**：指数 1/2/4/8s… 封顶 30s，叠加可注入 jitter（测试固定 0），连接成功 `reset()`。

## 3. 单元测试

新增 5 个测试，全部为确定性单测（fake clock / 内存 KV / 固定 jitter / 软件 P-256 签名器）：

| 测试 | 覆盖 |
|---|---|
| `auth_state_machine` | 密码/Token 登录方式、同意图复用/冲突拒绝、Refreshing 防重入、刷新断线重发、被踢禁重连、generation 丢弃迟到成功/失败、登出（12 组） |
| `token_manager` | adopt+冷启动、严格有效期、Refresh Single-Flight、持久化失败 fail-close、断线复用 request_id、原子轮换失败存储不变、family 吊销、损坏 KV 清理（8 组） |
| `device_proof_service` | 四类操作 canonical message 与独立复算逐字节一致、仅 password-login 带公钥、验签往返、绑定标志生效（4 组） |
| `reconnect_policy` | 指数退避 1/2/4/8/16/30 封顶、reset、disable 返回 -1、jitter 叠加（4 组） |
| `auth_request_builder` | TokenLoginRq/RefreshTokenRq/LogoutRq 字段号(对齐 im.proto)+ 设备签名验签（3 组） |

### 结果

```text
本次修复后普通定向测试：9/9 passed
  protocol / frame_codec / storage / secure_channel
  + auth_state_machine / token_manager / device_proof_service / reconnect_policy / auth_request_builder

ASan+UBSan 基础组件定向测试：5/5 passed，无内存/UB 报告
```

证据：`outputs/kernel-round3-asan-client.log`。

## 4. 与服务端的一致性保证

- 设备证明 canonical message：`DeviceProofService::buildMessage` 与服务端
  `deviceproof::message` 采用相同的 `"jitong-device-proof-v1" || field(...)` 编码；
  测试用「独立复算 + P-256 验签」双重交叉验证。
- Token 请求字段号严格对齐 `protocol/im.proto`：
  `TokenLoginRq{1,3,4,5}`、`RefreshTokenRq{1,2,3,4}`、`LogoutRq{1,2,3,4}`；
  测试用 proto 反解校验。
- 协议号沿用 `Protocol.h`：TOKEN_LOGIN_RQ/RS、TOKEN_REFRESH_RQ/RS、LOGOUT_RQ/RS、KICKED_OFFLINE。

## 5. 本轮明确未做（后续）

| 项 | 状态 | 说明 |
|---|---|---|
| T06 Android UI 切换 | ⏭ 未做 | 正式 JNI(`nativeStart/nativeLoginWithPassword/...`)、`JitongSdk.kt`、砍掉 `MainViewModel` 的 Token 编排 —— 按本轮约定不含 |
| AVD instrumentation | ⏭ 未做 | 需 T06 稳定后进行 |
| AccountSession 会话编排落地到 ClientCore | ⏭ 未接线 | 本轮交付的是可测内核组件（状态机/Token/设备证明/退避/请求构造）；把它们组装进 `ClientCore` 的实际连接/收发回调、并驱动自动登录/刷新/重连，作为 T06/接线阶段一并完成 |
| Android Keystore P-256 签名器 | ⏭ 未做 | `IP256Signer` 抽象已就位，Keystore 实现属 JNI 层(T03 的设备端部分)，随 T06 落地 |

## 6. 2026-09-08 代码审查修复

修复方案与反例见 `outputs/kernel-round3-p5-review-fix-plan.md`。本次已完成：

- TokenSession 原子批量存储契约及失败回滚测试；
- Refresh pending requestId 的完成/中断/取消语义拆分；
- requestId 持久化失败 fail-close；
- Password/Token 登录方式固定和 Refreshing 防重入；
- Refresh 断线重连后重发动作；
- 迟到失败响应静默丢弃；
- 损坏过期字段 fail-close 并清理凭据。

生产 Android `ITokenStore::applyAtomically` 仍须在接线阶段用事务、加密单记录或版本化双槽实现，
不能以多次 SharedPreferences 写入冒充原子提交。

## 7. 判定

当前可判定为 **P5 基础组件阶段通过**：基础组件已通过确定性单测（普通 + ASan+UBSan），
设备证明格式与 Token 请求字段已完成单元级对齐。由于 `AccountSession`、ClientCore 协议响应接线、
Android Keystore/原子 KV、正式 JNI、真实并发和服务端 E2E 尚未完成，**不得判定 P5 整体完成**。
