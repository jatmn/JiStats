-- ============================================
-- MIGRATION 009: Add Claude Code Provider
-- ============================================

INSERT OR IGNORE INTO schema_version (version, description) VALUES (9, 'Add Claude Code provider');

INSERT OR IGNORE INTO providers (provider_key, display_name, enabled) VALUES
    ('claude_code', 'Claude Code', 1);
