-- A whole-second UTC timestamp still bounds range and retention queries.
ALTER TABLE quota_snapshots ADD COLUMN snapshot_subsecond_ns INTEGER;

INSERT INTO schema_version (version, description)
VALUES (15, 'Preserve source precision for dated quota snapshots');
