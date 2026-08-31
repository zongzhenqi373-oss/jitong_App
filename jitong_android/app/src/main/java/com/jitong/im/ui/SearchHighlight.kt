package com.jitong.im.ui

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import com.jitong.im.util.PinyinIndex

/** 搜索结果高亮：将中文、连续全拼或首字母命中映射回原消息字符。 */
fun highlightedSearchText(
    content: String,
    query: String,
    highlightColor: Color,
    normalColor: Color,
): AnnotatedString = buildAnnotatedString {
    append(content)
    if (content.isNotEmpty()) {
        addStyle(SpanStyle(color = normalColor), 0, content.length)
    }
    PinyinIndex.matchRanges(content, query).forEach { range ->
        if (range.first >= 0 && range.last < content.length) {
            addStyle(
                SpanStyle(color = highlightColor, fontWeight = FontWeight.Bold),
                range.first,
                range.last + 1,
            )
        }
    }
}
