package com.jitong.im

import com.jitong.im.util.PinyinIndex
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class PinyinIndexTest {
    @Test
    fun sentenceContainsPinyinAndInitialForChineseCharacterInTheMiddle() {
        val tokens = PinyinIndex.of(
            "哈哈，手机又抽风啦?没事~我比较喜欢看悬疑或者轻喜剧，你有私藏的好剧吗?",
        )

        // “你”在长句中间；搜索兜底使用 instr(pinyin/initials, keyword)，
        // 因此连续全拼中的 ni 和连续首字母中的 n 都必须真实存在。
        assertTrue(tokens.full.contains("ni"))
        assertTrue(tokens.initials.contains("n"))
    }

    @Test
    fun singleInitialHighlightsNiButNotZhengOrZhen() {
        val text = "正真你年"

        assertEquals(listOf(2..3), PinyinIndex.matchRanges(text, "n"))
    }

    @Test
    fun continuousPinyinMapsBackToOriginalChineseCharacters() {
        val text = "喜欢看悬疑或者轻喜剧"

        assertEquals(listOf(3..4), PinyinIndex.matchRanges(text, "xuanyi"))
        assertEquals(listOf(8..9), PinyinIndex.matchRanges(text, "xj"))
    }

    @Test
    fun literalChineseQueryMapsDirectly() {
        assertEquals(listOf(2..4), PinyinIndex.matchRanges("天气轻喜剧", "轻喜剧"))
    }
}
