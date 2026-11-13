-- hitmanlevels.com: accounts, authors' keys, levels and their versions, reports (docs/site_api.md)

-- an account: one sign-in at Discord, GitHub or Google; no password, no e-mail
CREATE TABLE users (
  id INTEGER PRIMARY KEY,
  provider TEXT NOT NULL,          -- discord | github | google
  provider_id TEXT NOT NULL,       -- the account's number there
  name TEXT NOT NULL,              -- shown on the site
  created INTEGER NOT NULL,        -- unix seconds
  blocked INTEGER NOT NULL DEFAULT 0,
  admin INTEGER NOT NULL DEFAULT 0,
  UNIQUE (provider, provider_id)
);

-- a signed-in browser: the cookie's token is kept as its SHA-256 only
CREATE TABLE sessions (
  token_hash TEXT PRIMARY KEY,
  user_id INTEGER NOT NULL REFERENCES users (id),
  expires INTEGER NOT NULL
);

-- an author key (Ed25519, 64 hex): bound to the account that first uploads with it
CREATE TABLE authors (
  key TEXT PRIMARY KEY,
  user_id INTEGER NOT NULL REFERENCES users (id),
  blocked INTEGER NOT NULL DEFAULT 0
);

-- a level (its id from the package's signed manifest) and its versions
CREATE TABLE levels (
  id TEXT PRIMARY KEY,
  game TEXT NOT NULL,              -- h2 (later h1, h3, h4)
  author_key TEXT NOT NULL REFERENCES authors (key),
  taken_down INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE versions (
  level_id TEXT NOT NULL REFERENCES levels (id),
  version INTEGER NOT NULL,
  title TEXT NOT NULL,
  base_scene TEXT NOT NULL,
  license TEXT NOT NULL,
  author_name TEXT NOT NULL,       -- as the manifest names the author
  path TEXT NOT NULL,              -- /levels/<id>/<version>.h2lvl (the R2 object's key without the slash)
  size INTEGER NOT NULL,
  sha256 TEXT NOT NULL,
  uploaded_by INTEGER NOT NULL REFERENCES users (id),
  created INTEGER NOT NULL,
  taken_down INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (level_id, version)
);

CREATE TABLE reports (
  id INTEGER PRIMARY KEY,
  level_id TEXT NOT NULL,
  version INTEGER,
  user_id INTEGER NOT NULL REFERENCES users (id),
  reason TEXT NOT NULL,
  created INTEGER NOT NULL,
  handled INTEGER NOT NULL DEFAULT 0
);

-- the list's serial (grows with every list signed)
CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
INSERT INTO meta (key, value) VALUES ('serial', '0');
