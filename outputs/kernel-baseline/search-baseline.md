# P0-T04 搜索策略与高亮基线

> 用例数据见 `search-golden.json`（65 条）。本文件说明**算法**，用例说明**期望**。

## 1. 索引生成（`util/PinyinIndex.kt`）

```text
PinyinIndex.of(text) → Tokens(full, initials)

逐字符：
  汉字          → syllable = Pinyin.toPinyin(ch).lowercase()
                  full += syllable        ; initials += syllable.firstOrNull() ?: ch
  字母/数字     → full += ch.lowercase()  ; initials += ch.lowercase()
  其它(标点/空格/emoji) → full += ' '     ; initials += ' '

返回：full.trim() ；initials.replace(" ", "")
```

**关键差异**：`full` 里非字母数字字符占一个空格（可被 `instr` 命中并影响偏移）；`initials` 里空格被**删除**（不占位）。

| 表列 | 内容 |
|---|---|
| `content` | 原文 |
| `pinyin` | `full` |
| `initials` | 首字母串 |

约束：**只有 `type == 0`（TEXT）且 content 非空才建索引**。

多音字：**未处理**，tinypinyin 取单值，无词典、无候选展开。C++ 侧必须导出同一张码表（P9-T01）。

## 2. 查询策略（`data/ChatStore.kt`）

归一化：`kw.trim().lowercase()`
三路并发，结果 `distinctBy { it.msgId }` + `sortedByDescending { it.ts }`：

| 路 | 条件 | SQL |
|---|---|---|
| FTS | 任何输入 | `messages_fts MATCH :pattern` |
| LIKE | 任何输入，仅 `type = 0` | `content LIKE '%' \|\| :kw \|\| '%'` |
| 拼音 | **单字母** `kw.length==1 && kw[0] in 'a'..'z'` | `instr(f.initials, :kw) > 0` |
| 拼音 | 其它 | `instr(f.pinyin, :kw) > 0 OR instr(f.initials, :kw) > 0` |

`ftsPattern()`：按 `\s+` 分词，每词包成 `"词"*`（前缀匹配），词间 ` AND `，内部 `"` 转义为 `""`。解析失败被 `runCatching` 吞掉降级为空。

全部查询 `ORDER BY ts DESC LIMIT 100`；会话内版本多一个 `conversationId = :id` 条件。

### SQL 原文（`data/db/Daos.kt`）

```sql
-- searchFts
SELECT m.* FROM messages m JOIN messages_fts f ON m.msgId = f.msgId
 WHERE m.ownerId = :ownerId AND messages_fts MATCH :pattern
 ORDER BY m.ts DESC LIMIT 100

-- searchLike
SELECT * FROM messages WHERE ownerId = :ownerId AND type = 0
  AND content LIKE '%' || :kw || '%' ORDER BY ts DESC LIMIT 100

-- searchInitialsLike（单字母）
SELECT m.* FROM messages m JOIN messages_fts f ON m.msgId = f.msgId
 WHERE m.ownerId = :ownerId AND instr(f.initials, :kw) > 0
 ORDER BY m.ts DESC LIMIT 100

-- searchPinyinLike（多字母）
SELECT m.* FROM messages m JOIN messages_fts f ON m.msgId = f.msgId
 WHERE m.ownerId = :ownerId
   AND (instr(f.pinyin, :kw) > 0 OR instr(f.initials, :kw) > 0)
 ORDER BY m.ts DESC LIMIT 100
```

## 3. 高亮映射（`PinyinIndex.matchRanges`）

```text
keyword = query.trim().lowercase()

1. ranges = findAll(text.lowercase(), keyword)            # 直接子串，总是执行
2. 若 keyword 含非 [a-z0-9] → return mergeRanges(ranges)  # 中文/混合输入到此为止
3. tokens = buildCharTokens(text)                          # 只收集汉字与字母数字，带原始 index
4. 若 keyword 长度 1 且为 a-z：
     tokens.filter { it.initial == keyword[0] }.forEach { ranges += it.charIndex..it.charIndex }
     return mergeRanges(ranges)
5. full = 逐 token 展开 pinyin 的每个字符；fullOffsets 为每个字符对应的 charIndex
   addMappedRanges(full, fullOffsets, keyword, ranges)
6. initials = tokens 的 initial 拼接；offsets = tokens 的 charIndex
   addMappedRanges(initials, offsets, keyword, ranges)
7. return mergeRanges(ranges)

addMappedRanges(source, offsets, keyword, out):
   findAll(source, keyword).forEach { range ->
       first = offsets.getOrNull(range.first) ?: return@forEach
       last  = offsets.getOrNull(range.last)  ?: return@forEach
       out += minOf(first,last)..maxOf(first,last)
   }

mergeRanges：按 start 排序，相邻或重叠（first <= previous.last + 1）即合并
```

### 边界行为（**必须原样保留**）

| 现象 | 原因 |
|---|---|
| `addMappedRanges` 中 `getOrNull` 为 null 时**整条命中被丢弃** | offset 越界（见 `search-golden.json` S-059 的分析） |
| 单个字母 query 只匹配**首字母**，不匹配拼音中的字母 | 步骤 4 提前 return |
| 中文 query 不做拼音回查 | 步骤 2 提前 return |
| 直接子串命中与拼音命中会**叠加后合并** | 步骤 1 总是执行 |

### 消费方（`ui/SearchHighlight.kt`）

```kotlin
PinyinIndex.matchRanges(content, query).forEach { range ->
    if (range.first >= 0 && range.last < content.length) {
        addStyle(SpanStyle(color, FontWeight.Bold), range.first, range.last + 1)
    }
}
```

→ 区间为**闭区间**，Compose 使用 `range.last + 1` 作为开区间右端点。

## 4. 验收方式

```text
P9 门禁：outputs/kernel-baseline/search-golden.json 全部 65 条通过
        + 「n」不会错误匹配「正/真」，但能按产品规则匹配「你」
```

三条执行路径必须跑同一份 JSON：

| 路径 | 执行者 |
|---|---|
| Android Legacy | `androidTest/.../SearchGoldenTest.kt`（真机，验证期望值） |
| C++ Native | `client_core/tests/golden/`（P9 实现） |
| 首次回填 | `BaselineExportTest.kt` 导出真机结果，覆盖 `expectHighlights` |

> 期望值为按上述算法**推算**结果；首次在真机跑 `BaselineExportTest` 后必须逐条比对，
> 特别是 `deviceVerifyRequired` 中列出的用例。若真机结果与本文件不一致，**以真机为准更新本文件**并在 PR 中说明。

首次设备执行修订：`S-055` 原 query `yjfs` 与“文件已发送”的真实 initials
`wjyfs` 不相符，已改为从“件”开始的 `jyfs`；高亮仍为 `[1,4]`。

## 5. Native 内核需补齐的差异（P9 阶段对照）

| 编号 | Android 现状 | V3 要求 | 处理 |
|---|---|---|---|
| G-01 | 高亮区间在 Kotlin 计算 | 内核返回 `highlightRanges`，UI 只加粗 | **下沉** |
| G-02 | 拼音与正文索引写入分两个 SQL（同一事务） | 同事务 | 保持 |
| G-03 | FTS 与 messages 通过 `msgId` JOIN，非 external content | — | P6 可保持同样结构 |
| G-04 | 无游标分页，固定 `LIMIT 100` | `SearchQuery{cursor}` | **新增能力** |
| G-05 | 三路合并后 `distinctBy(msgId)` | msg_id 去重 | 对齐 |
