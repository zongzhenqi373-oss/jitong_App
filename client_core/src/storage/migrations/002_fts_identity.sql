-- 为 FTS4 增加可按 (owner_id,msg_id) O(1) 校验的规范镜像。
-- FTS4 普通列没有 B-tree 索引，直接按 msg_id 扫描会让重复导入退化为 O(n²)。
CREATE TABLE message_fts_identity (
    owner_id INTEGER NOT NULL,
    msg_id   TEXT    NOT NULL,
    content  TEXT    NOT NULL,
    pinyin   TEXT    NOT NULL DEFAULT '',
    initials TEXT    NOT NULL DEFAULT '',
    PRIMARY KEY(owner_id, msg_id)
) WITHOUT ROWID;

INSERT INTO message_fts_identity(owner_id,msg_id,content,pinyin,initials)
SELECT m.owner_id,f.msg_id,f.content,f.pinyin,f.initials
FROM message_fts AS f
JOIN messages AS m ON m.msg_id=f.msg_id;
