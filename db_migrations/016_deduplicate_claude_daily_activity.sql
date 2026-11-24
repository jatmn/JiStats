-- Cache revisions are provenance, not the identity of a daily activity row.
WITH claude_days AS (
    SELECT id,
           ROW_NUMBER() OVER (
               PARTITION BY provider_id, account_id, started_at_utc, raw_reference ORDER BY id
           ) AS occurrence
    FROM activity_events
    WHERE provider_surface = 'claude_code_local_stats'
      AND activity_kind = 'session_active'
      AND source_type = 'local'
      AND surface_key = 'stats-cache'
      AND raw_reference GLOB 'scope:fnv1a64:?*'
)
DELETE FROM activity_events
WHERE id IN (SELECT id FROM claude_days WHERE occurrence > 1);

CREATE UNIQUE INDEX IF NOT EXISTS idx_activity_events_claude_cache_day
ON activity_events(provider_id, account_id, started_at_utc, raw_reference)
WHERE provider_surface = 'claude_code_local_stats'
  AND activity_kind = 'session_active'
  AND source_type = 'local'
  AND surface_key = 'stats-cache'
  AND raw_reference GLOB 'scope:fnv1a64:?*';

INSERT INTO schema_version (version, description)
VALUES (16, 'Deduplicate Claude daily stats-cache activity');
