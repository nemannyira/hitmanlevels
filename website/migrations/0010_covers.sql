-- a level's own picture its author uploaded on the site (the R2 object's key, covers/<id>/<time>.png|jpg): shown
-- before the package's preview.png and the map's picture
ALTER TABLE levels ADD COLUMN cover TEXT;
