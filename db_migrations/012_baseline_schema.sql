-- JiStats SQLite baseline (schema version 12).

-- Schema version tracking
CREATE TABLE IF NOT EXISTS schema_version (
    version INTEGER PRIMARY KEY,
    applied_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    description TEXT NOT NULL
);

-- Providers table
CREATE TABLE IF NOT EXISTS providers (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    provider_key TEXT NOT NULL UNIQUE,
    display_name TEXT NOT NULL,
    enabled INTEGER NOT NULL DEFAULT 1,
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    updated_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

-- Accounts table
CREATE TABLE IF NOT EXISTS accounts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    provider_id INTEGER NOT NULL REFERENCES providers(id),
    account_key TEXT NOT NULL,
    display_name TEXT NOT NULL,
    scope_type TEXT NOT NULL,
    scope_identifier TEXT NOT NULL,
    enabled INTEGER NOT NULL DEFAULT 1,
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    updated_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    UNIQUE(provider_id, account_key)
);

-- Credential profiles table
CREATE TABLE IF NOT EXISTS credential_profiles (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    provider_id INTEGER NOT NULL REFERENCES providers(id),
    account_id INTEGER NOT NULL REFERENCES accounts(id),
    profile_name TEXT NOT NULL,
    secret_ref TEXT NOT NULL,
    credential_type TEXT NOT NULL,
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    updated_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    last_validated_at_utc TEXT,
    UNIQUE(account_id, profile_name)
);

-- Plans table
CREATE TABLE IF NOT EXISTS plans (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id INTEGER NOT NULL REFERENCES accounts(id),
    plan_key TEXT NOT NULL,
    plan_type TEXT NOT NULL,
    display_name TEXT NOT NULL,
    quota_window TEXT NOT NULL,
    limit_value INTEGER,
    is_active INTEGER NOT NULL DEFAULT 1,
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    updated_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    UNIQUE(account_id, plan_key)
);

-- Usage events table
CREATE TABLE IF NOT EXISTS usage_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    provider_id INTEGER NOT NULL REFERENCES providers(id),
    account_id INTEGER NOT NULL REFERENCES accounts(id),
    plan_id INTEGER REFERENCES plans(id),
    event_time_utc TEXT NOT NULL,
    source_type TEXT NOT NULL,
    source_run_id TEXT,
    model_key TEXT,
    request_type TEXT,
    input_tokens INTEGER,
    output_tokens INTEGER,
    total_tokens INTEGER,
    cost_minor_units INTEGER,
    is_backfilled INTEGER NOT NULL DEFAULT 0,
    raw_reference TEXT,
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

-- Quota snapshots table
CREATE TABLE IF NOT EXISTS quota_snapshots (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    provider_id INTEGER NOT NULL REFERENCES providers(id),
    account_id INTEGER NOT NULL REFERENCES accounts(id),
    plan_id INTEGER REFERENCES plans(id),
    snapshot_time_utc TEXT NOT NULL,
    window_start_utc TEXT NOT NULL,
    window_end_utc TEXT NOT NULL,
    used_value INTEGER NOT NULL,
    limit_value INTEGER,
    confidence_tier TEXT NOT NULL,
    basis_summary TEXT,
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    quota_dimension TEXT NOT NULL DEFAULT 'requests',
    remaining_value INTEGER,
    is_reconciled INTEGER NOT NULL DEFAULT 0
);

-- Settings table
CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

-- Locally observed activity
CREATE TABLE IF NOT EXISTS activity_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    provider_id INTEGER NOT NULL REFERENCES providers(id),
    account_id INTEGER NOT NULL REFERENCES accounts(id),
    plan_id INTEGER REFERENCES plans(id),
    provider_surface TEXT NOT NULL,
    activity_kind TEXT NOT NULL,
    started_at_utc TEXT NOT NULL,
    ended_at_utc TEXT,
    duration_seconds INTEGER,
    source_type TEXT NOT NULL DEFAULT 'local',
    source_run_id TEXT,
    surface_key TEXT,
    window_title_hint TEXT,
    confidence_tier TEXT NOT NULL DEFAULT 'LocalExact',
    raw_reference TEXT,
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_activity_events_paid
    ON activity_events(provider_id, account_id, started_at_utc);

CREATE INDEX IF NOT EXISTS idx_activity_events_surface_kind
    ON activity_events(provider_surface, activity_kind, started_at_utc);

CREATE INDEX IF NOT EXISTS idx_activity_events_source
    ON activity_events(source_type, started_at_utc);

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

-- Startup and recovery state
CREATE TABLE IF NOT EXISTS runtime_state (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    last_launch_at_utc TEXT,
    last_successful_sync_utc TEXT,
    last_local_observation_utc TEXT,
    recovery_marker TEXT,
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    updated_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

-- Provider capability discovery
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

-- Hourly and daily usage rollups
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
    local_request_count INTEGER NOT NULL DEFAULT 0,
    imported_request_count INTEGER NOT NULL DEFAULT 0,
    api_request_count INTEGER NOT NULL DEFAULT 0,
    unknown_request_count INTEGER NOT NULL DEFAULT 0,
    backfilled_request_count INTEGER NOT NULL DEFAULT 0,
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
    local_request_count INTEGER NOT NULL DEFAULT 0,
    imported_request_count INTEGER NOT NULL DEFAULT 0,
    api_request_count INTEGER NOT NULL DEFAULT 0,
    unknown_request_count INTEGER NOT NULL DEFAULT 0,
    backfilled_request_count INTEGER NOT NULL DEFAULT 0,
    UNIQUE(provider_id, account_id, rollup_period_start_utc, model_key)
);

CREATE INDEX IF NOT EXISTS idx_usage_rollups_hourly_provider_account_period
    ON usage_rollups_hourly(provider_id, account_id, rollup_period_start_utc);

CREATE INDEX IF NOT EXISTS idx_usage_rollups_daily_provider_account_period
    ON usage_rollups_daily(provider_id, account_id, rollup_period_start_utc);

-- Recovery gap markers
CREATE TABLE IF NOT EXISTS gap_markers (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    provider_id INTEGER NOT NULL REFERENCES providers(id),
    account_id INTEGER NOT NULL REFERENCES accounts(id),
    period_start_utc TEXT NOT NULL,
    period_end_utc TEXT NOT NULL,
    classification TEXT NOT NULL,
    basis_summary TEXT,
    source_kind TEXT,
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    updated_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    UNIQUE(provider_id, account_id, period_start_utc, period_end_utc)
);

CREATE INDEX IF NOT EXISTS idx_gap_markers_provider_account_period
    ON gap_markers(provider_id, account_id, period_start_utc, period_end_utc);

-- Recovery and export metadata
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

-- Daily quota snapshot rollups
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

-- Codex local SQLite import state
CREATE TABLE IF NOT EXISTS codex_local_sources (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_path TEXT NOT NULL,
    source_kind TEXT NOT NULL DEFAULT 'codex_state_sqlite',
    source_identity TEXT NOT NULL,
    first_seen_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    last_seen_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    last_read_utc TEXT,
    UNIQUE(source_path, source_identity)
);

CREATE TABLE IF NOT EXISTS codex_thread_import_state (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_id INTEGER NOT NULL REFERENCES codex_local_sources(id),
    thread_id TEXT NOT NULL,
    model_provider TEXT NOT NULL,
    model_key TEXT NOT NULL,
    reasoning_effort TEXT,
    first_seen_created_at_ms INTEGER,
    last_seen_updated_at_ms INTEGER,
    baseline_tokens_used INTEGER NOT NULL DEFAULT 0,
    last_seen_tokens_used INTEGER NOT NULL DEFAULT 0,
    last_imported_tokens_used INTEGER NOT NULL DEFAULT 0,
    baseline_established_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    last_event_time_utc TEXT,
    counter_reset_count INTEGER NOT NULL DEFAULT 0,
    last_status TEXT NOT NULL DEFAULT 'active',
    updated_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    UNIQUE(source_id, thread_id)
);

CREATE INDEX IF NOT EXISTS idx_codex_local_sources_kind
    ON codex_local_sources(source_kind, last_read_utc);

CREATE INDEX IF NOT EXISTS idx_codex_thread_import_state_provider_model
    ON codex_thread_import_state(model_provider, model_key, last_seen_updated_at_ms);

CREATE INDEX IF NOT EXISTS idx_codex_thread_import_state_source_thread
    ON codex_thread_import_state(source_id, thread_id);

-- Insert default providers
INSERT OR IGNORE INTO providers (provider_key, display_name, enabled) VALUES
    ('openai', 'OpenAI', 1),
    ('minimax', 'MiniMax', 1),
    ('claude_code', 'Claude Code', 1),
    ('kimi', 'Kimi Code', 1),
    ('hicap-ai', 'HICAP Ai', 1),
    ('grok', 'Grok', 1),
    ('cursor', 'Cursor', 1);

-- Insert default settings
INSERT OR IGNORE INTO settings (key, value) VALUES
    ('app.version', '1.0.0'),
    ('monitoring.enabled', 'false'),
    ('monitoring.interval_minutes', '15'),
    ('ui.theme', 'dark'),
    ('ui.taskbar_overlay_enabled', 'true');

INSERT OR IGNORE INTO runtime_state (id) VALUES (1);

INSERT OR IGNORE INTO schema_version (version, description) VALUES (12, 'JiStats baseline schema');
