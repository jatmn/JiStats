-- ============================================
-- MIGRATION 010: Add Kimi Code Provider
-- ============================================

INSERT OR IGNORE INTO schema_version (version, description) VALUES (10, 'Add Kimi Code provider');

INSERT OR IGNORE INTO providers (provider_key, display_name, enabled) VALUES
    ('kimi', 'Kimi Code', 1);
