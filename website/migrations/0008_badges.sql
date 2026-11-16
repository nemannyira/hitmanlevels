-- badges (docs/badges.md): earned by a player from their ranked runs as they arrive (not retroactive), public
CREATE TABLE badges (
  user_id INTEGER NOT NULL,
  badge TEXT NOT NULL,             -- finished, silent_assassin, ...; "played": the player has a ranked run (rarity)
  level_id TEXT NOT NULL DEFAULT '',  -- '' a profile's badge
  stage INTEGER NOT NULL DEFAULT 0,   -- an escalation's stage (0: none)
  run_id TEXT,                     -- the run that gave it
  earned INTEGER NOT NULL,         -- unix seconds
  PRIMARY KEY (user_id, badge, level_id, stage)
);
CREATE INDEX badges_level ON badges (level_id, badge);
