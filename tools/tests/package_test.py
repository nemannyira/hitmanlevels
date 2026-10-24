"""Level packages (.h2lvl, src/levelfmt/package.h) and their installer (build/Release/h2level_pack.exe)
without the game: export, install, what this PC knows of authors and versions, and hostile packages
(other names, compression, bombs, changed files, signatures, layouts). Python's own Ed25519
(`cryptography`) builds packages too, so the two implementations check each other. Nothing outside a
temporary folder is touched (H2LEVEL_HOME and a stand-in game folder). Build first.

  python tools/tests/package_test.py
"""
import hashlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
from cryptography.hazmat.primitives import serialization

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
EXE = os.path.join(ROOT, "build", "Release", "h2level_pack.exe")
EXAMPLE = os.path.join(ROOT, "examples", "missions", "Courier")
PREFIX = b"H2LVL manifest v1\n"
L = 2 ** 252 + 27742317777372353535851937790883648493

results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, str(detail)[:160]))


TMP = tempfile.mkdtemp(prefix="h2pkg_")
HOME = os.path.join(TMP, "home")
GAME = os.path.join(TMP, "game")
ONLINE = os.path.join(GAME, "Mods", "Online")
os.makedirs(HOME)
os.makedirs(GAME)
open(os.path.join(GAME, "hitman2.exe"), "wb").close()
ENV = dict(os.environ, H2LEVEL_HOME=HOME)


def pack(*args):
    p = subprocess.run([EXE] + list(args), capture_output=True, text=True, encoding="utf-8", env=ENV)
    return p.returncode, p.stdout + p.stderr


def install(path, *flags):
    return pack("install", path, "--game", GAME, *flags)


def online_entries():
    return sorted(os.listdir(ONLINE)) if os.path.isdir(ONLINE) else []


# ---- a package made in Python: the format as package.h describes it ----
def raw_key(priv):
    return priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def canonical(obj):
    return json.dumps(obj, separators=(",", ":"), sort_keys=True, ensure_ascii=False).encode("utf-8")


def manifest_of(files, priv, **fields):
    m = {"format": 1, "id": "pytestlevel01", "version": 1, "title": "T", "base_scene": "C1-1\\C1-1__MAIN",
         "author": {"name": "Py", "key": raw_key(priv).hex()}, "license": "CC BY 4.0",
         "files": [{"name": n, "size": len(d), "sha256": hashlib.sha256(d).hexdigest()} for n, d in files]}
    m.update(fields)
    return m


def level_doc(**extra):
    doc = {"format": "h2level", "version": 1, "name": "T", "base_scene": "C1-1\\C1-1__MAIN", "objects": [],
           "spawned": [], "npcs": [], "lists": [], "scripts": ["main.lua"],
           "package": {"id": "pytestlevel01", "version": 1}}
    doc.update(extra)
    return doc


def zip_bytes(entries, method=zipfile.ZIP_STORED):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", method) as z:
        for name, data in entries:
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = method
            z.writestr(info, data)
    return buf.getvalue()


def py_package(priv, level=None, scripts=None, extra_files=(), manifest_text=None, sig=None, method=zipfile.ZIP_STORED,
               tamper=None, **mfields):
    level = level if level is not None else level_doc()
    files = [("level.json", json.dumps(level).encode())]
    for name, data in (scripts if scripts is not None else [("main.lua", b"print('hi')\n")]):
        files.append(("scripts/" + name, data))
    files += list(extra_files)
    m = manifest_of(files, priv, **mfields)
    text = manifest_text if manifest_text is not None else canonical(m)
    signature = sig if sig is not None else priv.sign(PREFIX + text)
    if tamper:
        files = tamper(files)
    return zip_bytes([("manifest.json", text), ("manifest.sig", signature)] + files, method)


def write(name, data):
    path = os.path.join(TMP, name)
    with open(path, "wb") as f:
        f.write(data)
    return path


if not os.path.exists(EXE):
    print("FAIL build first: %s" % EXE)
    sys.exit(1)

# ---- the signature code itself ----
code, out = pack("selftest")
check("Ed25519: the RFC 8032 vectors", code == 0, out.strip())

# ---- the author ----
code, out = pack("author")
check("no author before one is made", code == 1, out.strip())
code, out = pack("author", "Tester", "CC0 1.0")
check("the author key made", code == 0 and "Tester" in out and os.path.exists(os.path.join(HOME, "author.key")), out)
key_hex = json.load(open(os.path.join(HOME, "author.json")))["key"]
sealed = open(os.path.join(HOME, "author.key"), "rb").read()
check("the secret seed is not stored as it is (DPAPI)", len(sealed) > 100 and bytes.fromhex(key_hex) not in sealed)
code, out = pack("author", "Tester 2")
check("a second call keeps the key", json.load(open(os.path.join(HOME, "author.json")))["key"] == key_hex, out)
pack("author", "Tester", "CC0 1.0")

# ---- export ----
LEVEL = os.path.join(TMP, "Courier")
shutil.copytree(EXAMPLE, LEVEL)
PKG1 = os.path.join(TMP, "courier_v1.h2lvl")
code, out = pack("export", LEVEL, PKG1)
lj = json.load(open(os.path.join(LEVEL, "level.json")))
check("export", code == 0 and os.path.exists(PKG1), out.strip())
check("the level stamped: an id and version 1", lj.get("package", {}).get("version") == 1 and
      len(lj["package"].get("id", "")) == 16, lj.get("package"))
LEVEL_ID = lj["package"]["id"]

z = zipfile.ZipFile(PKG1)
names = z.namelist()
check("Python's zipfile reads it: stored files, manifest first", names[:3] == ["manifest.json", "manifest.sig", "level.json"]
      and all(i.compress_type == zipfile.ZIP_STORED for i in z.infolist()), names)
mtext, msig = z.read("manifest.json"), z.read("manifest.sig")
manifest = json.loads(mtext)
check("the manifest is canonical JSON", canonical(manifest) == mtext)
try:
    Ed25519PublicKey.from_public_bytes(bytes.fromhex(manifest["author"]["key"])).verify(msig, PREFIX + mtext)
    verified = True
except Exception as ex:  # noqa: BLE001
    verified = ex
check("Python's Ed25519 verifies the editor's signature", verified is True, verified)
check("the hashes are the files'", all(hashlib.sha256(z.read(f["name"])).hexdigest() == f["sha256"]
                                       for f in manifest["files"]))
check("the author's key and license in it", manifest["author"]["key"] == key_hex and manifest["license"] == "CC0 1.0")
z.close()

# ---- install ----
code, out = install(PKG1)
check("a new author: not installed without --yes (exit 3)", code == 3 and not online_entries(), out.splitlines()[-1:])
code, out = pack("inspect", PKG1)
check("inspect shows the author and fingerprint", code == 0 and "Tester" in out and "an author not seen" in out, out)
code, out = install(PKG1, "--yes")
target = os.path.join(ONLINE, LEVEL_ID)
check("installed with --yes", code == 0 and os.path.isdir(target), out.splitlines()[-1:])
files = sorted(os.listdir(target)) if os.path.isdir(target) else []
check("its files: level.json, the scripts, package.json", files == sorted(["level.json", "main.lua", "package.json"] +
      (["README.md"] if os.path.exists(os.path.join(LEVEL, "README.md")) else [])), files)
check("the files are read-only", all(not os.access(os.path.join(target, f), os.W_OK) for f in files))
check("nothing else in Mods\\Online", online_entries() == [LEVEL_ID], online_entries())
known = json.load(open(os.path.join(HOME, "known.json")))
check("known.json: the author and the level's version", key_hex in known["authors"] and
      known["levels"][LEVEL_ID]["installed"] == 1, known)
code, out = install(PKG1)
check("the same version again: installed already", code == 0 and "installed already" in out, out.splitlines()[-1:])

PKG2 = os.path.join(TMP, "courier_v2.h2lvl")
code, out = pack("export", LEVEL, PKG2)
code, out = install(PKG2)
check("version 2 from a known author installs without asking", code == 0 and
      json.load(open(os.path.join(target, "level.json")))["package"]["version"] == 2, out.splitlines()[-1:])
check("the old version's folder is gone", online_entries() == [LEVEL_ID], online_entries())
code, out = install(PKG1)
check("an older version needs --allow-older (exit 3)", code == 3, out.splitlines()[-2:])
code, out = install(PKG1, "--allow-older")
check("an older version with --allow-older", code == 0 and
      json.load(open(os.path.join(target, "level.json")))["package"]["version"] == 1, out.splitlines()[-1:])
known = json.load(open(os.path.join(HOME, "known.json")))
check("the highest version seen is kept", known["levels"][LEVEL_ID]["highest"] == 2 and
      known["levels"][LEVEL_ID]["installed"] == 1, known["levels"][LEVEL_ID])
code, out = install(PKG2)
check("back to version 2 (not 'installed already')", code == 0 and
      json.load(open(os.path.join(target, "level.json")))["package"]["version"] == 2, out.splitlines()[-1:])

# ---- another author ----
other = Ed25519PrivateKey.generate()
lv = level_doc(package={"id": LEVEL_ID, "version": 9})
fake = write("fake.h2lvl", py_package(other, level=lv, id=LEVEL_ID, version=9))
code, out = install(fake, "--yes")
check("another author's package of an installed id needs --allow-other-author", code == 3 and "ANOTHER" in out,
      out.splitlines()[-3:])
check("the level is not replaced", json.load(open(os.path.join(target, "level.json")))["package"]["version"] == 2)

# ---- Python's packages: the installer accepts the other implementation's signatures ----
py = Ed25519PrivateKey.generate()
good = write("py_good.h2lvl", py_package(py))
code, out = install(good, "--yes")
check("a package built and signed by Python installs", code == 0 and os.path.isdir(os.path.join(ONLINE, "pytestlevel01")),
      out.splitlines()[-1:])
for i in range(20):  # more keys and messages
    k = Ed25519PrivateKey.generate()
    data = py_package(k, level=level_doc(name="Level %d" % i), title="Level %d" % i)
    code, out = pack("inspect", write("py_%d.h2lvl" % i, data))
    if code != 0:
        break
check("20 more Python-signed packages verify", code == 0, out.strip()[-120:])

# ---- hostile packages: each refused, nothing written ----
before = online_entries()
k = Ed25519PrivateKey.generate()


def resign(entries_fn):
    """a package whose files are changed and whose manifest is signed again over them"""
    files = entries_fn([("level.json", json.dumps(level_doc()).encode()), ("scripts/main.lua", b"print(1)\n")])
    text = canonical(manifest_of([f for f in files if f[0] not in ("manifest.json", "manifest.sig")], k))
    return zip_bytes([("manifest.json", text), ("manifest.sig", k.sign(PREFIX + text))] + files)


def stored_zip_raw(entries, prepend=b"", append=b"", comment=b""):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as zf:
        zf.comment = comment
        for name, data in entries:
            zf.writestr(zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0)), data)
    return prepend + buf.getvalue() + append


good_bytes = py_package(k)
gz = zipfile.ZipFile(io.BytesIO(good_bytes))
good_entries = [(n, gz.read(n)) for n in gz.namelist()]
mt = gz.read("manifest.json")
mobj = json.loads(mt)
sig = gz.read("manifest.sig")
s_int = int.from_bytes(sig[32:], "little")
malleable = sig[:32] + (s_int + L).to_bytes(32, "little")

bomb = zip_bytes([("manifest.json", mt), ("manifest.sig", sig), ("level.json", b" " * (50 << 20))],
                 zipfile.ZIP_DEFLATED)

HOSTILE = [
    ("empty file", b""),
    ("random bytes", os.urandom(5000)),
    ("truncated", good_bytes[: len(good_bytes) // 2]),
    ("compressed (deflate)", py_package(k, method=zipfile.ZIP_DEFLATED)),
    ("a zip bomb (50 MB of deflate)", bomb),
    ("larger than 2 MB", py_package(k, extra_files=[("README.md", b"x" * (3 << 20))])),
    ("zip slip ../", resign(lambda f: f + [("../evil.lua", b"x")])),
    ("zip slip scripts/../", resign(lambda f: f + [("scripts/../x.lua", b"x")])),
    ("an absolute name", resign(lambda f: f + [("C:/Windows/x.lua", b"x")])),
    ("a backslash name", resign(lambda f: f + [("scripts\\main.lua", b"x")])),
    ("a device name", resign(lambda f: f + [("scripts/con.lua", b"x")])),
    ("an alternate data stream", resign(lambda f: f + [("level.json:evil", b"x")])),
    ("an exe", resign(lambda f: f + [("evil.exe", b"MZ")])),
    ("a name twice (case)", resign(lambda f: f + [("scripts/MAIN.lua", b"x")])),
    ("a file changed after signing", py_package(k, tamper=lambda f: [(n, d + b" " if n == "level.json" else d) for n, d in f])),
    ("a script changed after signing", py_package(k, tamper=lambda f: [(n, b"os.execute('x')" if n.endswith(".lua") else d) for n, d in f])),
    ("a signature with a flipped bit", py_package(k, sig=bytes([sig[0] ^ 1]) + sig[1:])),
    ("a signature of another key", py_package(k, sig=Ed25519PrivateKey.generate().sign(PREFIX + mt), manifest_text=mt)),
    ("S + L (a second form of the signature)", zip_bytes([("manifest.json", mt), ("manifest.sig", malleable)] + good_entries[2:])),
    ("the title changed, the signature kept", zip_bytes([("manifest.json", mt.replace(b'"title":"T"', b'"title":"X"')),
                                                         ("manifest.sig", sig)] + good_entries[2:])),
    ("a manifest not canonical (signed)", py_package(k, manifest_text=json.dumps(manifest_of(
        [("level.json", json.dumps(level_doc()).encode()), ("scripts/main.lua", b"print('hi')\n")], k), indent=1).encode())),
    ("a manifest with a key twice (signed)", py_package(k, manifest_text=canonical(mobj)[:-1] + b',"id":"other0000"}')),
    ("a file not in the manifest", zip_bytes(good_entries + [("README.md", b"hello")])),
    ("a manifest file missing", zip_bytes(good_entries[:-1])),
    ("an id with ..", py_package(k, id="../../x")),
    ("level.json refused strictly (signed)", py_package(k, level=level_doc(autoexec="x"))),
    ("level.json of another id", py_package(k, level=level_doc(package={"id": "otherlevel01", "version": 1}))),
    ("a script level.json does not list", py_package(k, scripts=[("main.lua", b"x"), ("extra.lua", b"x")])),
    ("a script not UTF-8", py_package(k, scripts=[("main.lua", b"\xff\xfe\x00")])),
    ("a preview not a PNG", py_package(k, extra_files=[("preview.png", b"GIF89a")])),
    ("README with control characters", py_package(k, extra_files=[("README.md", b"a\x1b[2Jb")])),
    ("data before the zip", stored_zip_raw(good_entries, prepend=b"MZ" + b"\0" * 100)),
    ("data after the zip", good_bytes + b"PK\x05\x06junk"),
    ("a zip comment", stored_zip_raw(good_entries, comment=b"hello")),
    ("an author name of 100 letters", py_package(k, author={"name": "x" * 100, "key": raw_key(k).hex()})),
    ("format 2", py_package(k, format=2)),
]
for name, data in HOSTILE:
    path = write("hostile.h2lvl", data)
    code, out = install(path, "--yes", "--allow-older", "--allow-other-author")
    check("refused: " + name, code == 1 and "REFUSED" in out, (out.strip().splitlines() or [""])[0])
check("nothing written by the refused ones", online_entries() == before, online_entries())

# ---- export refuses a path of this PC ----
PRIV = os.path.join(TMP, "Private")
shutil.copytree(EXAMPLE, PRIV)
with open(os.path.join(PRIV, "main.lua"), "a", encoding="utf-8") as f:
    f.write('\n-- dofile("C:\\\\Users\\\\someone\\\\Desktop\\\\x.lua")\n')
before_text = open(os.path.join(PRIV, "level.json"), encoding="utf-8").read()
code, out = pack("export", PRIV, os.path.join(TMP, "private.h2lvl"))
check("export refuses a script holding C:\\Users\\...", code == 1 and "path of this PC" in out, out.strip())
check("the level's version is not used up then", open(os.path.join(PRIV, "level.json"), encoding="utf-8").read() == before_text)

shutil.rmtree(TMP, ignore_errors=True)
print("\n%d passed, %d failed" % (results.count(True), results.count(False)))
sys.exit(0 if all(results) else 1)
