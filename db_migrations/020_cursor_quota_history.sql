-- Before Cursor required complete cycle bounds, monthly was also the fallback.
-- Use strftime for system SQLite 3.34, which does not have unixepoch.
CREATE TEMP TABLE cursor_quota_history_repair AS
SELECT qs.id AS snapshot_id, qs.provider_id, qs.account_id, qs.plan_id,
       substr(qs.snapshot_time_utc, 1, 10) || 'T00:00:00Z' AS rollup_day
FROM quota_snapshots qs
JOIN providers p ON p.id = qs.provider_id
JOIN plans pl ON pl.id = qs.plan_id AND pl.account_id = qs.account_id
WHERE p.provider_key = 'cursor'
  AND pl.plan_key = 'cursor_subscription_api:monthly'
  AND qs.quota_dimension = 'percent'
  AND (COALESCE(CAST(strftime('%s', qs.window_start_utc) AS INTEGER), 0) <= 0
       OR COALESCE(CAST(strftime('%s', qs.window_end_utc) AS INTEGER), 0)
          <= CAST(strftime('%s', qs.window_start_utc) AS INTEGER));

INSERT INTO plans (account_id, plan_key, plan_type, display_name, quota_window, limit_value)
SELECT DISTINCT pl.account_id, 'cursor_subscription_api:subscription', pl.plan_type,
                pl.display_name, 'subscription', pl.limit_value
FROM temp.cursor_quota_history_repair repair
JOIN plans pl ON pl.id = repair.plan_id
WHERE true
ON CONFLICT(account_id, plan_key) DO NOTHING;

-- Either bucket can contain a mixture after the move. Analysis reads retained
-- raw observations for these days, and normal refreshes rebuild today's cache.
DELETE FROM quota_snapshot_rollups_daily
WHERE (provider_id, account_id, plan_bucket_id, rollup_period_start_utc, quota_dimension) IN (
    SELECT provider_id, account_id, plan_id, rollup_day, 'percent'
    FROM temp.cursor_quota_history_repair
    UNION
    SELECT repair.provider_id, repair.account_id, pl.id, repair.rollup_day, 'percent'
    FROM temp.cursor_quota_history_repair repair
    JOIN plans pl ON pl.account_id = repair.account_id AND pl.plan_key = 'cursor_subscription_api:subscription'
);

UPDATE quota_snapshots
SET plan_id = pl.id
FROM temp.cursor_quota_history_repair repair
JOIN plans pl ON pl.account_id = repair.account_id AND pl.plan_key = 'cursor_subscription_api:subscription'
WHERE quota_snapshots.id = repair.snapshot_id;

DROP TABLE temp.cursor_quota_history_repair;

INSERT INTO schema_version (version, description)
VALUES (20, 'Correct incomplete Cursor cycle history');
