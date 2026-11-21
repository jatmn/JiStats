-- ============================================
-- MIGRATION 006: Add Provider Capabilities
-- ============================================

CREATE TABLE IF NOT EXISTS provider_capabilities (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    provider_id INTEGER NOT NULL REFERENCES providers(id),
    capability_key TEXT NOT NULL,
    supported INTEGER NOT NULL DEFAULT 0,
    config_json TEXT,
    discovered_at_utc TEXT,
    UNIQUE(provider_id, capability_key)
);

CREATE INDEX IF NOT EXISTS idx_provider_capabilities_provider_key
    ON provider_capabilities(provider_id, capability_key);

INSERT OR IGNORE INTO schema_version (version, description)
VALUES (6, 'Add provider capability metadata table');
