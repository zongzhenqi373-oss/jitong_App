package com.jitong.im.data.db

import android.content.Context
import androidx.room.Database
import androidx.room.Room
import androidx.room.RoomDatabase
import net.zetetic.database.sqlcipher.SupportOpenHelperFactory

@Database(
    entities = [MessageEntity::class, MessageFtsEntity::class, ConversationEntity::class,
        LegacyChangeLogEntity::class, LegacyCutoverControlEntity::class],
    version = 11,
    exportSchema = false,
)
abstract class AppDatabase : RoomDatabase() {
    abstract fun messageDao(): MessageDao
    abstract fun conversationDao(): ConversationDao
    abstract fun legacyChangeLogDao(): LegacyChangeLogDao
    abstract fun legacyCutoverControlDao(): LegacyCutoverControlDao

    companion object {
        @Volatile
        private var instance: AppDatabase? = null

        @Volatile
        private var instanceOwnerId: Int? = null

        val MIGRATION_2_3 = object : androidx.room.migration.Migration(2, 3) {
            override fun migrate(db: androidx.sqlite.db.SupportSQLiteDatabase) {
                db.execSQL("ALTER TABLE messages ADD COLUMN fileId TEXT NOT NULL DEFAULT ''")
                db.execSQL("ALTER TABLE messages ADD COLUMN fileName TEXT NOT NULL DEFAULT ''")
                db.execSQL("ALTER TABLE messages ADD COLUMN fileSize INTEGER NOT NULL DEFAULT 0")
                db.execSQL("ALTER TABLE messages ADD COLUMN localPath TEXT")
                db.execSQL("ALTER TABLE messages ADD COLUMN transferred INTEGER NOT NULL DEFAULT 0")
            }
        }

        val MIGRATION_3_4 = object : androidx.room.migration.Migration(3, 4) {
            override fun migrate(db: androidx.sqlite.db.SupportSQLiteDatabase) {
                db.execSQL("ALTER TABLE messages ADD COLUMN contentType TEXT NOT NULL DEFAULT ''")
                db.execSQL("ALTER TABLE messages ADD COLUMN sha256 TEXT NOT NULL DEFAULT ''")
            }
        }

        val MIGRATION_4_5 = object : androidx.room.migration.Migration(4, 5) {
            override fun migrate(db: androidx.sqlite.db.SupportSQLiteDatabase) {
                // 旧版使用 min*2^20+max，用户 ID 超过 20 位会碰撞；统一迁移为 32+32 位组合。
                val expression = "((CASE WHEN ownerId < peerId THEN ownerId ELSE peerId END) << 32) " +
                    "| ((CASE WHEN ownerId < peerId THEN peerId ELSE ownerId END) & 4294967295)"
                db.execSQL("UPDATE messages SET conversationId = $expression")
                db.execSQL("UPDATE conversations SET conversationId = $expression")
                db.execSQL("DROP INDEX IF EXISTS index_messages_ownerId_conversationId_ts")
                db.execSQL(
                    "CREATE INDEX IF NOT EXISTS index_messages_ownerId_conversationId_seq " +
                        "ON messages(ownerId, conversationId, seq)",
                )
            }
        }

        val MIGRATION_5_6 = object : androidx.room.migration.Migration(5, 6) {
            override fun migrate(db: androidx.sqlite.db.SupportSQLiteDatabase) {
                // FTS 虚表不能可靠 ALTER COLUMN，重建后由 ChatStore 用原 messages 表回填。
                db.execSQL("DROP TABLE IF EXISTS messages_fts")
                db.execSQL(
                    "CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts " +
                        "USING FTS4(content, pinyin, initials, msgId)",
                )
            }
        }

        val MIGRATION_6_7 = object : androidx.room.migration.Migration(6, 7) {
            override fun migrate(db: androidx.sqlite.db.SupportSQLiteDatabase) {
                db.execSQL("ALTER TABLE messages ADD COLUMN thumbnailFileId TEXT NOT NULL DEFAULT ''")
                db.execSQL("ALTER TABLE messages ADD COLUMN thumbnailPath TEXT")
                db.execSQL("ALTER TABLE messages ADD COLUMN thumbnailSize INTEGER NOT NULL DEFAULT 0")
                db.execSQL("ALTER TABLE messages ADD COLUMN thumbnailSha256 TEXT NOT NULL DEFAULT ''")
                db.execSQL("ALTER TABLE messages ADD COLUMN thumbnailW INTEGER NOT NULL DEFAULT 0")
                db.execSQL("ALTER TABLE messages ADD COLUMN thumbnailH INTEGER NOT NULL DEFAULT 0")
            }
        }

        val MIGRATION_7_8 = object : androidx.room.migration.Migration(7, 8) {
            override fun migrate(db: androidx.sqlite.db.SupportSQLiteDatabase) {
                db.execSQL("ALTER TABLE messages ADD COLUMN largeThumbnailFileId TEXT NOT NULL DEFAULT ''")
                db.execSQL("ALTER TABLE messages ADD COLUMN largeThumbnailPath TEXT")
                db.execSQL("ALTER TABLE messages ADD COLUMN largeThumbnailSize INTEGER NOT NULL DEFAULT 0")
                db.execSQL("ALTER TABLE messages ADD COLUMN largeThumbnailSha256 TEXT NOT NULL DEFAULT ''")
                db.execSQL("ALTER TABLE messages ADD COLUMN largeThumbnailW INTEGER NOT NULL DEFAULT 0")
                db.execSQL("ALTER TABLE messages ADD COLUMN largeThumbnailH INTEGER NOT NULL DEFAULT 0")
            }
        }

        val MIGRATION_8_9 = object : androidx.room.migration.Migration(8, 9) {
            override fun migrate(db: androidx.sqlite.db.SupportSQLiteDatabase) {
                db.execSQL("""CREATE TABLE IF NOT EXISTS legacy_change_log (
                    changeSeq INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
                    ownerId INTEGER NOT NULL,
                    entityType TEXT NOT NULL,
                    entityKey TEXT NOT NULL,
                    operation TEXT NOT NULL,
                    changedAt INTEGER NOT NULL)""")
                db.execSQL("CREATE INDEX IF NOT EXISTS index_legacy_change_log_ownerId_changeSeq " +
                    "ON legacy_change_log(ownerId, changeSeq)")

                installV9ChangeLogTriggers(db)
            }
        }

        /** 保持既有 8→9 迁移语义不变；9→10 再替换为快照触发器。 */
        private fun installV9ChangeLogTriggers(db: androidx.sqlite.db.SupportSQLiteDatabase) {
            fun trigger(name: String, timing: String, table: String, owner: String,
                        type: String, key: String, operation: String) {
                db.execSQL("""CREATE TRIGGER IF NOT EXISTS $name AFTER $timing ON $table BEGIN
                    INSERT INTO legacy_change_log(ownerId,entityType,entityKey,operation,changedAt)
                    VALUES($owner,'$type',CAST($key AS TEXT),'$operation',CAST(strftime('%s','now') AS INTEGER));
                    END""")
            }
            trigger("legacy_messages_ai", "INSERT", "messages", "NEW.ownerId", "MESSAGE", "NEW.msgId", "UPSERT")
            trigger("legacy_messages_au", "UPDATE", "messages", "NEW.ownerId", "MESSAGE", "NEW.msgId", "UPSERT")
            trigger("legacy_messages_ad", "DELETE", "messages", "OLD.ownerId", "MESSAGE", "OLD.msgId", "DELETE")
            trigger("legacy_conversations_ai", "INSERT", "conversations", "NEW.ownerId", "CONVERSATION", "NEW.conversationId", "UPSERT")
            trigger("legacy_conversations_au", "UPDATE", "conversations", "NEW.ownerId", "CONVERSATION", "NEW.conversationId", "UPSERT")
            trigger("legacy_conversations_ad", "DELETE", "conversations", "OLD.ownerId", "CONVERSATION", "OLD.conversationId", "DELETE")
        }

        val MIGRATION_9_10 = object : androidx.room.migration.Migration(9, 10) {
            override fun migrate(db: androidx.sqlite.db.SupportSQLiteDatabase) {
                db.execSQL("ALTER TABLE legacy_change_log ADD COLUMN payloadVersion INTEGER NOT NULL DEFAULT 0")
                db.execSQL("ALTER TABLE legacy_change_log ADD COLUMN payload TEXT NOT NULL DEFAULT ''")
                // v9 日志没有快照，不能冒充 v10 strict delta；全量 snapshot 会重新建立基线。
                db.execSQL("DELETE FROM legacy_change_log")
                installChangeLogTriggers(db)
            }
        }

        val MIGRATION_10_11 = object : androidx.room.migration.Migration(10, 11) {
            override fun migrate(db: androidx.sqlite.db.SupportSQLiteDatabase) {
                db.execSQL("""CREATE TABLE IF NOT EXISTS legacy_cutover_control(
                    ownerId INTEGER NOT NULL PRIMARY KEY,
                    epoch INTEGER NOT NULL,
                    state INTEGER NOT NULL,
                    updatedAt INTEGER NOT NULL)""")
                installWriteFenceTriggers(db)
            }
        }

        internal fun installWriteFenceTriggers(db: androidx.sqlite.db.SupportSQLiteDatabase) {
            fun guard(name:String,timing:String,table:String,owner:String) {
                db.execSQL("""CREATE TRIGGER IF NOT EXISTS $name BEFORE $timing ON $table BEGIN
                    SELECT CASE WHEN EXISTS(SELECT 1 FROM legacy_cutover_control
                        WHERE ownerId=$owner AND state=1)
                    THEN RAISE(ABORT,'legacy_write_frozen') END;
                    END""")
            }
            guard("legacy_fence_messages_bi","INSERT","messages","NEW.ownerId")
            guard("legacy_fence_messages_bu","UPDATE","messages","NEW.ownerId")
            guard("legacy_fence_messages_bd","DELETE","messages","OLD.ownerId")
            guard("legacy_fence_conversations_bi","INSERT","conversations","NEW.ownerId")
            guard("legacy_fence_conversations_bu","UPDATE","conversations","NEW.ownerId")
            guard("legacy_fence_conversations_bd","DELETE","conversations","OLD.ownerId")
        }

        /** v10 触发器：UPSERT 在源写事务内固化 JSON 快照，DELETE 只留 tombstone。 */
        internal fun installChangeLogTriggers(db: androidx.sqlite.db.SupportSQLiteDatabase) {
            val names = listOf("legacy_messages_ai", "legacy_messages_au", "legacy_messages_ad",
                "legacy_conversations_ai", "legacy_conversations_au", "legacy_conversations_ad")
            names.forEach { db.execSQL("DROP TRIGGER IF EXISTS $it") }

            val messagePayload = """json_object(
                'msgId',NEW.msgId,'conversationId',NEW.conversationId,'peerId',NEW.peerId,
                'seq',NEW.seq,'ts',NEW.ts,'localOrder',NEW.id,'fromMe',NEW.fromMe,
                'type',NEW.type,'content',COALESCE(NEW.content,''),'status',NEW.status,
                'pinyin',COALESCE((SELECT pinyin FROM messages_fts WHERE msgId=NEW.msgId LIMIT 1),''),
                'initials',COALESCE((SELECT initials FROM messages_fts WHERE msgId=NEW.msgId LIMIT 1),''),
                'mediaPath',COALESCE(NEW.mediaPath,''),'imgW',NEW.imgW,'imgH',NEW.imgH,
                'fileId',NEW.fileId,'fileName',NEW.fileName,'fileSize',NEW.fileSize,
                'contentType',NEW.contentType,'sha256',NEW.sha256,
                'thumbnailFileId',NEW.thumbnailFileId,'thumbnailPath',COALESCE(NEW.thumbnailPath,''),
                'thumbnailSize',NEW.thumbnailSize,'thumbnailSha256',NEW.thumbnailSha256,
                'thumbnailW',NEW.thumbnailW,'thumbnailH',NEW.thumbnailH,
                'largeThumbnailFileId',NEW.largeThumbnailFileId,
                'largeThumbnailPath',COALESCE(NEW.largeThumbnailPath,''),
                'largeThumbnailSize',NEW.largeThumbnailSize,
                'largeThumbnailSha256',NEW.largeThumbnailSha256,
                'largeThumbnailW',NEW.largeThumbnailW,'largeThumbnailH',NEW.largeThumbnailH,
                'localPath',COALESCE(NEW.localPath,''),'transferred',NEW.transferred)""".trimIndent()
            val conversationPayload = """json_object(
                'conversationId',NEW.conversationId,'peerId',NEW.peerId,
                'lastMsg',NEW.lastMsg,'lastTs',NEW.lastTs,'unread',NEW.unread)""".trimIndent()

            fun upsert(name: String, timing: String, table: String, owner: String,
                       type: String, key: String, payload: String) {
                db.execSQL("""CREATE TRIGGER $name AFTER $timing ON $table BEGIN
                    INSERT INTO legacy_change_log(ownerId,entityType,entityKey,operation,changedAt,payloadVersion,payload)
                    VALUES($owner,'$type',CAST($key AS TEXT),'UPSERT',CAST(strftime('%s','now') AS INTEGER),1,$payload);
                    END""")
            }
            fun delete(name: String, table: String, owner: String, type: String, key: String) {
                db.execSQL("""CREATE TRIGGER $name AFTER DELETE ON $table BEGIN
                    INSERT INTO legacy_change_log(ownerId,entityType,entityKey,operation,changedAt,payloadVersion,payload)
                    VALUES($owner,'$type',CAST($key AS TEXT),'DELETE',CAST(strftime('%s','now') AS INTEGER),0,'');
                    END""")
            }
            upsert("legacy_messages_ai", "INSERT", "messages", "NEW.ownerId", "MESSAGE", "NEW.msgId", messagePayload)
            upsert("legacy_messages_au", "UPDATE", "messages", "NEW.ownerId", "MESSAGE", "NEW.msgId", messagePayload)
            delete("legacy_messages_ad", "messages", "OLD.ownerId", "MESSAGE", "OLD.msgId")
            upsert("legacy_conversations_ai", "INSERT", "conversations", "NEW.ownerId", "CONVERSATION", "NEW.conversationId", conversationPayload)
            upsert("legacy_conversations_au", "UPDATE", "conversations", "NEW.ownerId", "CONVERSATION", "NEW.conversationId", conversationPayload)
            delete("legacy_conversations_ad", "conversations", "OLD.ownerId", "CONVERSATION", "OLD.conversationId")
        }

        /**
         * 打开指定账号的加密本地库；key 由调用方通过 DbKeyManager 拿到（登录后才可用）。
         * 每个 ownerId 各自一个物理库文件（jitong_<ownerId>.db），互不共享密钥，
         * 同一设备换号登录时不会因为密钥解不开而误清空另一个账号的本地缓存。
         */
        fun get(context: Context, ownerId: Int, key: ByteArray): AppDatabase = synchronized(this) {
            val cached = instance
            if (cached != null && instanceOwnerId == ownerId) return cached

            cached?.close() // 换了账号：关掉上一个账号的库连接，再开新账号的
            // 新版 sqlcipher-android 不再提供旧版 SQLiteDatabase.loadLibs(context)，
            // 需要在首次使用前显式装载 AAR 中的 native SQLCipher 库。
            System.loadLibrary("sqlcipher")
            val db = Room.databaseBuilder(
                context.applicationContext, AppDatabase::class.java, "jitong_$ownerId.db",
            )
                // Factory 会持有/使用传入的口令数组；传副本，避免影响调用方持有的 key。
                .openHelperFactory(SupportOpenHelperFactory(key.copyOf()))
                .addMigrations(MIGRATION_2_3, MIGRATION_3_4, MIGRATION_4_5, MIGRATION_5_6,
                    MIGRATION_6_7, MIGRATION_7_8, MIGRATION_8_9, MIGRATION_9_10,
                    MIGRATION_10_11)
                .addCallback(object : RoomDatabase.Callback() {
                    override fun onCreate(db: androidx.sqlite.db.SupportSQLiteDatabase) {
                        installChangeLogTriggers(db)
                        installWriteFenceTriggers(db)
                    }
                    override fun onOpen(db: androidx.sqlite.db.SupportSQLiteDatabase) {
                        installChangeLogTriggers(db)
                        installWriteFenceTriggers(db)
                    }
                })
                // P6-T02：去掉 .fallbackToDestructiveMigration()。
                // 破坏性迁移会静默清空用户本地聊天记录；升级路径已由上面的显式 MIGRATION_*
                // 覆盖，未覆盖的路径应**报错**而不是重建。密钥不可用/库打不开时保留密文文件
                // 供诊断与人工恢复（由 DbKeyManager.clearLocalDatabase 显式清除）。
                .build()
            instance = db
            instanceOwnerId = ownerId
            db
        }

        /** 退出登录或 ViewModel 销毁时释放文件句柄，并清除单例引用。 */
        fun closeCurrent() = synchronized(this) {
            instance?.close()
            instance = null
            instanceOwnerId = null
        }
    }
}
