# ADR-01：数据库无感解锁采用双包装

| 项 | 值 |
|---|---|
| 状态 | **ACCEPTED**（不可变决策） |
| 日期 | 2026-09-10 |
| 负责人 | [待指定] |
| 评审 | [待指定]（需产品 + 安全） |
| 关联 | `P7-G0`、§2 ADR-01、`kernel-round7-implementation-plan.md` |

---

## 1. 背景与问题陈述

### 1.1 现状（代码事实）

| 组件 | 事实 |
|---|---|
| `DbKeyManager` | realKey(32B) 由 `passHash` 经 PBKDF2-HMAC-SHA256(120k, salt16) 派生 wrapKey，AES-GCM(256, iv12, tag128) 包装后落盘；blob 格式 `salt(16) ‖ iv(12) ‖ ciphertext` |
| `TokenVault` | 双 Token 由 **Android Keystore** AES-GCM 保护；sessionId/userId 明文存 MMKV，密文存 SP |
| `PasswordVault` | "记住密码"字段由 Keystore 保护，注释明确要求"用户还没登录、还没输入密码之前就能被读出来" |
| `AppDatabase` | `version = 8`；已去掉 `fallbackToDestructiveMigration()`，密钥不可用时**报错并保留密文文件** |
| `NativeDatabase` | 上一轮已实现 `openWithBridge`：**桥返回 Unavailable 时状态保持 `Locked`，不降级、不用 Token 派生 key** |

### 1.2 矛盾

Token 可从 Keystore 恢复 → 冷启动自动连接服务端；
但 DB realKey 只能由 passHash 解开 → 无 passHash 时本地库锁定。

即 **"连得上但看不到本地历史"**。这不是缺陷而是上一轮刻意选择的 fail-close 语义，但它与"Token 自动登录"的产品体验冲突，且会把用户推向"清库重来"。

### 1.3 约束（不可协商）

- 不存明文密码，不存 passHash；
- 不因 Keystore 失败而生成新 realKey（会造成旧库永久不可读）；
- 任何失败路径都不得创建空库、不得覆盖旧 key blob、不得 destructive migration；
- 不削弱 SQLCipher realKey 强度，不绕过 Android 锁屏/Keystore 安全边界。

---

## 2. 决策

**同一个随机 `realKey` 同时保存两个独立 AEAD wrapper。**

```
realKey (32B, SecureRandom)
 ├─ passwordWrapper = AES-GCM( PBKDF2(passHash, passwordSalt) , realKey )
 └─ deviceWrapper   = AES-GCM( Android Keystore account key   , realKey )
```

两条解锁路径互为备份，任一成功即可解出同一个 realKey。

---

## 3. 详细设计

### 3.1 Wrapper 二进制格式（版本化）

每个 wrapper 独立文件（或同一文件内两个版本化段），至少包含：

```
magic | version | ownerId | keyId | algorithm | salt | nonce | ciphertext(+tag)
```

### 3.2 密码学要求

- 两个 wrapper 使用**独立 nonce**；
- **AAD 绑定** `ownerId ‖ wrapperType ‖ version ‖ keyId`，防止跨账号/跨类型/跨版本移植密文；
- Keystore alias 包含 `ownerId`，**不跨账号复用**；
- `passwordWrapper` 与 `deviceWrapper` 的 keyId 独立，替换其中一个不影响另一个。

### 3.3 创建与替换规则

| 场景 | 规则 |
|---|---|
| 首次密码登录 | 解开或创建 realKey 后，**才允许**创建 `deviceWrapper` |
| Keystore 不可用 | **禁止**生成新 realKey；DB 保持锁定，转 `NEEDS_PASSWORD_UNLOCK` |
| 密码变更 | 只有在**已成功获得 realKey** 后，才能原子替换 `passwordWrapper`；失败保留旧 blob |
| 登出 | 只清 Token 与内存 key；**是否删除 `deviceWrapper` 由"保留本地聊天记录"产品选项决定**，清理必须显式 |

### 3.4 冷启动状态机

```
冷启动
  ├─ Token 有效 + deviceWrapper 解出 realKey → READY（无感解锁）
  ├─ deviceWrapper 失败/不存在            → NEEDS_PASSWORD_UNLOCK
  │     └─ 用户输入密码 → passwordWrapper 解出 realKey → READY（并补写 deviceWrapper）
  ├─ 两者皆失败 + 本地库存在             → LOCKED_WITH_DATA（禁止建空库）
  └─ 两者皆失败 + 本地库不存在           → 可新建 realKey
```

> `NEEDS_PASSWORD_UNLOCK` / `LOCKED_WITH_DATA` 必须向 UI 明确表达，**不得**表现为"无历史消息"。

---

## 4. 备选方案与取舍

| 选项 | 语义 | 取舍 |
|---|---|---|
| **A. 保持现状（Token 可连，库锁定，要求重输密码）** | 最安全，零新增攻击面 | 体验差：冷启动看不到历史；与"记住登录态"矛盾 |
| **B. 双包装（本决策）** | 无感 Token 冷启动；Keystore 失效可回退密码 | 新增一套 Keystore wrapper；需处理 Keystore 不可用机型降级 |
| C. 复用 `PasswordVault` 解密密码重建 passHash | 改动最小 | 仅覆盖"已勾选记住密码"的账号；**不能**把"记住密码"变成默认开启，否则等同于长期持有明文凭据 |

**选 B 的理由**：A 牺牲体验且已与现状冲突；C 覆盖面不足且把"记住密码"语义扩大化（安全上等同于长期持有明文密码）。B 在不持有明文密码的前提下，把"设备已解锁"这一既有信任（Keystore 已在保护 Token）延伸到 DB 解锁，信任模型一致。

**代价**：Keystore wrapper 降低的是**冷启动门槛**，不是 realKey 强度——攻击者拿到已解锁设备+Keystore 访问权才能解出，与当前 Token 保护级别相当。

---

## 5. 安全边界（显式声明）

1. `deviceWrapper` **不能**绕过 Android 锁屏：Keystore key 的可用性仍受设备解锁状态/生物认证策略约束（视 alias 配置而定）。
2. 不降低 realKey 强度：realKey 仍是 CSPRNG 32 字节，两个 wrapper 只是它的两种封装。
3. 不扩大明文暴露面：全程无明文密码、无 passHash 落盘。
4. 失败一律 fail-close：不解出就锁定，**绝不**新建空库覆盖。

---

## 6. 验收矩阵（可机读，进入测试数据）

维度：`记住密码? × Token 有效? × Keystore 可用? × 密码是否变更? × blob 状态?`

| # | 记住密码 | Token | Keystore | 密码变更 | blob 状态 | 期望 |
|---|---|---|---|---|---|---|
| 1 | 是 | 有效 | 可用 | 否 | 正常 | READY（deviceWrapper） |
| 2 | 是 | 有效 | 失效 | 否 | 正常 | NEEDS_PASSWORD_UNLOCK → 密码解锁 → READY |
| 3 | 是 | 过期 | 可用 | 否 | 正常 | 刷新 Token 后 READY |
| 4 | 否 | 有效 | 可用 | 否 | 正常 | READY（deviceWrapper） |
| 5 | 否 | 有效 | 失效 | 否 | 正常 | NEEDS_PASSWORD_UNLOCK → 输入密码 → READY |
| 6 | — | — | — | 是 | 正常 | 需旧密码或已解锁态原子替换 passwordWrapper；成功后保留 deviceWrapper |
| 7 | — | — | — | — | wrapper 截断 | 该 wrapper 解失败 → 转另一路径；失败锁定，不建空库 |
| 8 | — | — | — | — | GCM tag 错 | 同上（篡改检测） |
| 9 | — | — | — | — | 磁盘写失败 | 原子替换失败 → 保留旧 blob，状态不变 |
| 10 | — | — | — | — | 本地库存在、两 wrapper 均失败 | `LOCKED_WITH_DATA`，UI 明确提示，**不得**静默清空 |

**每条都必须断言**：不创建空库、不覆盖旧 blob、不 destructive migration、不因失败生成新 realKey。

---

## 7. 待定项（不阻塞本 ADR 生效）

1. **登出是否删除 `deviceWrapper`** —— 取决于"保留本地聊天记录"产品选项，需产品确认；未确认前默认**保留**（与当前登出不清 Room 的语义一致）。
2. **Keystore alias 是否绑定生物认证**（`setUserAuthenticationRequired`）—— 若绑定，无感解锁在锁屏状态下会失败并转 `NEEDS_PASSWORD_UNLOCK`。需产品与安全确认用户体验取舍。

---

## 8. 不可变决策（后续不得单方面更改）

1. realKey 唯一，两个 wrapper 只是其封装；
2. 不保存明文密码或 passHash；
3. 任何失败路径 fail-close，禁止创建空库/覆盖旧 blob/destructive migration；
4. 不得因 Keystore 失败生成新 realKey。
