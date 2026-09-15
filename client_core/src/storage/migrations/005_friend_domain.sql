-- P7-G4：好友域资料补全。
--
-- friends 表（001 建立）当前只有 nick/remark/apply_msg/apply_state/ts，缺少 FriendDto 的
-- 完整事实字段 tel/avatar/signature/sex。补齐这些字段，使 Legacy/Native 能逐字段对账
-- （P6 教训：字段遗漏会导致 Golden 对账失效）。
--
-- 注意：FriendDto.online 是 Presence（路由/在线状态），**不进 friends 表**——好友事实
-- 与在线事件分离（v2 §3 P7-G4：Presence 只表达路由状态，不作好友事实源）。
--
-- friend_requests.created_at 与 updated_at 分离：创建时间不随同意/拒绝而改变，
-- 列表按创建时间排序不受状态变更影响。

ALTER TABLE friends ADD COLUMN tel TEXT NOT NULL DEFAULT '';
ALTER TABLE friends ADD COLUMN avatar TEXT NOT NULL DEFAULT '';
ALTER TABLE friends ADD COLUMN signature TEXT NOT NULL DEFAULT '';
ALTER TABLE friends ADD COLUMN sex INTEGER NOT NULL DEFAULT 0;

ALTER TABLE friend_requests ADD COLUMN created_at INTEGER NOT NULL DEFAULT 0;
