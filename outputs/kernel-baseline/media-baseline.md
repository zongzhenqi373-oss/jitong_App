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

### 4.0 偏色规避策略（2026-09-08 变更，QQNT 式工程规避）

**变更**：发送侧三档产物由 AVIF 改为 **JPEG**；AVIF 降级为「仅传输中间格式」。

| | 变更前 | 变更后 |
|---|---|---|
| 发送侧编码 | AVIF(q100, YUV444, lossless chroma) | **JPEG(原图 90 / 大缩略图 85 / 小缩略图 82)** |
| 发送侧 MIME | `image/avif` | **`image/jpeg`** |
| 文件扩展名 | `.avif` | **`.jpg`** |
| AVIF 角色 | 端到端格式（上传、存储、展示） | **仅传输中间格式**（下载到后转 JPEG 并删除源文件） |
| 奇数宽高 | 裁掉 1px 取偶（avif-coder 要求） | **不裁**（JPEG 无此限制） |

**为什么**：本地编 AVIF 是偏色根因 —— `avif-coder` 的 lossless chroma 在部分 ABI 上
解码出现 G/B 通道错位。2026-09-08 门禁首次运行即捕获：**白色回读 `255,172,255`**。

**修复后实测**（`MediaGoldenTest`，容差从 35 收紧到 8 后仍全绿）：

```text
solid-white/original       expected=#FFFFFFFF  actual=#FFFFFFFF   (偏差 0)
ultra-wide/original        expected=#FFD02020  actual=#FFD02020   (偏差 0)
ultra-tall/original        expected=#FF2050D0  actual=#FF2050D0   (偏差 0)
odd-dimensions/original    expected=#FF20C050  actual=#FF1FC050   (偏差 1，JPEG q90 量化)
```

**这套方案防什么 / 不防什么**

- ✅ 防「量化损失 + 色彩空间往返」这类**可控**偏色：统一落到 8bit sRGB SDR；
  编码前只做一次必要的色彩定点（`ensureEncodable` 在已满足时零拷贝），杜绝多次 RGB 往返累积误差。
- ❌ **不做** Display P3 / HDR / 10bit 的保真：这类输入统一转换到 sRGB，属有意的信息损失。
  要覆盖那类场景需给转码接口补 nclx/ICC 出入参，并让输出 JPEG 携带正确色彩配置文件
  （`ColorTransferParams` 已定义、`JpegIcc` 已实现写入，真正保真需 NDK 层 libavif + libjpeg-turbo，P10）。

**关于「YUV 直转」**：`Bitmap.compress(JPEG)` 由 libjpeg-turbo 完成，输入是 RGBA，
Kotlin/Bitmap 层**无法**做到无 RGB 往返的 YUV 直转。当前把往返压缩到唯一一次；
真正的直连需要 NDK 层 libavif → libjpeg-turbo（P10，接口已预留）。

**远程开关**：`ImagePipelineConfig.avifTransportEnabled`（默认关）+ 机型黑名单，
命中黑名单的机型强制走常规下载，不出问题可远程一键关闭 AVIF 链路。

### 4.1 常量

```text
MAX_ORIGINAL_EDGE     = 4096      # 原图长边上限
JPEG_QUALITY_ORIGINAL     = 90    # 原图 JPEG 质量（ImagePipelineConfig）
JPEG_QUALITY_LARGE_THUMB  = 85    # 大缩略图质量
JPEG_QUALITY_THUMB        = 82    # 小缩略图质量
THUMB_W x THUMB_H     = 320 x 240 # 小缩略图固定尺寸（4:3）
LARGE_THUMB_EDGE      = 1280      # 大缩略图长边
OUTPUT_MIME           = "image/jpeg"
OUTPUT_EXT            = ".jpg"
```

质量常量统一放在 `util/ImagePipelineConfig.kt`，避免编码参数散落多处导致两端不一致。

### 4.2 流程

```text
Uri → openInputStream
  ① inJustDecodeBounds 取原始宽高
  ② inSampleSize：while (longEdge / (sample*2) >= 4096) sample *= 2
  ③ 解码：inPreferredConfig = ARGB_8888，inPreferredColorSpace = SRGB
  ④ stripHdrGainmap()：Android 14+ 且 hasGainmap → gainmap = null（去 Ultra HDR 增益图）
  ⑤ 原图缩放：scale = min(1f, 4096 / max(w,h))，低于 1 才缩放
  ⑥ ensureEncodable()：已是 8bit sRGB、不透明、ARGB_8888 时**直接复用，零拷贝**；
     否则 toSrgb() 合成到白底不透明位图（JPEG 不支持 alpha）
  ⑦ 三档编码（统一 JPEG）：
       原图       = JPEG(q90) of encodable
       小缩略图   = JPEG(q82) of centerCropThumbnail(encodable)
       大缩略图   = JPEG(q85) of scaleLongEdge(encodable, 1280)
  ⑧ 返回 Compressed(原图bytes/w/h, 小图bytes/w/h, 大图bytes/w/h)
  异常统一吞掉返回 null
```

- `centerCropThumbnail`：目标比 4:3。**横向比 > 4:3 截宽度中间；否则截高度中间**，然后缩放至 320×240（**居中裁剪，非顶部首屏**）
- `scaleLongEdge`：等比缩放长边到 1280，宽高向下取偶
- 展示端 `decodeForDisplay`：优先 `BitmapFactory`（API 33+ 原生 AVIF），失败回落 `HeifCoder`；解码后同样 `stripHdrGainmap`。**保留 AVIF 解码能力**用于兼容历史已发出的 AVIF 消息

> **与 V3 §11.3 的差异**：V3 要求「纵向超长图缩略图展示**顶部首屏**并带长图标记」，
> 现状是**居中裁剪**。P10 阶段需确认是保留现状还是按 V3 改（若改，需同步更新本文件与 `media-golden.json`）。

### 4.3 下载侧：AVIF 中间格式归一化

对应 `MainViewModel.normalizeDownloadedImage()`：

```text
下载完成 → 读前 12 字节 → isAvifBytes()?
   ├─ 否 → 原样使用（常规路径，零开销）
   └─ 是 → ImageCodec.transcodeAvifToJpeg(src, dst)
             ├─ 成功 → 删除 AVIF 源文件，使用 JPEG（用户看到的永远是 JPEG）
             └─ 失败 → 保留原文件，交给 decodeForDisplay 的 AVIF 兜底解码（降级）
```

要点：

- **开关只决定是否「请求」AVIF**（服务端 URL 参数），**不决定下载到 AVIF 后是否转码** ——
  历史消息/服务端直传都可能带来 AVIF，一律检测后转码；
- 写入走 `.tmp` + `renameTo`，失败不留半截文件；
- 源与目标同名（下载路径已是 `.jpg`）时不删除文件，避免删掉刚转好的产物；
- 转码失败**不报错给用户**，降级为「按普通图片处理」。

> 服务端目前**不具备**实时压 AVIF 的能力（`HttpFileServer` 只有 `.avif` 的 MIME 识别），
> 因此 `ImagePipelineConfig.avifTransportEnabled` 默认关闭。待服务端支持后打开即可。

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
| D-01 | 普通图（如 3000×2000 sRGB） | 选图发送 | 原图长边 ≤ 4096；**产物为 JPEG(FFD8)**；小缩略图 320×240；大缩略图长边 1280 |
| D-02 | 横向超长图（如 8000×1000） | 选图发送 | 原图缩到 4096×512；缩略图**居中裁剪**为 4:3 后 320×240；原图不裁剪 |
| D-03 | 纵向超长图（如 1000×8000） | 选图发送 | 原图缩到 512×4096；缩略图居中裁剪 4:3 后 320×240（**现状非顶部首屏**，见 §4 差异） |
| D-04 | 奇数宽高（如 401×301） | 选图发送 | **原样输出 401×301，不裁边**（JPEG 无偶数要求）；大缩略图仍取偶为 400×300 |
| D-05 | Display P3 输入 | 选图发送 | 统一转 sRGB 后编码；展示端无色偏（对比基线像素/感知哈希） |
| D-06 | Ultra HDR / gainmap JPEG | 选图发送 | gainmap 被剥离，输出 SDR；不得出现绿/洋红通道异常 |
| D-07 | EXIF 旋转 90/180/270 | 选图发送 | 解码后方向正确（依赖 BitmapFactory 的 EXIF 处理） |
| D-08 | HEIC 输入 | 选图发送 | 系统解码成功并转 sRGB；失败时 `loadAndCompress` 返回 null（**现状无降级提示**） |
| D-09 | 纯色回读 | 白/红/蓝/绿纯色图发送后回读中心像素 | 三档通道偏差 **≤ 8**（实测 ≤ 1）；**不得**出现 G/B 通道错位（历史值 `255,172,255`） |
| D-21 | 下载到 AVIF | 服务端返回 AVIF 中间格式 | 转码成 JPEG、删除 AVIF 源文件、`localPath` 指向 .jpg |
| D-22 | AVIF 转码失败 | AVIF 损坏/解码器不支持 | 不报错，保留原文件，由 `decodeForDisplay` 的 AVIF 兜底解码 |
| D-23 | 机型黑名单 | 远程配置命中当前机型 | 不请求 AVIF，直接走常规下载 |
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
   已能执行尺寸、格式与关键像素颜色门禁。
   2026-09-08 首次运行发现 AVIF 白色回读 `255,172,255`（G/B 通道错位），
   门禁一度保持失败——**禁止以放宽容差掩盖偏色**。
   同日发送侧改为 JPEG 后：容差由 35 **收紧到 8**，门禁全绿（实测偏差 ≤ 1）。
   若将来重新引入 AVIF 编码，这条门禁必须仍然保持收紧后的容差。
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
| P-09 | 发送侧已改 JPEG；AVIF 仅作传输中间格式 | Native 内核保持同一策略 | **对齐**，不得在内核侧重新引入端到端 AVIF |
| P-10 | 无 nclx/ICC 出入参、输出 JPEG 不携带 ICC | `ColorTransferParams` + `JpegIcc` | 接口已定义/实现，宽色域保真需 NDK（P10） |
| P-11 | 无服务端 AVIF 压缩 | 服务端架平实时压 | **服务端新增能力**（当前开关默认关闭） |
