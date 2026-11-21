-- ============================================
-- MIGRATION 001: Initial Schema
-- ============================================

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
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

-- Settings table
CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

-- Insert initial schema version
INSERT OR IGNORE INTO schema_version (version, description) VALUES (1, 'Initial schema');

-- Insert default providers
INSERT OR IGNORE INTO providers (provider_key, display_name, enabled) VALUES 
    ('openai', 'OpenAI', 1),
    ('minimax', 'MiniMax', 1);

-- Insert default settings
INSERT OR IGNORE INTO settings (key, value) VALUES 
    ('app.version', '1.0.0'),
    ('monitoring.enabled', 'false'),
    ('monitoring.interval_minutes', '15'),
    ('ui.theme', 'dark');
