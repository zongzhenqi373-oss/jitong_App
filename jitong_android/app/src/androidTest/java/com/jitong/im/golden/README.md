# Android 基线导出与回归测试（androidTest）

用途：在**真实 Android 进程**里执行当前 Kotlin Legacy 实现，把行为导出成 Golden 用例；
以及在迁移过程中回归验证「Legacy 行为没有漂移」。

## 为什么必须在 androidTest 跑

- 搜索依赖 Room FTS4 的 `simple` tokenizer 与 `tinypinyin` 词典，桌面 JVM 单测无法完全复现；
- 媒体依赖 `ImageCodec`（AVIF/JPEG、EXIF、Display P3）与 `HttpMediaClient`，必须在真机；
- Token/DB key 依赖 Keystore/MMKV，只有 instrumented 环境可用。

## 目录规划

```text
golden/
├── SearchGoldenTest.kt      加载 search-golden.json，执行真实 Room FTS4 + TinyPinyin
├── MediaGoldenTest.kt       生成固定像素向量，验证尺寸/AVIF/颜色
├── BaselineExportTest.kt    预留：导出设备特有 Golden 到 /sdcard
├── MessageGoldenTest.kt     P8 门禁
└── MediaGoldenTest.kt       P12 门禁
```

## 运行方式

```bash
# 需要真机或模拟器；结果文件通过 adb pull 取回
./gradlew :app:connectedAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=\
com.jitong.im.golden.SearchGoldenTest

adb pull /sdcard/Android/data/com.jitong.im/files/golden ./outputs/kernel-baseline/
```

## 脱敏

导出的用例**只包含**：命中与否、命中条数、排序后的相对序号、高亮区间下标。
**禁止**导出消息正文、token、手机号。导出脚本必须走 `outputs/kernel-baseline/BASELINE.md` §5 的检查。

## 当前状态

- [x] 目录已建立（P0-T01）
- [x] Search runner：65/65 通过（API 34 arm64，2026-09-08）
- [x] Media runner：已可执行；当前因 AVIF 白色回读为 `255,172,255` 正确失败
- [ ] BaselineExport runner（仅设备特有数据需要；当前 JSON 已直接作为共享输入）
- [ ] 与桌面/Native runner 共用同一份 JSON schema
