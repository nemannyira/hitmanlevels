-- a player's play history (docs/play_stats.md section 4): their plays sent signed in, one by one, on their page;
-- shown to others only when the player turns it on (off at first)
ALTER TABLE users ADD COLUMN history_public INTEGER NOT NULL DEFAULT 0;
