-- 007: 分片上传草稿（断点续传本地事实源）。
--
-- 语义（对应服务端 upload_sessions/upload_chunks 协议）：
--   - 一条待发送消息草稿由固定 msg_id 标识；图片的原图/小图/大图各占一行
--     （variant 维度独立上传状态），共享同一 msg_id。
--   - state：0=pending（待传） 1=uploading 2=finalized（已得 file_id，消息未发）
--           3=sent（消息已入 Outbox）4=cancelled（用户取消，终态，不自动恢复）
--           5=failed（可恢复失败：断网/服务端 5xx；重启后 resume）
--   - 恢复纪律：重启后先 GET 服务端会话拿已收分片，与本地 chunks_done 取并集前的
--     交集校验，不能只信本地进度。
--   - chunks_done：已确认完成的分片号，逗号分隔升序（如 "0,1,2"）；分片数上限
--     100MB/1MiB=100，文本表示足够。
--   - file_id 只在 finalize 成功后写入；sent 只在 Outbox 事务提交后写入。

CREATE TABLE IF NOT EXISTS upload_drafts (
    owner_id        INTEGER NOT NULL,
    msg_id          TEXT NOT NULL,          -- 固定消息 ID（重试/恢复不重新生成）
    variant         INTEGER NOT NULL,       -- MediaVariant：0=原图 1=大图 2=小图
    conversation_id INTEGER NOT NULL,
    peer_id         INTEGER NOT NULL,
    local_path      TEXT NOT NULL,
    file_name       TEXT NOT NULL,
    file_size       INTEGER NOT NULL,
    sha256          TEXT NOT NULL,
    content_type    TEXT NOT NULL DEFAULT '',
    upload_id       TEXT NOT NULL DEFAULT '',  -- 服务端会话；空=尚未创建
    chunk_size      INTEGER NOT NULL DEFAULT 0,
    chunk_count     INTEGER NOT NULL DEFAULT 0,
    chunks_done     TEXT NOT NULL DEFAULT '',  -- 已确认分片号，逗号分隔
    file_id         TEXT NOT NULL DEFAULT '',  -- finalize 后回填
    state           INTEGER NOT NULL DEFAULT 0,
    error_code      INTEGER NOT NULL DEFAULT 0,
    created_at      INTEGER NOT NULL,
    updated_at      INTEGER NOT NULL,
    PRIMARY KEY (owner_id, msg_id, variant)
);

-- 恢复扫描：启动时按 owner 找未终态草稿
CREATE INDEX IF NOT EXISTS idx_upload_drafts_active
    ON upload_drafts(owner_id, state);
