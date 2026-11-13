-- what a level's page shows, taken from its package when it is uploaded (website/check: the WebAssembly check)
ALTER TABLE versions ADD COLUMN readme TEXT NOT NULL DEFAULT '';
ALTER TABLE versions ADD COLUMN briefing TEXT NOT NULL DEFAULT '';
ALTER TABLE versions ADD COLUMN objectives TEXT NOT NULL DEFAULT '[]';  -- a JSON array of the laptop's lines
ALTER TABLE versions ADD COLUMN preview INTEGER NOT NULL DEFAULT 0;      -- 1: levels/<id>/<version>/preview.png in R2

-- downloads of a level's packages (every version)
ALTER TABLE levels ADD COLUMN downloads INTEGER NOT NULL DEFAULT 0;
