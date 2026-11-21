-- ============================================
-- MIGRATION 012: Codex local SQLite usage state
-- ============================================

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

INSERT OR IGNORE INTO providers (provider_key, display_name, enabled)
VALUES ('hicap-ai', 'HICAP Ai', 1);

INSERT OR IGNORE INTO schema_version (version, description)
VALUES (12, 'Add Codex local SQLite usage import state');
