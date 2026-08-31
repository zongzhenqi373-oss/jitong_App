package com.jitong.im.util

import me.majiajie.tinypinyin.Pinyin

/** 把中文正文展开为可供 FTS4 检索的全拼和拼音首字母。 */
object PinyinIndex {
    data class Tokens(val full: String, val initials: String)
    private data class CharToken(val charIndex: Int, val pinyin: String, val initial: Char)

    fun of(text: String): Tokens {
        val full = StringBuilder()
        val initials = StringBuilder()
        text.forEach { ch ->
            if (Pinyin.isChinese(ch)) {
                val syllable = Pinyin.toPinyin(ch).lowercase()
                full.append(syllable)
                initials.append(syllable.firstOrNull() ?: ch)
            } else if (ch.isLetterOrDigit()) {
                val normalized = ch.lowercaseChar()
                full.append(normalized)
                initials.append(normalized)
            } else {
                full.append(' ')
                initials.append(' ')
            }
        }
        return Tokens(full.toString().trim(), initials.toString().replace(" ", ""))
    }

    /** 将正文、连续全拼和拼音首字母的命中位置映射回原始消息字符。 */
    fun matchRanges(text: String, query: String): List<IntRange> {
        val keyword = query.trim().lowercase()
        if (keyword.isEmpty() || text.isEmpty()) return emptyList()

        val ranges = findAll(text.lowercase(), keyword).toMutableList()
        if (!keyword.all { it in 'a'..'z' || it.isDigit() }) return mergeRanges(ranges)

        val tokens = buildCharTokens(text)
        if (keyword.length == 1 && keyword[0] in 'a'..'z') {
            tokens.filter { it.initial == keyword[0] }
                .forEach { ranges += it.charIndex..it.charIndex }
            return mergeRanges(ranges)
        }

        val full = StringBuilder()
        val fullOffsets = mutableListOf<Int>()
        tokens.forEach { token ->
            token.pinyin.forEach { ch ->
                full.append(ch)
                fullOffsets += token.charIndex
            }
        }
        addMappedRanges(full.toString(), fullOffsets, keyword, ranges)

        val initials = tokens.joinToString("") { it.initial.toString() }
        addMappedRanges(initials, tokens.map { it.charIndex }, keyword, ranges)
        return mergeRanges(ranges)
    }

    private fun buildCharTokens(text: String): List<CharToken> = buildList {
        text.forEachIndexed { index, ch ->
            when {
                Pinyin.isChinese(ch) -> {
                    val syllable = Pinyin.toPinyin(ch).lowercase()
                    add(CharToken(index, syllable, syllable.firstOrNull() ?: ch))
                }
                ch.isLetterOrDigit() -> {
                    val normalized = ch.lowercaseChar()
                    add(CharToken(index, normalized.toString(), normalized))
                }
            }
        }
    }

    private fun findAll(source: String, keyword: String): List<IntRange> {
        val result = mutableListOf<IntRange>()
        var start = source.indexOf(keyword)
        while (start >= 0) {
            result += start until start + keyword.length
            start = source.indexOf(keyword, start + 1)
        }
        return result
    }

    private fun addMappedRanges(
        source: String,
        offsets: List<Int>,
        keyword: String,
        output: MutableList<IntRange>,
    ) {
        findAll(source, keyword).forEach { range ->
            val first = offsets.getOrNull(range.first) ?: return@forEach
            val last = offsets.getOrNull(range.last) ?: return@forEach
            output += minOf(first, last)..maxOf(first, last)
        }
    }

    private fun mergeRanges(input: List<IntRange>): List<IntRange> {
        val sorted = input.sortedWith(compareBy<IntRange> { it.first }.thenBy { it.last })
        val output = mutableListOf<IntRange>()
        sorted.forEach { range ->
            val previous = output.lastOrNull()
            if (previous == null || range.first > previous.last + 1) {
                output += range
            } else {
                output[output.lastIndex] = previous.first..maxOf(previous.last, range.last)
            }
        }
        return output
    }
}
