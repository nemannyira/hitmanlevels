-- when a report was dealt with: the daily clean-up deletes it a year later (privacy.html)
ALTER TABLE reports ADD COLUMN handled_at INTEGER;
