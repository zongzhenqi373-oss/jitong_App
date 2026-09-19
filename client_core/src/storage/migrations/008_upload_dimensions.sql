-- 图片草稿的宽高随上传状态持久化，重启后生成卡片仍可等比占位。
ALTER TABLE upload_drafts ADD COLUMN image_width INTEGER NOT NULL DEFAULT 0;
ALTER TABLE upload_drafts ADD COLUMN image_height INTEGER NOT NULL DEFAULT 0;
