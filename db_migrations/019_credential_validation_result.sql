-- Older profiles can have a check time without a recorded outcome.
ALTER TABLE credential_profiles ADD COLUMN validation_state TEXT NOT NULL DEFAULT 'unknown'
    CHECK (validation_state = 'unknown' OR
           (validation_state IN ('valid', 'invalid', 'unreachable') AND last_validated_at_utc IS NOT NULL));
ALTER TABLE credential_profiles ADD COLUMN last_validation_error TEXT NOT NULL DEFAULT '';

INSERT INTO schema_version (version, description)
VALUES (19, 'Preserve completed credential validation results');
