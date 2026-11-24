-- Keep the most recent saved state per task, preferring terminal states to running copies.
WITH ranked_tasks AS (
    SELECT id,
           ROW_NUMBER() OVER (
               PARTITION BY provider_id, account_id, raw_reference
               ORDER BY COALESCE(lower(surface_key)
                            IN ('completed', 'failed', 'killed', 'lost', 'succeeded', 'success'), 0) DESC,
                        ended_at_utc DESC, id DESC
           ) AS rank
    FROM activity_events
    WHERE provider_surface = 'kimi_local_runtime' AND source_type = 'local'
      AND activity_kind = 'session_active' AND raw_reference IS NOT NULL
)
DELETE FROM activity_events
WHERE id IN (SELECT id FROM ranked_tasks WHERE rank > 1);

CREATE UNIQUE INDEX IF NOT EXISTS idx_kimi_task_activity_identity
    ON activity_events(provider_id, account_id, raw_reference)
    WHERE provider_surface = 'kimi_local_runtime' AND source_type = 'local'
      AND activity_kind = 'session_active' AND raw_reference IS NOT NULL;

INSERT INTO schema_version (version, description)
VALUES (17, 'Preserve one local activity row per Kimi task');
