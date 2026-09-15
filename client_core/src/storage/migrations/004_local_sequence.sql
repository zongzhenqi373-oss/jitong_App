-- Allocation and draft insertion share the Writer transaction. Never reuse an ACKed order.
CREATE TABLE local_sequence(owner_id INTEGER PRIMARY KEY, last_order INTEGER NOT NULL CHECK(last_order >= 0));
INSERT INTO local_sequence(owner_id,last_order)
SELECT owner_id, MAX(local_order) FROM (
 SELECT owner_id,local_order FROM messages
 UNION ALL SELECT owner_id,local_order FROM outbox
) GROUP BY owner_id;
CREATE INDEX idx_messages_local_order ON messages(local_order);
ALTER TABLE outbox ADD COLUMN payload_version INTEGER NOT NULL DEFAULT 0;
ALTER TABLE outbox ADD COLUMN finished_attempt INTEGER NOT NULL DEFAULT 0;
ALTER TABLE conversations ADD COLUMN last_msg_id TEXT NOT NULL DEFAULT '';
