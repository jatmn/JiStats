-- ============================================
-- MIGRATION 004: Add Mirror State Tables
-- ============================================

-- Expand quota snapshots with mirror-specific metadata.
ALTER TABLE quota_snapshots ADD COLUMN quota_dimension TEXT NOT NULL DEFAULT 'requests';
ALTER TABLE quota_snapshots ADD COLUMN remaining_value INTEGER;
ALTER TABLE quota_snapshots ADD COLUMN is_reconciled INTEGER NOT NULL DEFAULT 0;

-- Best-effort backfill for existing rows that already have both used and limit values.
UPDATE quota_snapshots
SET remaining_value = limit_value - used_value
WHERE remaining_value IS NULL
  AND limit_value IS NOT NULL;

-- Sync state tracks reconciliation/import cursor progress per provider account.
CREATE TABLE IF NOT EXISTS sync_state (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    provider_id INTEGER NOT NULL REFERENCES providers(id),
    account_id INTEGER NOT NULL REFERENCES accounts(id),
    sync_kind TEXT NOT NULL,
    cursor_value TEXT,
    last_attempt_at_utc TEXT,
    last_success_at_utc TEXT,
    last_failure_at_utc TEXT,
    last_failure_category TEXT,
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    updated_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    UNIQUE(provider_id, account_id, sync_kind)
);

-- Mirror rules capture provider-specific quota mirroring behavior and versioning.
CREATE TABLE IF NOT EXISTS mirror_rules (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    provider_id INTEGER NOT NULL REFERENCES providers(id),
    rule_key TEXT NOT NULL,
    rule_version INTEGER NOT NULL DEFAULT 1,
    config_json TEXT,
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    UNIQUE(provider_id, rule_key, rule_version)
);

-- Connectivity state keeps the last-known retry/offline state per provider account.
CREATE TABLE IF NOT EXISTS connectivity_state (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    provider_id INTEGER NOT NULL REFERENCES providers(id),
    account_id INTEGER NOT NULL REFERENCES accounts(id),
    status TEXT NOT NULL DEFAULT 'online',
    failure_category TEXT,
    last_checked_at_utc TEXT,
    next_retry_at_utc TEXT,
    notice_dismissed_at_utc TEXT,
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    updated_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    UNIQUE(provider_id, account_id)
);

CREATE INDEX IF NOT EXISTS idx_quota_snapshots_dimension_time
    ON quota_snapshots(quota_dimension, snapshot_time_utc);
CREATE INDEX IF NOT EXISTS idx_sync_state_provider_account_kind
    ON sync_state(provider_id, account_id, sync_kind);
CREATE INDEX IF NOT EXISTS idx_connectivity_state_provider_account
    ON connectivity_state(provider_id, account_id);
CREATE INDEX IF NOT EXISTS idx_mirror_rules_provider_key
    ON mirror_rules(provider_id, rule_key);

INSERT OR IGNORE INTO schema_version (version, description)
VALUES (4, 'Add mirror-state tables and quota snapshot metadata');
