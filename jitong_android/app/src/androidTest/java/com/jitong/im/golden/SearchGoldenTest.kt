package com.jitong.im.golden

import android.content.Context
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.jitong.im.data.db.AppDatabase
import com.jitong.im.data.db.MessageEntity
import com.jitong.im.util.PinyinIndex
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/** 在真实 Android SQLite FTS4 + TinyPinyin 上执行仓库统一的 65 条搜索 Golden。 */
@RunWith(AndroidJUnit4::class)
class SearchGoldenTest {
    private lateinit var db: AppDatabase

    @Before
    fun setUp() {
        val context = ApplicationProvider.getApplicationContext<Context>()
        db = Room.inMemoryDatabaseBuilder(context, AppDatabase::class.java)
            .allowMainThreadQueries()
            .build()
    }

    @After
    fun tearDown() = db.close()

    @Test
    fun allSearchCasesMatchDatabaseAndHighlightContract() = runBlocking {
        val testContext = InstrumentationRegistry.getInstrumentation().context
        val root = JSONObject(testContext.assets.open("search-golden.json").bufferedReader().use { it.readText() })
        val cases = root.getJSONArray("cases")
        assertEquals(65, cases.length())

        for (index in 0 until cases.length()) {
            val item = cases.getJSONObject(index)
            val id = item.getString("id")
            val content = item.getString("content")
            val query = item.getString("query").trim().lowercase()
            val ownerId = 10_000 + index
            val entity = MessageEntity(
                ownerId = ownerId,
                msgId = id,
                conversationId = ownerId.toLong() shl 32 or 99L,
                peerId = 99,
                fromMe = false,
                type = 0,
                content = content,
                ts = index.toLong(),
                status = 2,
            )
            db.messageDao().insertWithFts(entity)

            val pattern = query.split(Regex("\\s+"))
                .filter(String::isNotBlank)
                .joinToString(" AND ") { "\"${it.replace("\"", "\"\"")}\"*" }
            val fts = runCatching { db.messageDao().searchFts(ownerId, pattern) }.getOrDefault(emptyList())
            val literal = db.messageDao().searchLike(ownerId, query)
            val phonetic = if (query.length == 1 && query[0] in 'a'..'z') {
                db.messageDao().searchInitialsLike(ownerId, query)
            } else {
                db.messageDao().searchPinyinLike(ownerId, query)
            }
            val actualMatch = (fts + literal + phonetic).distinctBy { it.msgId }.isNotEmpty()
            assertEquals("$id database match for '$query' in '$content'", item.getBoolean("expectMatch"), actualMatch)

            val expectedRanges = item.getJSONArray("expectHighlights").let { array ->
                (0 until array.length()).map { rangeIndex ->
                    val range = array.getJSONArray(rangeIndex)
                    range.getInt(0)..range.getInt(1)
                }
            }
            assertEquals("$id highlight ranges", expectedRanges, PinyinIndex.matchRanges(content, query))
        }
    }
}
