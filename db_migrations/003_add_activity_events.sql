-- ============================================
-- MIGRATION 003: Add activity_events table
-- ============================================

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

INSERT OR IGNORE INTO schema_version (version, description)
VALUES (3, 'Add activity_events table for subscription/local activity observation');
