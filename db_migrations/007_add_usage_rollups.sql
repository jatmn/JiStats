-- ============================================
-- MIGRATION 007: Add usage rollup caches
-- ============================================

CREATE TABLE IF NOT EXISTS usage_rollups_hourly (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    provider_id INTEGER NOT NULL REFERENCES providers(id),
    account_id INTEGER NOT NULL REFERENCES accounts(id),
    rollup_period_start_utc TEXT NOT NULL,
    model_key TEXT NOT NULL DEFAULT '',
    request_count INTEGER NOT NULL DEFAULT 0,
    input_tokens INTEGER NOT NULL DEFAULT 0,
    output_tokens INTEGER NOT NULL DEFAULT 0,
    total_tokens INTEGER NOT NULL DEFAULT 0,
    total_cost_minor_units INTEGER NOT NULL DEFAULT 0,
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    updated_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    UNIQUE(provider_id, account_id, rollup_period_start_utc, model_key)
);

CREATE TABLE IF NOT EXISTS usage_rollups_daily (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    provider_id INTEGER NOT NULL REFERENCES providers(id),
    account_id INTEGER NOT NULL REFERENCES accounts(id),
    rollup_period_start_utc TEXT NOT NULL,
    model_key TEXT NOT NULL DEFAULT '',
    request_count INTEGER NOT NULL DEFAULT 0,
    input_tokens INTEGER NOT NULL DEFAULT 0,
    output_tokens INTEGER NOT NULL DEFAULT 0,
    total_tokens INTEGER NOT NULL DEFAULT 0,
    total_cost_minor_units INTEGER NOT NULL DEFAULT 0,
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    updated_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    UNIQUE(provider_id, account_id, rollup_period_start_utc, model_key)
);

CREATE INDEX IF NOT EXISTS idx_usage_rollups_hourly_provider_account_period
    ON usage_rollups_hourly(provider_id, account_id, rollup_period_start_utc);

CREATE INDEX IF NOT EXISTS idx_usage_rollups_daily_provider_account_period
    ON usage_rollups_daily(provider_id, account_id, rollup_period_start_utc);

INSERT OR IGNORE INTO schema_version (version, description)
VALUES (7, 'Add hourly and daily usage rollup caches');
