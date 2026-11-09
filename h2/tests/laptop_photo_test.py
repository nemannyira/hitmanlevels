"""A laptop photo of the level's own (mission.files) is drawn exactly where the map's own photo is. On C1-1
(one photo place, its Target Photo): the map's photo opened in the laptop (B, Up x3, Enter) and its frame
measured on a screenshot (what is not the black around it); then a magenta picture of the same shape
(512 x 391) given as the level's photo1.png, opened the same way, its frame measured (the magenta); the two
frames must agree within 2 % of the screen. Uses Mods/_PhotoFrame (removed); a running game with dev commands.

  python h2/tests/laptop_photo_test.py
"""
import json
import os
import shutil
import struct
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_PhotoFrame"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
DIK_ESC, DIK_ENTER, DIK_B, DIK_UP = 0x01, 0x1C, 0x30, 0xC8
results = []


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def press(label, settle=0.8):
    lua('h2.dev_press(%s, 1) return 1' % json.dumps(label))
    status = "looking"
    t = time.time()
    while time.time() - t < 5 and status == "looking":
        time.sleep(0.2)
        status = lua("return h2.dev_press_status()")
    time.sleep(settle)
    return status


def key(dik):
    lua("h2.dev_key(%d) return 1" % dik)
    time.sleep(0.7)


def png(w, h, rgb):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    rows = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def backs():
    with open(os.path.join(dev.GAME, "H2Fix.log"), encoding="latin1", errors="replace") as f:
        return f.read().count("test: back in the editor")


def photo_shot(name):
    """the level played from the start, the laptop's photo opened, a screenshot; back in the editor"""
    lua('h2.dev_editor(true) h2.dev_mode("simple") return 1')
    time.sleep(1)
    press("5 Try it")
    check("%s: Play from the start" % name, press("Play from the start").startswith("pressed"))
    time.sleep(5)
    dev.wait_ready("C1-1__MAIN", 150)
    time.sleep(3)
    key(DIK_B)
    for _ in range(3):
        key(DIK_UP)  # (from Continue to the first file)
    key(DIK_ENTER)
    time.sleep(1)
    lua('h2.dev_shot("laptop_%s") return 1' % name)
    time.sleep(1)
    key(DIK_ESC)
    key(DIK_ESC)
    before = backs()
    lua("return h2.dev_wmkey(0x77)")  # (F8)
    for _ in range(90):
        time.sleep(1)
        if backs() > before:
            break
    dev.wait_ready("C1-1__MAIN", 60)
    return os.path.join(dev.GAME, "Screens", "laptop_%s.bmp" % name)


def frame(path, inside):
    """the box of the pixels `inside` accepts, as fractions of the screen (left, top, right, bottom); the top 6 %
    left out (the test's hint line, the game's cursor in the corner)"""
    from PIL import Image  # noqa: PLC0415
    im = Image.open(path).convert("RGB")
    w, h = im.size
    small = im.resize((w // 4, h // 4))
    px = small.load()
    xs, ys = [], []
    for y in range(int(small.size[1] * 0.06), small.size[1]):
        for x in range(small.size[0]):
            if inside(px[x, y]):
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return (min(xs) / small.size[0], min(ys) / small.size[1], (max(xs) + 1) / small.size[0], (max(ys) + 1) / small.size[1])


if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
try:
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')
    dev.wait_ready("C1-1__MAIN", 150)
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    lua('return h2.dev_level("save")')

    # 1. the map's own photo: what is not the black around it (the editor's hint line at the top left out)
    own = frame(photo_shot("own"), lambda p: max(p) > 40)
    check("the map's photo shown", own is not None, str(own))

    # 2. a magenta picture of the photo's shape as the level's photo
    with open(os.path.join(FOLDER, "photo1.png"), "wb") as f:
        f.write(png(512, 391, (255, 0, 255)))
    with open(os.path.join(FOLDER, "level.json"), encoding="utf-8") as f:
        doc = json.load(f)
    doc.setdefault("mission", {})["files"] = [{"name": "Target Photo", "image": "photo1.png"}]
    with open(os.path.join(FOLDER, "level.json"), "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    lua('return h2.dev_level("load", "%s")' % LEVEL)
    mine = frame(photo_shot("mine"), lambda p: p[0] > 200 and p[1] < 70 and p[2] > 200)
    check("the level's photo shown", mine is not None, str(mine))

    if own and mine:
        off = max(abs(a - b) for a, b in zip(own, mine))
        check("its frame is the map's photo's (within 2 % of the screen)", off <= 0.02,
              "map's %s, the level's %s" % (["%.3f" % v for v in own], ["%.3f" % v for v in mine]))
finally:
    try:
        lua('h2.dev_mode("advanced") h2.dev_editor(false) return h2.dev_level("revert")')
    except RuntimeError:
        pass
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
sys.exit(0 if all(results) else 1)
