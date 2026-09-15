# ADR-02：服务端能力协商不升级安全协议版本

| 项 | 值 |
|---|---|
| 状态 | **ACCEPTED**（不可变决策） |
| 日期 | 2026-09-10 |
| 负责人 | [待指定] |
| 关联 | `P7-G0`、§2 ADR-02、`kernel-round7-implementation-plan.md` |

---

## 1. 背景与问题陈述

### 1.1 现状（代码事实）

| 事实 | 证据 |
|---|---|
| 应用安全协议版本 `APP_SECURITY_VERSION = 1` | `net/Protocol.kt:51` |
| 握手为 protobuf：`AppClientHello` / `AppServerHello` | `net/SecureChannel.kt:131-176` |
| `AppClientHello` 字段 | version、clientEphemeralPublicKey、clientNonce、cipherSuite |
| `AppServerHello` 字段（被严格校验） | version、serverEphemeralPublicKey、serverNonce、sessionId、keyId、cipherSuite、signature |
| version 严格相等校验 | `validateServerHello`：`hello.version != APP_SECURITY_VERSION` → 抛 IOException |
| 握手 transcript / frame AAD | `"jitong-app-handshake-v1"` / `"jitong-app-frame-v1"` |
| 媒体既有能力（走既有 HTTP API） | 秒传 preflight+proof（`tryInstantUpload`）、Range 断点下载（`download`）、整文件流式上传（`upload`） |
| **不存在**的能力 | 分片上传、按缺洞区间精确拉取、多设备已读同步 |

### 1.2 问题

P7 需要引入增强能力（分片上传、精确区间补洞、多设备已读），但这些**服务端尚未交付**。若直接改安全协议版本或在握手里加必填字段：

- 改 `version` → 与所有旧服务端/旧客户端不兼容，且 `validateServerHello` 会因版本不等直接断连；
- 加必填字段 → 旧端解析失败或校验失败。

因此需要一种**不改版本、可前向/后向兼容**的能力协商方式，且必须明确：已有能力不得因为新协商机制而"被关闭"。

---

## 2. 决策

1. **保持当前应用安全协议 `version = 1` 不变**；
2. 若实现增强项，在既有 protobuf `hello` 中增加**可选** `repeated string capabilities`；
3. 客户端与服务端分别声明能力，**生效集合取交集**；
4. **缺字段等价空集合**，**未知值忽略**；
5. 旧 protobuf 端依靠 **unknown-field 兼容**，不改变 TLS、GCM 与握手 version 校验；
6. **若本轮不实施任何增强项，则不修改生产握手协议**，只在 Native 内保留 `ServerCapabilities` 空集合与兼容分支。

---

## 3. 详细设计

### 3.1 能力集合

| capability | 语义 | 门控性质 |
|---|---|---|
| `resumable_upload_v1` | 分片上传（每片幂等 + checkpoint） | **增强项**，服务端未声明则回退整文件流式上传 |
| `roam_range_v1` | 按指定缺洞区间拉取 | **增强项**，未声明则回退现有 `beforeSeq` 漫游分页逐页追平 |
| `read_sync_v1` | 多设备已读水位同步 | **增强项**，未声明则仅本地已读 |
| `instant_proof_v1` | 秒传（preflight + proof） | **仅前向兼容声明**，不作为启用条件 |
| `range_download_v1` | Range 断点下载 | **仅前向兼容声明**，不作为启用条件 |

> **关键区分**：秒传、Range 下载、整文件流式上传是**当前已交付能力**，走既有 HTTP API。
> **不得**把它们置于新 capability 门控之下——否则"服务端没声明 capability"会导致已有功能被关闭，构成回归。
> 上表后两项仅为未来统一声明预留，本轮不读取、不依赖。

### 3.2 协商与降级

```
生效集合 = clientCapabilities ∩ serverCapabilities
```

- 未知 capability：忽略（不影响握手成功）；
- `roam_range_v1` 缺失：补洞复用现有漫游分页，**设置最大页数、最大持续时间与指数退避**；预算耗尽保留 gap 并等待下次触发，**不形成请求风暴**；
- `resumable_upload_v1` 缺失：回退整文件流式上传；
- `read_sync_v1` 缺失：只做本地已读，不假定多设备水位。

### 3.3 服务端安全要求（增强项启用前提）

- 文件类 capability 必须与 **access token、deviceId、接收者授权**绑定；
- proof / chunk id 具备**过期时间**且**防重放**；
- 服务端需完成 handler、schema、鉴权、限流与兼容矩阵。

---

## 4. 备选方案与取舍

| 选项 | 取舍 |
|---|---|
| **A. 升级 `version` 到 2** | 与旧端完全不兼容；`validateServerHello` 严格相等校验会导致旧服务端直接断连。**否决** |
| **B. hello 增加必填 capabilities 字段** | 旧端解析/校验失败，等价破坏兼容。**否决** |
| **C. 可选 `repeated string` + 交集 + 未知忽略（本决策）** | protobuf unknown-field 天然兼容；旧端忽略未知字段，新端对缺字段按空集合处理。**选中** |
| D. 完全不协商，客户端硬编码尝试 | 对未交付能力产生无谓请求与错误风暴，且无法降级。**否决** |

---

## 5. 兼容矩阵（必测）

| 组合 | 期望 |
|---|---|
| 新客户端 × 旧服务端 | 握手成功；`capabilities` 缺失 → 空集合；三项增强全部回退到既有路径；秒传/Range/整文件**功能不退化** |
| 旧客户端 × 新服务端 | 握手成功；旧端忽略未知 capabilities 字段；服务端按空集合处理 |
| 新客户端 × 新服务端 | 按交集启用增强项 |

每个增强 capability 还需**关闭测试**：显式关闭时现有功能仍可用，且不无限重试。

---

## 6. 验收

- [ ] `APP_SECURITY_VERSION` 仍为 `1`，`validateServerHello` 的 version 校验未被放宽；
- [ ] 三组兼容测试通过（新×旧 / 旧×新 / 新×新）；
- [ ] 每个增强 capability 有关闭测试，关闭时走既有降级路径且无请求风暴；
- [ ] 秒传、Range 下载、整文件上传在 capability 为空时**仍可用**（回归门禁）；
- [ ] 本轮若未实施增强项，则生产握手协议**未被修改**（diff 可证）。

---

## 7. 不可变决策

1. 安全协议 `version` 在本轮保持 `1`；
2. 已有能力（秒传 / Range 下载 / 整文件流式上传）**不受新 capability 门控**；
3. 缺字段等价空集合、未知值忽略，不得因未知 capability 断连；
4. 增强项未获服务端声明时一律降级，不得构造不存在的协议请求。
