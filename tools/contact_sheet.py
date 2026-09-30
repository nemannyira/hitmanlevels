"""Tile the screenshots of a run_game.ps1 capture into one labelled image.

Usage: python contact_sheet.py <dir> [columns=8]
"""
import glob, os, sys

from PIL import Image, ImageDraw

d = sys.argv[1]
cols = int(sys.argv[2]) if len(sys.argv) > 2 else 8
files = sorted(glob.glob(os.path.join(d, "*s.png")))
w, h = 200, 150
sheet = Image.new("RGB", (w * cols, h * ((len(files) + cols - 1) // cols)))
for i, f in enumerate(files):
    im = Image.open(f).convert("RGB")
    ImageDraw.Draw(im).text((3, 3), os.path.basename(f)[:-4], fill=(255, 40, 40))
    sheet.paste(im, ((i % cols) * w, (i // cols) * h))
sheet.save(os.path.join(d, "sheet.png"))
print(os.path.join(d, "sheet.png"), len(files))
