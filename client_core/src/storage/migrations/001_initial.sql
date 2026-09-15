-- P6-T03 初始 Schema（version = 1）
--
-- 约定：
--   - msg_id 负责幂等，conversation_seq 负责顺序与补洞，二者不可替代；
--   - 同 (owner_id, conversation_seq) 只能对应一个 msg_id，冲突即数据一致性错误；
--   - FTS 必须与 Room 的 @Fts4 保持**默认 tokenizer**（simple），不写 tokenize 子句；
--     Room 侧靠 pinyin/initials 两列做中文检索，Native 必须原样搬运这两列，
--     否则会出现"正文存在但拼音搜不到"的倒退。
--   - 媒体只存元数据（路径/大小/sha256），不移动/复制/删除媒体文件。

CREATE TABLE messages (
    id               INTEGER PRIMARY KEY,
    owner_id         INTEGER NOT NULL,
    msg_id           TEXT    NOT NULL,
    conversation_id  INTEGER NOT NULL DEFAULT 0, -- 对应 Room conversations.conversationId
    peer_id          INTEGER NOT NULL,
    conversation_seq INTEGER NOT NULL DEFAULT 0, -- 会话级序号（服务端权威，顺序与补洞）
    server_time      INTEGER NOT NULL DEFAULT 0,
    local_order      INTEGER NOT NULL DEFAULT 0, -- 本地单调序号（未确认/离线消息排序）
    from_me          INTEGER NOT NULL DEFAULT 0,
    type             INTEGER NOT NULL DEFAULT 0, -- 0=TEXT 1=IMAGE 2=FILE
    content          TEXT,
    status           INTEGER NOT NULL DEFAULT 0, -- 0发送中 1已送达 2已接收 3离线转存
    media_path       TEXT,
    img_w            INTEGER NOT NULL DEFAULT 0,
    img_h            INTEGER NOT NULL DEFAULT 0,
    file_id          TEXT    NOT NULL DEFAULT '',
    file_name        TEXT    NOT NULL DEFAULT '',
    file_size        INTEGER NOT NULL DEFAULT 0,
    content_type     TEXT    NOT NULL DEFAULT '',
    sha256           TEXT    NOT NULL DEFAULT '',
    thumbnail_file_id      TEXT NOT NULL DEFAULT '',
    thumbnail_path         TEXT,
    thumbnail_size         INTEGER NOT NULL DEFAULT 0,
    thumbnail_sha256       TEXT NOT NULL DEFAULT '',
    thumbnail_w            INTEGER NOT NULL DEFAULT 0,
    thumbnail_h            INTEGER NOT NULL DEFAULT 0,
    large_thumbnail_file_id TEXT NOT NULL DEFAULT '',
    large_thumbnail_path    TEXT,
    large_thumbnail_size    INTEGER NOT NULL DEFAULT 0,
    large_thumbnail_sha256  TEXT NOT NULL DEFAULT '',
    large_thumbnail_w       INTEGER NOT NULL DEFAULT 0,
    large_thumbnail_h       INTEGER NOT NULL DEFAULT 0,
    local_path       TEXT,
    transferred      INTEGER NOT NULL DEFAULT 0
);

-- msg_id 幂等：漫游合并/重发不重复
CREATE UNIQUE INDEX idx_messages_owner_msg ON messages(owner_id, msg_id);
-- 会话内按服务端权威序号分页/排序
CREATE INDEX idx_messages_conversation_seq ON messages(owner_id, peer_id, conversation_seq);
-- 时间序分页（keyset）
CREATE INDEX idx_messages_server_time ON messages(owner_id, peer_id, server_time, msg_id);
-- 失败重发 / 未读类查询
CREATE INDEX idx_messages_status ON messages(owner_id, status, local_order);
-- 同 seq 只能对应一个 msg_id（占位消息 seq=0 不参与）
CREATE UNIQUE INDEX idx_messages_owner_peer_seq
    ON messages(owner_id, peer_id, conversation_seq) WHERE conversation_seq > 0;

-- 会话表
CREATE TABLE conversations (
    conversation_id    INTEGER PRIMARY KEY,
    owner_id           INTEGER NOT NULL,
    peer_id            INTEGER NOT NULL,
    last_message       TEXT,
    last_message_time  INTEGER NOT NULL DEFAULT 0,
    unread             INTEGER NOT NULL DEFAULT 0,
    read_seq           INTEGER NOT NULL DEFAULT 0
);
CREATE UNIQUE INDEX idx_conversations_owner_peer ON conversations(owner_id, peer_id);
CREATE INDEX idx_conversations_recent ON conversations(owner_id, last_message_time);

-- 好友 / 申请
CREATE TABLE friends (
    owner_id    INTEGER NOT NULL,
    friend_id   INTEGER NOT NULL,
    nick        TEXT    NOT NULL DEFAULT '',
    remark      TEXT    NOT NULL DEFAULT '',
    apply_msg   TEXT    NOT NULL DEFAULT '',
    apply_state INTEGER NOT NULL DEFAULT 0,
    ts          INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (owner_id, friend_id)
);
CREATE INDEX idx_friends_apply ON friends(owner_id, apply_state);

-- 全文索引：与 Room 的 @Fts4 一致（默认 tokenizer，独立存储）
CREATE VIRTUAL TABLE message_fts USING fts4(content, pinyin, initials, msg_id);

-- 同步水位（增量补洞）
CREATE TABLE sync_watermarks (
    owner_id   INTEGER NOT NULL,
    domain     TEXT    NOT NULL,
    peer_id    INTEGER NOT NULL DEFAULT 0,
    synced_seq INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (owner_id, domain, peer_id)
);

-- 发送队列（发号器状态全在 C++）
CREATE TABLE outbox (
    local_order    INTEGER PRIMARY KEY,
    owner_id       INTEGER NOT NULL,
    msg_id         TEXT    NOT NULL,
    packet_type    INTEGER NOT NULL DEFAULT 0,
    payload        BLOB    NOT NULL,
    retry_count    INTEGER NOT NULL DEFAULT 0,
    next_retry_at  INTEGER NOT NULL DEFAULT 0,
    last_error     TEXT
);
CREATE INDEX idx_outbox_retry ON outbox(owner_id, next_retry_at);
CREATE UNIQUE INDEX idx_outbox_msg ON outbox(owner_id, msg_id);

-- 媒体文件记录（只登记元信息，不移动/复制/删除文件）
CREATE TABLE media_records (
    file_id    TEXT PRIMARY KEY,
    owner_id   INTEGER NOT NULL,
    category   TEXT    NOT NULL DEFAULT '',
    file_size  INTEGER NOT NULL DEFAULT 0,
    sha256     TEXT    NOT NULL DEFAULT '',
    local_path TEXT,
    ref_count  INTEGER NOT NULL DEFAULT 0,
    state      INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX idx_media_owner_state ON media_records(owner_id, state);

-- 传输任务（上传/下载续传）
CREATE TABLE transfer_tasks (
    task_id     TEXT PRIMARY KEY,
    owner_id    INTEGER NOT NULL,
    msg_id      TEXT    NOT NULL DEFAULT '',
    file_id     TEXT    NOT NULL DEFAULT '',
    direction   INTEGER NOT NULL DEFAULT 0, -- 0=上传 1=下载
    local_path  TEXT,
    total_size  INTEGER NOT NULL DEFAULT 0,
    transferred INTEGER NOT NULL DEFAULT 0,
    state       INTEGER NOT NULL DEFAULT 0,
    updated_at  INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX idx_transfer_owner_state ON transfer_tasks(owner_id, state);

-- 迁移元数据（影子迁移 checkpoint / 完成标记）
CREATE TABLE migration_meta (
    key   TEXT PRIMARY KEY,
    value TEXT
);
