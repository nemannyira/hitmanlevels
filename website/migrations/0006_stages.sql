-- an escalation's stages (docs/escalation.md), taken from its package when it is uploaded: a JSON array of
-- { name, text } in order ([] for a level without stages); the level's page shows them, its numbers and boards
-- are per stage (runs.stage)
ALTER TABLE versions ADD COLUMN stages TEXT NOT NULL DEFAULT '[]';
