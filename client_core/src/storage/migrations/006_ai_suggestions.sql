-- P7-G4：AI 候选回复持久化。
--
-- 语义（v2 §3 P7-G4）：AI 建议是"冻结上下文（conversationId/tone/contextVersion）"的候选回复。
-- 状态机（pending→success/error/aborted）由 AiSuggestionService 维护；本表只落最终结果，
-- 供 UI 重建后按 dbVersion/requestId 恢复快照（可重放快照，G1 事件契约）。
--
-- 注意：suggestions 用 US(0x1F) 分隔（候选文本极少含该控制字符），由 AiSuggestionService
-- 序列化/反序列化。候选正文不得写日志（G4 安全要求），但允许加密落库（本库 SQLCipher）。

CREATE TABLE IF NOT EXISTS ai_suggestions (
    request_id      TEXT    NOT NULL,
    owner_id        INTEGER NOT NULL,
    conversation_id INTEGER NOT NULL,
    peer_id         INTEGER NOT NULL DEFAULT 0,
    tone            TEXT    NOT NULL DEFAULT '',
    status          INTEGER NOT NULL DEFAULT 0,  -- 0=pending 1=success 2=error 3=aborted
    suggestions     TEXT    NOT NULL DEFAULT '', -- US(0x1F) 分隔的候选列表
    error_code      INTEGER NOT NULL DEFAULT 0,
    generated_at    INTEGER NOT NULL DEFAULT 0,
    context_version TEXT    NOT NULL DEFAULT '',
    PRIMARY KEY(owner_id, request_id)
);
CREATE INDEX IF NOT EXISTS idx_ai_suggestions_conv
    ON ai_suggestions(owner_id, conversation_id, generated_at);
