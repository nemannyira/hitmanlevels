"""Drive a running game through the mod's developer commands
(H2Fix.ini [Debug] DevCommands=1).

  python dev.py "return h2.scene()"        run Lua, print what it returns
  python dev.py --shot name [--scale 0.5]  in-game screenshot -> dumps/shots/name.png
  python dev.py --start                    launch the game through Steam
  python dev.py --kill                     stop the game
  python dev.py --wait-scene C1-1__MAIN    wait until the scene is loaded
"""
import os
import subprocess
import sys
import time

GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin"
CMD = os.path.join(GAME, "Mods", "_cmd.lua")
OUT = os.path.join(GAME, "Mods", "_cmd.out")
HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = os.path.join(HERE, "..", "dumps", "shots")


def _retry(fn, tries=40):
    """fn() again while the game has the file open (it writes _cmd.out as we look)"""
    for i in range(tries):
        try:
            return fn()
        except PermissionError:
            if i == tries - 1:
                raise
            time.sleep(0.05)


def _read():
    with open(OUT, encoding="utf-8", errors="replace") as f:
        return f.read().rstrip()


def run(code, timeout=15.0):
    if os.path.exists(OUT):
        _retry(lambda: os.remove(OUT))
    with open(CMD + ".tmp", "w", encoding="utf-8") as f:
        f.write(code)
    _retry(lambda: os.replace(CMD + ".tmp", CMD))
    t = time.time()
    while time.time() - t < timeout:
        if os.path.exists(OUT):
            time.sleep(0.05)
            return _retry(_read)
        time.sleep(0.1)
    if os.path.exists(CMD):
        _retry(lambda: os.remove(CMD))
    return "(timeout: game not answering)"


def shot(name, scale=0.5):
    from PIL import Image
    bmp = os.path.join(GAME, "Screens", name + ".bmp")
    if os.path.exists(bmp):
        os.remove(bmp)
    run('h2.dev_shot("%s")' % name)
    t = time.time()
    while time.time() - t < 10 and not os.path.exists(bmp):
        time.sleep(0.1)
    time.sleep(0.3)
    if not os.path.exists(bmp):
        return None
    os.makedirs(SHOTS, exist_ok=True)
    img = Image.open(bmp).convert("RGB")
    if scale != 1:
        img = img.resize((int(img.width * scale), int(img.height * scale)), Image.LANCZOS)
    png = os.path.abspath(os.path.join(SHOTS, name + ".png"))
    img.save(png)
    os.remove(bmp)
    return png


def wait_ready(scene=None, timeout=180.0):
    """after h2.dev_loadscene / h2.dev_loadgame: until the scene came in and the editor has put the level
    on it (h2.dev_ready) -> True; False when the game went down or it took longer. `scene`: a part of
    its name it must have ("C1-1__MAIN")."""
    t = time.time()
    while time.time() - t < timeout:
        out = run("return h2.dev_ready()", 5)
        if out.endswith("|ready") and (scene is None or scene.lower() in out.lower()):
            return True
        if "timeout" in out and not running():
            return False
        time.sleep(0.25)
    return False


def running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq hitman2.exe"], capture_output=True, text=True).stdout
    return "hitman2.exe" in out


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return
    if args[0] == "--start":
        os.startfile("steam://run/6850")
        t = time.time()
        while time.time() - t < 60:
            if running() and "timeout" not in run("return h2.scene()", 2):
                print("game up:", run("return h2.scene()"))
                return
            time.sleep(1)
        print("game did not answer")
    elif args[0] == "--kill":
        subprocess.run(["taskkill", "/F", "/IM", "hitman2.exe"], capture_output=True)
        print("killed")
    elif args[0] == "--shot":
        scale = float(args[args.index("--scale") + 1]) if "--scale" in args else 0.5
        print(shot(args[1], scale))
    elif args[0] == "--wait-scene":
        t = time.time()
        timeout = float(args[2]) if len(args) > 2 else 120
        while time.time() - t < timeout:
            s = run("return h2.scene()", 3)
            if args[1].lower() in s.lower():
                print(s)
                return
            time.sleep(1)
        print("scene not reached:", run("return h2.scene()", 3))
    else:
        print(run(" ".join(args)))


if __name__ == "__main__":
    main()
