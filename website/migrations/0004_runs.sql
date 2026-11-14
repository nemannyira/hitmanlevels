-- the plays of listed levels that players chose to share (docs/play_stats.md; POST /api/v1/runs from
-- levelpack stats send): no account, no IP, not when it was played; deleted with the level (privacy.html)
CREATE TABLE runs (
  id TEXT PRIMARY KEY,             -- the run's own random id (32 hex): a run is stored once
  level_id TEXT NOT NULL,
  version INTEGER NOT NULL,
  received INTEGER NOT NULL,       -- unix seconds
  result TEXT NOT NULL,            -- completed | failed | left
  time INTEGER NOT NULL,           -- game seconds
  rating TEXT,                     -- the game's name of it (SilentAssassin ...): completed runs
  stealth INTEGER,
  aggression INTEGER,
  counters TEXT NOT NULL,          -- JSON: the statistics screen's lines
  stage INTEGER,                   -- an escalation's
  alarm_highest INTEGER,
  events TEXT NOT NULL,            -- JSON array: objectives done / lost, spotted, died (game time, place)
  dll TEXT NOT NULL                -- the editor's version
);
CREATE INDEX runs_level ON runs (level_id, version);
