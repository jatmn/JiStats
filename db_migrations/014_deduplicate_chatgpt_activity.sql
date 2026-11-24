-- Match the reference written by OpenAIExportImport and keep the first saved row.
WITH imported_messages AS (
    SELECT ae.id,
           ROW_NUMBER() OVER (PARTITION BY ae.provider_id, ae.account_id, ae.raw_reference ORDER BY ae.id) AS occurrence
    FROM activity_events ae
    JOIN providers p ON p.id = ae.provider_id
    JOIN accounts a ON a.id = ae.account_id AND a.provider_id = ae.provider_id
    WHERE p.provider_key = 'openai'
      AND a.account_key = 'imported-history:openai_subscription_export_import'
      AND ae.provider_surface = 'openai_subscription_export_import'
      AND ae.source_type = 'imported'
      AND ae.activity_kind = 'message_send'
      AND ae.raw_reference GLOB 'conversation:?*/message:?*'
)
DELETE FROM activity_events
WHERE id IN (SELECT id FROM imported_messages WHERE occurrence > 1);

INSERT INTO schema_version (version, description)
VALUES (14, 'Deduplicate imported ChatGPT activity')
ON CONFLICT(version) DO NOTHING;
