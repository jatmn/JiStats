-- NULL marks legacy rollups whose boundary evidence is unknown.
ALTER TABLE quota_snapshot_rollups_daily ADD COLUMN observed_billing_windows TEXT;

INSERT INTO schema_version (version, description)
VALUES (18, 'Preserve observed billing windows in daily quota rollups');
