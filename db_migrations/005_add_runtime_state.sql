-- ============================================
-- MIGRATION 005: Add Runtime State
-- ============================================

CREATE TABLE IF NOT EXISTS runtime_state (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    last_launch_at_utc TEXT,
    last_successful_sync_utc TEXT,
    last_local_observation_utc TEXT,
    recovery_marker TEXT,
    created_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    updated_at_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

INSERT OR IGNORE INTO runtime_state (id) VALUES (1);

INSERT OR IGNORE INTO schema_version (version, description)
VALUES (5, 'Add runtime state singleton table');
