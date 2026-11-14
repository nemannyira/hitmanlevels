-- level 2 of the play statistics (docs/play_stats.md section 4, #54): levelpack signed in to an account, run
-- tickets, the boards and a player's page

-- a sign-in of levelpack (a device code): the player types the short code on the site's device page
CREATE TABLE device_codes (
  device_hash TEXT PRIMARY KEY,    -- SHA-256 of the code levelpack keeps (it asks for its token with it)
  user_code TEXT NOT NULL UNIQUE,  -- the code typed on the site (8 letters)
  expires INTEGER NOT NULL,
  user_id INTEGER                  -- set when the player approves it
);

-- levelpack's tokens (in the player's Windows Credential Manager), kept as their SHA-256 only
CREATE TABLE client_tokens (
  token_hash TEXT PRIMARY KEY,
  user_id INTEGER NOT NULL REFERENCES users (id),
  created INTEGER NOT NULL
);

-- a ticket levelpack takes when a run starts: the site's time then
CREATE TABLE tickets (
  id TEXT PRIMARY KEY,             -- 32 hex
  user_id INTEGER NOT NULL,
  level_id TEXT NOT NULL,
  version INTEGER NOT NULL,
  issued INTEGER NOT NULL,         -- unix seconds
  used INTEGER NOT NULL DEFAULT 0
);

ALTER TABLE runs ADD COLUMN user_id INTEGER;                       -- sent signed in: the player's
ALTER TABLE runs ADD COLUMN ranked INTEGER NOT NULL DEFAULT 0;     -- its ticket allows its time: on the boards
ALTER TABLE runs ADD COLUMN video TEXT;                            -- a video of it the player linked
ALTER TABLE runs ADD COLUMN verified INTEGER NOT NULL DEFAULT 0;   -- the owner watched it
CREATE INDEX runs_user ON runs (user_id);
