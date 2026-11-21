-- ============================================
-- MIGRATION 011: Add persisted quota snapshot rollups
-- ============================================

CREATE TABLE IF NOT EXISTS quota_snapshot_rollups_daily (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    provider_id INTEGER NOT NULL REFERENCES providers(id),
    account_id INTEGER NOT NULL REFERENCES accounts(id),
    plan_bucket_id INTEGER NOT NULL DEFAULT 0,
    quota_dimension TEXT NOT NULL DEFAULT 'requests',
    rollup_period_start_utc TEXT NOT NULL,
    sample_count INTEGER NOT NULL DEFAULT 0,
    latest_snapshot_time_utc TEXT NOT NULL,
    latest_window_start_utc TEXT NOT NULL,
    latest_window_end_utc TEXT NOT NULL,
    latest_used_value INTEGER NOT NULL DEFAULT 0,
    latest_limit_value INTEGER,
    latest_remaining_value INTEGER,
    low_water_remaining_value INTEGER,
    low_water_limit_value INTEGER,
    reset_count INTEGER NOT NULL DEFAULT 0,
    reconciled_sample_count INTEGER NOT NULL DEFAULT 0,
    latest_confidence_tier TEXT NOT NULL DEFAULT 'Unknown',
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    updated_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    UNIQUE(provider_id, account_id, plan_bucket_id, quota_dimension, rollup_period_start_utc)
);

CREATE INDEX IF NOT EXISTS idx_quota_snapshot_rollups_daily_scope_period
    ON quota_snapshot_rollups_daily(provider_id, account_id, rollup_period_start_utc);

CREATE INDEX IF NOT EXISTS idx_quota_snapshot_rollups_daily_series_period
    ON quota_snapshot_rollups_daily(provider_id, account_id, plan_bucket_id, quota_dimension, rollup_period_start_utc);

INSERT OR IGNORE INTO schema_version (version, description)
VALUES (11, 'Add persisted daily quota snapshot rollups');
