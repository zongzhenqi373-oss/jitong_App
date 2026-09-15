-- P7-G2：状态机实体（Schema v3）
--
-- 依据 kernel-round7-implementation-plan.md §3 P7-G2：
--   这些表是"支撑状态机"而非简单宽表——好友申请需方向/去重，补洞需区间合并拆分，
--   媒体需三档变体 + 引用计数，传输需 generation 防串号与终态，迁移需每流水位与
--   两阶段 cutover journal（ADR-03 单向 epoch）。
--
-- 单事务执行（由 SchemaManager 保证），任一步失败整体回滚，绝不删库。

-- 1) 好友申请：独立于 friends.apply_state，支持方向、去重与服务端版本
CREATE TABLE IF NOT EXISTS friend_requests (
    request_id     TEXT    NOT NULL,
    owner_id       INTEGER NOT NULL,
    from_user_id   INTEGER NOT NULL,
    to_user_id     INTEGER NOT NULL,
    direction      INTEGER NOT NULL DEFAULT 0,  -- 0=incoming 1=outgoing
    state          INTEGER NOT NULL DEFAULT 0,  -- 0=pending 1=accepted 2=rejected
    message        TEXT    NOT NULL DEFAULT '',
    server_version INTEGER NOT NULL DEFAULT 0,
    updated_at     INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(owner_id, request_id)
);
CREATE INDEX IF NOT EXISTS idx_friend_requests_state
    ON friend_requests(owner_id, state, updated_at);

-- 2) 同步缺洞区间：闭区间 [gap_from, gap_to]，支持合并/拆分与退避重试
CREATE TABLE IF NOT EXISTS sync_gaps (
    owner_id        INTEGER NOT NULL,
    conversation_id INTEGER NOT NULL,
    gap_from        INTEGER NOT NULL,
    gap_to          INTEGER NOT NULL,
    attempt         INTEGER NOT NULL DEFAULT 0,
    next_retry_at   INTEGER NOT NULL DEFAULT 0,
    updated_at      INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(owner_id, conversation_id, gap_from)
);
CREATE INDEX IF NOT EXISTS idx_sync_gaps_retry
    ON sync_gaps(owner_id, next_retry_at);

-- 3) 媒体变体：原图 / 大缩略图 / 小缩略图三档
--    real_mime 必须反映真实编码格式（禁止 JPEG 字节标成 AVIF）
CREATE TABLE IF NOT EXISTS media_variants (
    media_id   TEXT    NOT NULL,
    variant    INTEGER NOT NULL DEFAULT 0,  -- 0=origin 1=large thumbnail 2=small thumbnail
    real_mime  TEXT    NOT NULL DEFAULT '',
    width      INTEGER NOT NULL DEFAULT 0,
    height     INTEGER NOT NULL DEFAULT 0,
    file_size  INTEGER NOT NULL DEFAULT 0,
    file_hash  TEXT    NOT NULL DEFAULT '',
    local_path TEXT    NOT NULL DEFAULT '',
    state      INTEGER NOT NULL DEFAULT 0,  -- 0=pending 1=ready 2=failed
    PRIMARY KEY(media_id, variant)
);
CREATE INDEX IF NOT EXISTS idx_media_variants_hash
    ON media_variants(file_hash);

-- 4) 媒体引用：消息与媒体的关联；文件授权仍以服务端为准，本地只做引用计数
CREATE TABLE IF NOT EXISTS media_refs (
    owner_id INTEGER NOT NULL,
    msg_id   TEXT    NOT NULL,
    media_id TEXT    NOT NULL,
    variant  INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(owner_id, msg_id, media_id, variant)
);
CREATE INDEX IF NOT EXISTS idx_media_refs_media
    ON media_refs(media_id);

-- 5) 分片上传：仅当服务端声明 resumable_upload_v1 时才产生记录；
--    能力关闭时不得写入任何 part，避免伪断点状态
CREATE TABLE IF NOT EXISTS transfer_parts (
    task_id      TEXT    NOT NULL,
    part_index   INTEGER NOT NULL,
    offset_bytes INTEGER NOT NULL DEFAULT 0,
    size_bytes   INTEGER NOT NULL DEFAULT 0,
    state        INTEGER NOT NULL DEFAULT 0,  -- 0=pending 1=done 2=failed
    checksum     TEXT    NOT NULL DEFAULT '',
    updated_at   INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(task_id, part_index)
);

-- 6) 迁移 checkpoint：每流独立水位（ADR-04 只记录权威流）
CREATE TABLE IF NOT EXISTS migration_checkpoint (
    epoch            INTEGER NOT NULL,
    stream           TEXT    NOT NULL,  -- messages/conversations/watermark/media
    checkpoint_value TEXT    NOT NULL DEFAULT '',
    updated_at       INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(epoch, stream)
);

-- 7) Cutover journal：两阶段单向 epoch（ADR-03）
--    state: 0=LEGACY_ACTIVE 1=PREPARED
--           2=NATIVE_COMMITTED_NO_WRITE 3=NATIVE_COMMITTED_DIRTY
CREATE TABLE IF NOT EXISTS cutover_journal (
    epoch          INTEGER PRIMARY KEY,
    state          INTEGER NOT NULL DEFAULT 0,
    high_water     INTEGER NOT NULL DEFAULT 0,
    schema_version INTEGER NOT NULL DEFAULT 0,
    key_id         TEXT    NOT NULL DEFAULT '',
    summary        TEXT    NOT NULL DEFAULT '',
    dirty          INTEGER NOT NULL DEFAULT 0,
    updated_at     INTEGER NOT NULL DEFAULT 0
);

-- 8) 扩充 transfer_tasks：generation 防换号串号、阶段、断点 offset、错误域、终态
ALTER TABLE transfer_tasks ADD COLUMN generation INTEGER NOT NULL DEFAULT 0;
ALTER TABLE transfer_tasks ADD COLUMN phase INTEGER NOT NULL DEFAULT 0;
ALTER TABLE transfer_tasks ADD COLUMN offset_bytes INTEGER NOT NULL DEFAULT 0;
ALTER TABLE transfer_tasks ADD COLUMN error_domain TEXT NOT NULL DEFAULT '';
ALTER TABLE transfer_tasks ADD COLUMN terminal_state INTEGER NOT NULL DEFAULT 0;

CREATE INDEX IF NOT EXISTS idx_transfer_tasks_generation
    ON transfer_tasks(owner_id, generation);
