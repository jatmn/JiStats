-- ============================================
-- MIGRATION 002: Add Taskbar Overlay Preference
-- ============================================

-- Insert default taskbar overlay preference (enabled for current validation work)
INSERT OR IGNORE INTO settings (key, value) VALUES
    ('ui.taskbar_overlay_enabled', 'true');

INSERT OR IGNORE INTO schema_version (version, description)
VALUES (2, 'Add taskbar overlay preference');
