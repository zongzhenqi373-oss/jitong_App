# client_core Golden 测试

本目录存放**跨端行为一致性**测试：同一份用例既在 Android（Legacy Kotlin 实现）上跑，
也在 C++ Native Kernel 上跑，结果必须逐字段一致。

## 与单元测试的区别

| 目录 | 测什么 | 是否依赖外部 |
|---|---|---|
| `tests/unit/` | 模块内部逻辑（FrameCodec、TokenManager 状态机…） | 否 |
| `tests/integration/` | 本机回环，起假服务端 | 否，自建证书 |
| `tests/golden/` | **与 Android 现状的行为一致性** | 读 `outputs/kernel-baseline/*.json` |

## 用例来源

`golden` 测试不自己定义数据，而是加载 `../../../outputs/kernel-baseline/` 下的用例文件：

```text
outputs/kernel-baseline/search-golden.json
outputs/kernel-baseline/message-golden.json   (P0-T03 产出)
outputs/kernel-baseline/media-golden.json     (P0-T05 产出)
```

这样保证「一份用例、两端执行」。

## 当前状态

- [x] 目录已建立（P0-T01）
- [ ] 用例加载器与 runner（P9 搜索阶段实现；P0 阶段只需要目录与约定）
- [ ] `search-golden.json` 全部通过（P9 门禁）
- [ ] `message-golden.json` 全部通过（P8 门禁）
- [ ] `media-golden.json` 全部通过（P12 门禁）
