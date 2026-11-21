-- ============================================
-- MIGRATION 010: Add analysis/reporting persistence metadata
-- ============================================

ALTER TABLE usage_rollups_hourly ADD COLUMN local_request_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE usage_rollups_hourly ADD COLUMN imported_request_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE usage_rollups_hourly ADD COLUMN api_request_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE usage_rollups_hourly ADD COLUMN unknown_request_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE usage_rollups_hourly ADD COLUMN backfilled_request_count INTEGER NOT NULL DEFAULT 0;

ALTER TABLE usage_rollups_daily ADD COLUMN local_request_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE usage_rollups_daily ADD COLUMN imported_request_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE usage_rollups_daily ADD COLUMN api_request_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE usage_rollups_daily ADD COLUMN unknown_request_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE usage_rollups_daily ADD COLUMN backfilled_request_count INTEGER NOT NULL DEFAULT 0;

CREATE TABLE IF NOT EXISTS recovery_checkpoints (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    provider_id INTEGER NOT NULL REFERENCES providers(id),
    account_id INTEGER NOT NULL REFERENCES accounts(id),
    sync_kind TEXT NOT NULL,
    recovery_start_utc TEXT NOT NULL,
    recovery_end_utc TEXT NOT NULL,
    attempt_at_utc TEXT NOT NULL,
    outcome TEXT NOT NULL,
    fetched_event_count INTEGER NOT NULL DEFAULT 0,
    comparison_result TEXT NOT NULL DEFAULT 'not_available',
    local_event_count INTEGER NOT NULL DEFAULT 0,
    local_total_tokens INTEGER NOT NULL DEFAULT 0,
    local_total_cost_minor_units INTEGER NOT NULL DEFAULT 0,
    imported_total_tokens INTEGER NOT NULL DEFAULT 0,
    imported_total_cost_minor_units INTEGER NOT NULL DEFAULT 0,
    failure_category TEXT,
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_recovery_checkpoints_provider_account_kind_attempt
    ON recovery_checkpoints(provider_id, account_id, sync_kind, attempt_at_utc);

CREATE TABLE IF NOT EXISTS export_runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    run_kind TEXT NOT NULL,
    format TEXT NOT NULL DEFAULT 'html',
    requested_at_utc TEXT NOT NULL,
    completed_at_utc TEXT,
    success INTEGER NOT NULL DEFAULT 0,
    output_path TEXT,
    error_summary TEXT,
    range_start_utc TEXT,
    range_end_utc TEXT,
    provider_filter_csv TEXT,
    account_filter_csv TEXT,
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_export_runs_kind_requested
    ON export_runs(run_kind, requested_at_utc);

INSERT OR IGNORE INTO schema_version (version, description)
VALUES (10, 'Add analysis/reporting provenance, recovery checkpoints, and export history');
