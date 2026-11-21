-- ============================================
-- MIGRATION 008: Add gap marker persistence
-- ============================================

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

INSERT OR IGNORE INTO schema_version (version, description)
VALUES (8, 'Add persisted gap markers and classification state');
