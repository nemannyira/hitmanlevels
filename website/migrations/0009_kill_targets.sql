-- the level's own targets to kill (its non-hidden "kill" objectives), from its package as it is uploaded: the
-- level's page greys the Precision badge when there is none (#66); NULL: a version uploaded before (not known)
ALTER TABLE versions ADD COLUMN kill_targets INTEGER;
