-- Replace legacy Claude paths in place independently of the current source data.
-- SchemaManager records version 13 after committing this script and compacting the database.
UPDATE activity_events
SET raw_reference = jistats_path_reference(substr(raw_reference, 1, instr(raw_reference, '|date=') - 1))
WHERE provider_id = (SELECT id FROM providers WHERE provider_key = 'claude_code')
  AND provider_surface = 'claude_code_local_stats'
  AND surface_key = 'stats-cache'
  AND source_type = 'local'
  AND source_run_id GLOB 'claude_code_local_v1|*'
  AND instr(raw_reference, '|date=') > 1
  AND raw_reference NOT GLOB 'scope:fnv1a64:*';

UPDATE usage_events
SET raw_reference = jistats_path_reference(raw_reference)
WHERE provider_id = (SELECT id FROM providers WHERE provider_key = 'claude_code')
  AND source_type = 'provider_backfill'
  AND source_run_id GLOB 'claude_code_local_v1|*'
  AND is_backfilled = 1
  AND raw_reference <> ''
  AND raw_reference NOT GLOB 'scope:fnv1a64:*';
