# 第三轮 P5 基础组件代码审查修复方案

> 日期：2026-09-08
> 范围：仅修复当前已经提交到工作区的 `AuthStateMachine`、`TokenManager` 及其单元测试；不在本次修复中提前实现 `AccountSession`、正式 JNI、Android Keystore 或 UI 接线。

## 1. 修复目标

本次修复把现有 P5 基础组件调整到“可以被 AccountSession 安全接线”的状态，解决以下问题：

1. TokenSession 多 Key 顺序写入造成的新旧 Token 混搭。
2. Refresh 网络中断时错误清除 pending requestId。
3. requestId 持久化失败后仍允许发送刷新请求。
4. 状态机没有记录 Password/Token 登录方式。
5. Refreshing 状态没有纳入登录 Single-Flight。
6. Refresh 断线重连后无法重新产出 `SendRefresh`。
7. 迟到的旧失败响应错误关联到当前 operation。
8. 损坏或缺失的 Token 过期时间被当作有效数据。

## 2. 设计修改

### 2.1 TokenStore 原子接口

在 `ITokenStore` 增加批量原子变更接口，一次提交多个 upsert 和 remove：

```cpp
applyAtomically(puts, removes)
```

接口契约为“全部成功或完全不改变存储”。`TokenManager::adopt/rotate` 不再依赖多个独立 `put()` 拼接原子性。后续 Android 实现可采用以下任一方式满足契约：

- 加密单记录替换；
- 数据库事务；
- version + 双槽位，最后原子切换 active version。

刷新成功的 TokenSession 轮换与 pending requestId 删除放入同一原子提交。

### 2.2 Refresh requestId 生命周期

将原来含义模糊的 `endRefresh()` 拆分为三种结果：

- `completeRefresh()`：成功或服务端明确终态，清除 pending。
- `suspendRefresh()`：断线、超时等临时失败，保留 pending，供重连复用。
- `cancelRefresh()`：登出、吊销或销毁，清除 pending。

`beginRefresh()` 必须先成功持久化 requestId，才能设置 in-flight 并返回；失败返回空字符串，调用者不得发包。

### 2.3 登录状态机

新增 `AuthMethod { None, Password, AccessToken }`，每个登录 operation 固定保存认证方式。连接完成后状态机根据该字段确定性产生：

- `SendPasswordLogin`；或
- `SendTokenLogin`。

Single-Flight 判断覆盖 `Authenticating` 和 `Refreshing`：仅“同账号、同认证方式”的重复登录复用 operationId，其他在途认证请求统一拒绝。

Refreshing 断线时保留刷新状态；新的安全连接就绪后重新产生 `SendRefresh`。真正发送时由 TokenManager 复用持久化的 pending requestId。

旧 generation 的成功和失败结果都静默丢弃，不能给当前 operation 发送失败事件。

### 2.4 持久化数据校验

Token 冷启动加载采用严格整数解析，并校验：

- 数字必须完整可解析且大于 0；
- `sessionId`、Access Token、Refresh Token 均非空；
- Refresh Token 尚未过期。

任一字段损坏均清理当前账号 Token family，并返回无有效登录记录。

## 3. 补充测试

### TokenManager

- 原子提交失败后持久化快照完全不变。
- 模拟批量事务内部失败，不出现新旧 Token 混搭。
- requestId 持久化失败时 `beginRefresh()` 返回空，且不进入 in-flight。
- 网络中断调用 `suspendRefresh()` 后，重建 TokenManager 仍复用原 requestId。
- rotate 成功时 TokenSession 与 pending 删除同时生效。
- `refresh_exp` 为空、非数字、带尾随字符、为 0 时加载失败并清理。

### AuthStateMachine

- Token 登录连接成功产生 `SendTokenLogin`。
- Password 登录连接成功产生 `SendPasswordLogin`。
- Password/Token 两种意图不能错误复用同一次 operation。
- Refreshing 时再次登录被拒绝，不覆盖账号、operationId 或 generation。
- Refreshing 断线重连后重新产生 `SendRefresh`。
- 旧 generation 的失败响应不产生当前 operation 的 `LoginFailed`。

## 4. 验收标准

1. 新增的 P5 五个定向测试全部通过。
2. `protocol/frame_codec/storage/secure_channel` 等无本地监听依赖的回归测试通过。
3. ASan+UBSan 的 P5 定向测试通过。
4. 验收报告不再声明 P5 全部完成，明确标记为“基础组件阶段完成、AccountSession 接线未完成”。
5. 本次不以组件测试代替 AccountSession、Android arm64、服务端 E2E 和并发门禁；这些仍保留为后续验收项。

## 5. 实施顺序

1. 修改 `ITokenStore` 原子契约和 TokenManager。
2. 修改 AuthStateMachine 的认证方式与在途状态规则。
3. 增加反例单元测试。
4. 编译并执行定向测试、回归测试和 Sanitizer。
5. 根据实际测试结果修订 `kernel-round3-acceptance.md`。
