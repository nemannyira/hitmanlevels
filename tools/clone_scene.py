r"""A map of the game under a new name: Scenes\<new>\<new>__MAIN.ZIP (+ its
sound files), the ZIP's entries renamed (SCENES/<old>/... -> SCENES/<new>/...).

The engine loads a scene by that path (h2.dev_loadscene([[Custom1\Custom1__MAIN]]),
the editor's levels by base_scene): a new map name that starts as a copy of a
map, its own levels and save progress apart from the original's. Checked:
C1-1 as Custom1 loads with its characters, laptop and texts.

  python clone_scene.py C1-1 Custom1            (game folder from GAME below)
  python clone_scene.py --remove Custom1
"""
import os
import shutil
import sys
import zipfile

GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin"


def clone(old, new):
    src_dir = os.path.join(GAME, "Scenes", old)
    dst_dir = os.path.join(GAME, "Scenes", new)
    os.makedirs(dst_dir, exist_ok=True)
    src = zipfile.ZipFile(os.path.join(src_dir, old + "__MAIN.ZIP"))
    with zipfile.ZipFile(os.path.join(dst_dir, new + "__MAIN.ZIP"), "w", zipfile.ZIP_DEFLATED) as dst:
        for info in src.infolist():
            dst.writestr(info.filename.replace("/" + old + "/", "/" + new + "/").replace(old + "__MAIN", new + "__MAIN"),
                         src.read(info.filename))
    for ext in ("WAV", "WHD"):
        p = os.path.join(src_dir, old + "__MAIN." + ext)
        if os.path.exists(p):
            shutil.copy(p, os.path.join(dst_dir, new + "__MAIN." + ext))
    print(os.path.join("Scenes", new, new + "__MAIN.ZIP"), "made from", old)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--remove":
        shutil.rmtree(os.path.join(GAME, "Scenes", sys.argv[2]))
    elif len(sys.argv) == 3:
        clone(sys.argv[1], sys.argv[2])
    else:
        print(__doc__)
