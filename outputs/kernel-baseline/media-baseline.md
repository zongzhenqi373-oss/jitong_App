# P0-T05 图片 / 缩略图 / 秒传 / 传输基线

> 事实来源：`net/HttpMediaClient.kt`、`util/ImageCodec.kt`、`net/Sha256.kt`、`ui/MainViewModel.kt`、`ui/ChatScreen.kt`。

## 1. 通道概览

```text
IM 长连接（24563）  →  只传 file_id 与元数据（MediaCard）
HTTPS 文件服务（24564）→  传字节（上传 / 下载）
```

- OkHttp + `CertificatePinner`（复用 `TlsPinning.CURRENT_PIN`）
- 超时：connect 10s / read 2min / write 2min
- 鉴权头：`Authorization: Bearer <accessToken>`、`X-Device-Id: <deviceId>`
- 单文件上限：`Protocol.FILE_MAX_SIZE = 100 * 1024 * 1024`
- HTTP 错误映射：

| 码 | 语义 |
|---|---|
| 401 | 「登录凭证已过期，请重试」 |
| 403 | 「没有文件访问权限」 |
| 413 | 「文件超过服务器限制」 |
| 415 | 「图片格式不受支持」 |
| 其它非 2xx | 「文件服务错误 HTTP xxx」 |

> **现状缺口**：HTTP 401 **不触发 refresh**（见 `auth-baseline.md` D-02）。

## 2. 上传

### 2.1 接口

```text
POST /api/v1/upload
  header: X-File-Name（basename，空则 "file"，截断 255）、X-Receiver-Id
  body  : 整文件流（64 KiB 缓冲，contentLength = file.length()）
  resp  : { file_id, sha256, size, content_type }
```

> **现状：无分片上传**。整文件单次 POST，64 KiB 缓冲只用于流式写出与进度回调。
> V3 §11.5 的 `ChunkUploader`（upload_id / checkpoint / 并发窗口 / 续传）属**新增能力**。

### 2.2 秒传（PoP）

```text
1) 流式计算 SHA-256（sha256HexOfStream）
2) POST /api/v1/upload/preflight
     header: X-Receiver-Id, X-File-Size, X-File-Sha256
     body  : 空
     resp  : { hit: bool, challenge_id?, offset?, length? }
3) hit == false → 走 §2.1 全量上传
4) hit == true  → RandomAccessFile.seek(offset) + readFully(proof, length)
                  POST /api/v1/upload/proof/{challengeId}
                    Content-Type: application/octet-stream
                    body: proof 字节
                  resp: { file_id, sha256, size, content_type }
```

- 服务端在校验 proof 后写入 receiver 授权，返回**新的** `file_id`
- 客户端**未**做「计算期间文件被修改」的二次校验（无 mtime/size 复核）——V3 §11-T02 要求补

## 3. 下载与续传

```text
GET /api/v1/download/{fileId}
  fileId 校验：Regex("[A-Za-z0-9._-]+")
  header（续传）：Range: bytes=<offset>- ，offset = <dest>.part 当前长度

响应处理：
  code 206 → total = offset + contentLength，追加写
  code 200 且 offset > 0 → 服务端不支持 Range → 删除 .part，offset 归零，重新全量写
  写入：FileOutputStream(part, append = offset > 0)，64 KiB 缓冲，结束 output.fd.sync()

完成后：
  若 expectedSha256 非空 → 流式 SHA-256 比对（忽略大小写），不符抛「文件摘要校验失败」
  destination 存在则先 delete
  part.renameTo(destination)；失败则 copyTo(overwrite) + delete
```

## 4. 图片流水线（`util/ImageCodec.kt`）

### 4.1 常量

```text
MAX_ORIGINAL_EDGE     = 4096      # 原图长边上限
AVIF_QUALITY          = 100       # 原图 AVIF 质量
THUMB_QUALITY         = 82        # 小缩略图质量
THUMB_W x THUMB_H     = 320 x 240 # 小缩略图固定尺寸（4:3）
LARGE_THUMB_EDGE      = 1280      # 大缩略图长边
LARGE_THUMB_QUALITY   = 95
```

编码参数（三档一致）：`preciseMode = LOSSY`、`surfaceMode = RGB`、`chromaSubsampling = YUV444`

> 选型原因（代码注释）：lossless chroma 在部分 ABI/设备解码出现 G/B 通道错位（整图绿/洋红），
> 故用 q100 + YUV444 走标准 YUV 路径换取兼容性。

### 4.2 流程

```text
Uri → openInputStream
  ① inJustDecodeBounds 取原始宽高
  ② inSampleSize：while (longEdge / (sample*2) >= 4096) sample *= 2
  ③ 解码：inPreferredConfig = ARGB_8888，inPreferredColorSpace = SRGB
  ④ stripHdrGainmap()：Android 14+ 且 hasGainmap → gainmap = null（去 Ultra HDR 增益图）
  ⑤ 原图缩放：scale = min(1f, 4096 / max(w,h))，低于 1 才缩放
  ⑥ toSrgb()：创建 ARGB_8888、hasAlpha=false、sRGB 位图；先填白底再 drawBitmap
  ⑦ ensureEvenDimensions()：avif-coder 要求偶数宽高，奇数边裁掉最外侧 1px（不拉伸）
  ⑧ 三档编码：
       原图       = AVIF(q100) of encodable
       小缩略图   = AVIF(q82)  of centerCropThumbnail(encodable)
       大缩略图   = AVIF(q95)  of scaleLongEdge(encodable, 1280)
  ⑨ 返回 Compressed(原图bytes/w/h, 小图bytes/w/h, 大图bytes/w/h)
  异常统一吞掉返回 null
```

- `centerCropThumbnail`：目标比 4:3。**横向比 > 4:3 截宽度中间；否则截高度中间**，然后缩放至 320×240（**居中裁剪，非顶部首屏**）
- `scaleLongEdge`：等比缩放长边到 1280，宽高向下取偶
- 展示端 `decodeForDisplay`：优先 `BitmapFactory`（API 33+ 原生 AVIF），失败回落 `HeifCoder`；解码后同样 `stripHdrGainmap`

> **与 V3 §11.3 的差异**：V3 要求「纵向超长图缩略图展示**顶部首屏**并带长图标记」，
> 现状是**居中裁剪**。P10 阶段需确认是保留现状还是按 V3 改（若改，需同步更新本文件与 `media-golden.json`）。

## 5. 三档资源与 MediaCard

数据库 `messages` 中的字段分组（见 `message-baseline.md` §6.1）：

| 档 | 字段前缀 |
|---|---|
| 原图 | `fileId`, `fileName`, `fileSize`, `contentType`, `sha256`, `imgW`, `imgH`, `localPath` |
| 小缩略图 | `thumbnailFileId`, `thumbnailPath`, `thumbnailSize`, `thumbnailSha256`, `thumbnailW`, `thumbnailH` |
| 大缩略图 | `largeThumbnailFileId`, `largeThumbnailPath`, `largeThumbnailSize`, `largeThumbnailSha256`, `largeThumbnailW`, `largeThumbnailH` |

发送顺序：`POST /api/v1/upload`（或秒传）取得 `file_id` → `ImClient.sendMediaCard(...)` 复用 `CHAT_INFO_RQ(1005)`，type=IMAGE/FILE。

## 6. Golden 场景矩阵

| ID | 场景 | 输入 | 期望结果 |
|---|---|---|---|
| D-01 | 普通图（如 3000×2000 sRGB） | 选图发送 | 原图长边 ≤ 4096；宽高为偶数；小缩略图 320×240；大缩略图长边 1280；MediaCard 携带原始宽高 |
| D-02 | 横向超长图（如 8000×1000） | 选图发送 | 原图缩到 4096×512（偶数）；缩略图**居中裁剪**为 4:3 后 320×240；原图不裁剪 |
| D-03 | 纵向超长图（如 1000×8000） | 选图发送 | 原图缩到 512×4096；缩略图居中裁剪 4:3 后 320×240（**现状非顶部首屏**，见 §4 差异） |
| D-04 | 奇数宽高（如 3001×2001） | 选图发送 | 编码前裁为 3000×2000（各 -1px），不拉伸 |
| D-05 | Display P3 输入 | 选图发送 | 统一转 sRGB 后编码；展示端无色偏（对比基线像素/感知哈希） |
| D-06 | Ultra HDR / gainmap JPEG | 选图发送 | gainmap 被剥离，输出 SDR；不得出现绿/洋红通道异常 |
| D-07 | EXIF 旋转 90/180/270 | 选图发送 | 解码后方向正确（依赖 BitmapFactory 的 EXIF 处理） |
| D-08 | HEIC 输入 | 选图发送 | 系统解码成功并转 sRGB；失败时 `loadAndCompress` 返回 null（**现状无降级提示**） |
| D-09 | 尺寸过小（如 3×3 → 2×2） | 选图发送 | `ensureEvenDimensions` 要求 ≥2；过小抛「图片尺寸过小」→ 被 catch 返回 null |
| D-10 | 秒传命中 | 上传已存在于服务端的文件 | 只发 preflight + proof 片段（**不上传整文件**）；返回新 file_id |
| D-11 | 秒传未命中 | 上传新文件 | preflight `hit=false` → 全量 `POST /api/v1/upload` |
| D-12 | PoP challenge 越界 | 服务端返回 offset/length 超出文件大小 | 现状 `readFully` 抛 EOF → 被上层 catch；V3 要求显式校验范围并拒绝 |
| D-13 | 下载 200（不支持 Range） | `.part` 已有部分数据 | 删除 `.part`，offset 归零，全量重下 |
| D-14 | 下载 206（Range 续传） | 中断后重试 | 从 `.part` 长度续传；完成后 SHA-256 校验；rename 到目标 |
| D-15 | 下载摘要不符 | 服务端返回错误内容 | 抛「文件摘要校验失败」；`.part` 保留（下次续传会重新校验） |
| D-16 | 下载 404 | 失效 file_id | 「文件服务错误 HTTP 404」；V3 要求区分**失效索引**与**临时网络错误** |
| D-17 | 上传 401 | access token 过期 | 「登录凭证已过期，请重试」；**现状不自动 refresh**（D-02） |
| D-18 | 上传 413 | 超过服务端限制 | 「文件超过服务器限制」 |
| D-19 | 上传 415 | 不支持的格式 | 「图片格式不受支持」 |
| D-20 | 大文件内存占用 | 100 MB 文件 | 全程 64 KiB 缓冲流式读写；内存占用有界（无整文件 byte[]） |

## 7. 像素/色彩基线的保存方式

D-05 / D-06 / D-07 需要像素级对照。约定：

```text
1) 在真机对同一组样本图跑 Legacy 流水线，导出每档的：
     - 尺寸（w×h）
     - 文件大小
     - SHA-256（字节级，用于完全相同的场景）
     - 感知哈希（用于允许编码差异的场景：dHash 64bit）
2) 样本图放 test-images/（仓库已有该目录），不含个人信息
3) 结果存 `outputs/kernel-baseline/media-golden.json`；Android `MediaGoldenTest`
   已能执行尺寸、格式与关键像素颜色门禁。2026-09-08 首次运行发现 AVIF
   白色回读为 `255,172,255`，因此当前门禁保持失败，禁止以放宽容差掩盖偏色。
```

判定规则：

| 场景 | 判定 |
|---|---|
| 完全相同的输入 + 相同编码器版本 | SHA-256 必须一致 |
| 跨平台编码器（libavif vs avif-coder） | 尺寸必须一致；dHash 汉明距离 ≤ 5；无整体色偏 |
| 色偏检测 | 对比 sRGB 平均 RGB 三通道均值，偏差 ≤ 3/255 |

## 8. Native 内核需补齐的差异（P10~P12 阶段对照）

| 编号 | Android 现状 | V3 要求 | 处理 |
|---|---|---|---|
| P-01 | 无分片上传、无续传、无并发窗口 | `ChunkUploader` | **新增能力** |
| P-02 | 缩略图策略常量散落在 `ImageCodec` | `MediaConfig` 下发 | **下沉并配置化**（常量已在本文件固化） |
| P-03 | 纵向长图居中裁剪 | 顶部首屏 + 长图标记 | **待产品确认** |
| P-04 | 无 `setVisibleMessages` 分级加载调度 | `DownloadScheduler` P0~P4 优先级 | **新增能力** |
| P-05 | 无 `ImageDisplayState`（UI 自行拼装显示） | 内核返回状态 | **新增能力** |
| P-06 | PoP 无「文件变更二次校验」 | 计算期间文件改变则拒绝秒传 | **新增能力** |
| P-07 | 解码/编码在 Kotlin | 平台桥 `IPlatformImage` 原子能力 | **下沉策略，保留平台执行** |
| P-08 | 404 不区分失效索引与网络错误 | `MediaRecord` 区分 | **新增能力** |
