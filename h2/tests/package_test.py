"""Level packages (.h2lvl, shared/levelfmt/package.h) and their installer (build/Release/levelpack.exe)
without the game: export, install, what this PC knows of authors and versions, and hostile packages
(other names, compression, bombs, changed files, signatures, layouts). Python's own Ed25519
(`cryptography`) builds packages too, so the two implementations check each other. Nothing outside a
temporary folder is touched (HITMANLEVELS_HOME and a stand-in game folder). Build first.

  python h2/tests/package_test.py
"""
import hashlib
import io
import json
import re
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
EXE = os.environ.get("LEVELPACK") or os.path.join(ROOT, "build", "Release", "levelpack.exe")
EXAMPLE = os.path.join(ROOT, "h2", "examples", "missions", "Courier")
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
ENV = dict(os.environ, HITMANLEVELS_HOME=HOME)


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
    m = {"format": 1, "game": "h2", "id": "pytestlevel01", "version": 1, "title": "T", "base_scene": "C1-1\\C1-1__MAIN",
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
    ("a game this editor does not know", py_package(k, game="h9")),
    ("no game named", py_package(k, game="")),
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

# ---- issue #7: invisible characters in names and titles ----
def pack_env(env, *args, stdin=None):
    p = subprocess.run([EXE] + list(args), capture_output=True, text=True, encoding="utf-8", env=env, input=stdin)
    return p.returncode, p.stdout + p.stderr


for label, name in [("a right-to-left override", "Bob\u202eevil"), ("a zero-width space", "Bo\u200bb"),
                    ("a BOM", "\ufeffBob"), ("a C1 control", "Bob\u0085")]:
    code, out = pack("inspect", write("names.h2lvl", py_package(k, author={"name": name, "key": raw_key(k).hex()})))
    check("refused: an author name with " + label, code == 1 and "author's name" in out, out.strip()[:100])
code, out = pack("inspect", write("names.h2lvl", py_package(k, level=level_doc(name="T\u2066x"), title="T\u2066x")))
check("refused: a title with an isolate", code == 1, out.strip()[:100])
code, out = pack("inspect", write("names.h2lvl", py_package(k, author={"name": "Ádám Éva ÿ", "key": raw_key(k).hex()})))
check("accepted: accented letters in a name", code == 0, out.strip()[-80:])
# (not through the command line: a right-to-left override in a process's arguments is a malware trick,
# and the antivirus took the tool for one; the editor's name field goes through the same ValidManifestText)
code, out = pack("inspect", PKG2)
check("the installer shows the fingerprint before the name", re.search(r"Author:\s+\[[0-9A-F-]{19}\]\s+Tester", out), out)


# ---- issue #8: preview.png as the picture only; the Windows user name ----
def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def png(*extra):
    ihdr = struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0)
    idat = zlib.compress(b"\x00" + b"\xff\x00\x00" * 2 + b"\x00" + b"\x00\xff\x00" * 2)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr) + b"".join(extra) + png_chunk(b"IDAT", idat) +
            png_chunk(b"IEND", b""))


secret = png(png_chunk(b"tEXt", b"Author\x00someone at C:\\Users\\someone"), png_chunk(b"eXIf", b"MM\x00*junk"),
             png_chunk(b"tIME", b"\x07\xea\x0a\x18\x0c\x00\x00"))
PREV = os.path.join(TMP, "Preview")
shutil.copytree(EXAMPLE, PREV)
with open(os.path.join(PREV, "preview.png"), "wb") as f:
    f.write(secret)
with open(os.path.join(PREV, "main.lua"), "a", encoding="utf-8") as f:
    f.write("\n-- made by pkgtester on a rainy day\n")
code, out = pack_env(dict(ENV, USERNAME="pkgtester"), "export", PREV, os.path.join(TMP, "preview.h2lvl"))
check("export with a preview carrying text / EXIF / time", code == 0, out.strip()[-120:])
check("  a note: the Windows user name in a file", "Note:" in out and "pkgtester" in out, out.strip()[-120:])
if code == 0:
    sent = zipfile.ZipFile(os.path.join(TMP, "preview.h2lvl")).read("preview.png")
    check("  the packaged preview has no text, EXIF or time chunk",
          b"tEXt" not in sent and b"eXIf" not in sent and b"tIME" not in sent and b"someone" not in sent and
          sent.startswith(b"\x89PNG") and b"IDAT" in sent, len(sent))
    check("  ... and the picture's own chunks as they were", sent == png())
code, out = pack("inspect", write("prev.h2lvl", py_package(k, extra_files=[("preview.png", secret)])))
check("refused: a package whose preview carries text chunks", code == 1 and "preview.png" in out, out.strip()[:90])
code, out = pack("inspect", write("prev.h2lvl", py_package(k, extra_files=[("preview.png", png())])))
check("accepted: a clean preview", code == 0, out.strip()[-80:])
bad_crc = bytearray(png())
bad_crc[-5] ^= 1
code, out = pack("inspect", write("prev.h2lvl", py_package(k, extra_files=[("preview.png", bytes(bad_crc))])))
check("refused: a preview with a wrong chunk CRC", code == 1, out.strip()[:90])

# ---- issue #4: the author key backed up and restored ----
BACKUP = os.path.join(TMP, "me.h2key")
code, out = pack_env(ENV, "author", "backup", BACKUP, stdin="short\n")
check("backup: a short password refused", code == 1 and not os.path.exists(BACKUP), out.strip()[:90])
code, out = pack_env(ENV, "author", "backup", BACKUP, stdin="correct horse battery\n")
check("backup written", code == 0 and os.path.exists(BACKUP), out.strip()[:90])
blob = open(BACKUP, "rb").read() if os.path.exists(BACKUP) else b""
seed_hex = None
check("  the backup holds neither the name nor the public key in clear",
      b"Tester" not in blob and bytes.fromhex(key_hex) not in blob and key_hex.encode() not in blob, len(blob))
HOME2 = os.path.join(TMP, "home2")
os.makedirs(HOME2)
ENV2 = dict(ENV, HITMANLEVELS_HOME=HOME2)
code, out = pack_env(ENV2, "author", "restore", BACKUP, stdin="wrong password!\n")
check("restore: a wrong password refused, nothing written", code == 1 and not os.listdir(HOME2), out.strip()[:90])
tampered = bytearray(blob)
tampered[-3] ^= 0x40
code, out = pack_env(ENV2, "author", "restore", write("tampered.h2key", bytes(tampered)), stdin="correct horse battery\n")
check("restore: a changed backup refused", code == 1, out.strip()[:90])
header_changed = bytearray(blob)
header_changed[19] ^= 1  # (the Argon2 blocks: part of the authenticated header)
code, out = pack_env(ENV2, "author", "restore", write("hdr.h2key", bytes(header_changed)), stdin="correct horse battery\n")
check("restore: a changed header refused", code == 1, out.strip()[:90])
code, out = pack_env(ENV2, "author", "restore", BACKUP, stdin="correct horse battery\n")
restored = json.load(open(os.path.join(HOME2, "author.json"))) if code == 0 else {}
check("restored on 'another PC': the same key and name", code == 0 and restored.get("key") == key_hex and
      restored.get("name") == "Tester", out.strip()[:90])
L2 = os.path.join(TMP, "Courier2")
shutil.copytree(EXAMPLE, L2)
code, out = pack_env(ENV2, "export", L2, os.path.join(TMP, "from2.h2lvl"))
check("  a package signed there carries the same fingerprint",
      code == 0 and json.loads(zipfile.ZipFile(os.path.join(TMP, "from2.h2lvl")).read("manifest.json"))["author"]["key"]
      == key_hex, out.strip()[-80:])
HOME3 = os.path.join(TMP, "home3")
ENV3 = dict(ENV, HITMANLEVELS_HOME=HOME3)
pack_env(ENV3, "author", "Someone Else")
other_key = json.load(open(os.path.join(HOME3, "author.json")))["key"]
code, out = pack_env(ENV3, "author", "restore", BACKUP, stdin="correct horse battery\n")
check("restore over another key: refused without --replace", code == 1 and "another author key" in out and
      json.load(open(os.path.join(HOME3, "author.json")))["key"] == other_key, out.strip()[:120])
code, out = pack_env(ENV3, "author", "restore", BACKUP, "--replace", stdin="correct horse battery\n")
kept = [f for f in os.listdir(HOME3) if f.startswith("author.key.old-")]
check("  with --replace: the backup's key, the other kept beside", code == 0 and kept and
      json.load(open(os.path.join(HOME3, "author.json")))["key"] == key_hex, (out.strip()[:80], os.listdir(HOME3)))

# ---- issue #6: the Hub (the site's signed list, a local folder as the site) ----
SITE = os.path.join(TMP, "site")
SITE_KEY = os.path.join(TMP, "site.key")
HOMEH = os.path.join(TMP, "home_hub")
os.makedirs(HOMEH)
code, out = pack_env(ENV, "site", "keygen", SITE_KEY)
m = re.search(r"Public key: ([0-9a-f]{64})", out)
check("site keygen", code == 0 and m, out.strip()[:100])
site_pub = m.group(1) if m else ""
ENVH = dict(ENV, HITMANLEVELS_HOME=HOMEH, HITMANLEVELS_SITE_KEYS=site_pub)


def put_site(packages, serial, tweak=None):
    """the site's folder: the packages and a signed index.json -> the list as written"""
    shutil.rmtree(SITE, ignore_errors=True)
    entries = []
    for data in packages:
        man = json.loads(zipfile.ZipFile(io.BytesIO(data)).read("manifest.json"))
        path = "/levels/%s/%d.h2lvl" % (man["id"], man["version"])
        os.makedirs(os.path.dirname(os.path.join(SITE, path[1:])), exist_ok=True)
        with open(os.path.join(SITE, path[1:]), "wb") as f:
            f.write(data)
        entries.append({"game": man["game"], "id": man["id"], "version": man["version"], "title": man["title"],
                        "author": man["author"], "base_scene": man["base_scene"], "license": man["license"],
                        "path": path, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    index = {"format": 1, "serial": serial, "levels": entries}
    if tweak:
        tweak(index)
    os.makedirs(os.path.join(SITE, "api", "v1"), exist_ok=True)
    ipath = os.path.join(SITE, "api", "v1", "index.json")
    with open(ipath, "wb") as f:
        f.write(canonical(index))
    return pack_env(ENV, "site", "sign", ipath, SITE_KEY)


hub_pkgs = [open(PKG2, "rb").read(), py_package(k)]
code, out = put_site(hub_pkgs, 5)
check("site sign", code == 0 and os.path.exists(os.path.join(SITE, "api", "v1", "index.sig")), out.strip()[:90])
code, out = pack_env(ENVH, "hub", "list", "--source", SITE)
check("hub list: the signed list", code == 0 and "2 levels (list 5)" in out and LEVEL_ID in out, out.strip()[:160])
GAMEH = os.path.join(TMP, "game_hub")
os.makedirs(GAMEH)
open(os.path.join(GAMEH, "hitman2.exe"), "wb").close()
code, out = pack_env(ENVH, "hub", "install", LEVEL_ID, "--source", SITE, "--game", GAMEH, "--yes")
check("hub install: downloaded, checked, installed", code == 0 and
      os.path.exists(os.path.join(GAMEH, "Mods", "Online", LEVEL_ID, "level.json")), out.strip()[-120:])
code, out = pack_env(dict(ENVH, HITMANLEVELS_SITE_KEYS=""), "hub", "list", "--source", SITE)
check("no site key: the Hub is off", code == 1 and "no site key" in out, out.strip()[:90])
code, out = pack_env(dict(ENVH, HITMANLEVELS_SITE_KEYS=raw_key(Ed25519PrivateKey.generate()).hex()), "hub", "list",
                     "--source", SITE)
check("refused: a list signed by another key", code == 1 and "not signed by the site" in out, out.strip()[:100])
ipath = os.path.join(SITE, "api", "v1", "index.json")
with open(ipath, "rb") as f:
    signed = f.read()
with open(ipath, "wb") as f:
    f.write(signed.replace(b'"title":"Courier"', b'"title":"Courieq"'))
code, out = pack_env(ENVH, "hub", "list", "--source", SITE)
check("refused: a list changed after signing", code == 1 and "not signed by the site" in out, out.strip()[:100])
put_site(hub_pkgs, 6)
with open(os.path.join(SITE, "levels", LEVEL_ID, "2.h2lvl"), "ab") as f:
    f.write(b"x")
code, out = pack_env(ENVH, "hub", "install", LEVEL_ID, "--source", SITE, "--game", GAMEH, "--yes")
check("refused: a package not the list's (size / SHA-256)", code == 1 and "SHA-256" in out, out.strip()[:100])
put_site(hub_pkgs, 4)
code, out = pack_env(ENVH, "hub", "list", "--source", SITE)
check("refused: an older list than one seen (rollback)", code == 1 and "older level list" in out, out.strip()[:100])
put_site(hub_pkgs, 7, lambda ix: ix["levels"][0].update(path="/../../evil.h2lvl"))
code, out = pack_env(ENVH, "hub", "install", LEVEL_ID, "--source", SITE, "--game", GAMEH, "--yes")
check("refused: a path out of the site", code == 1 and "not a path of the site" in out, out.strip()[:100])
put_site(hub_pkgs, 8, lambda ix: ix["levels"][0].update(author={"name": "Tester", "key": raw_key(k).hex()}))
code, out = pack_env(ENVH, "hub", "install", LEVEL_ID, "--source", SITE, "--game", GAMEH, "--yes")
check("refused: a package whose author is not the list's", code == 1 and "not the list's" in out, out.strip()[:100])
code, out = pack_env(ENVH, "hub", "list", "--source", "http://hitmanlevels.com")
check("refused: plain HTTP", code == 1 and "https:// only" in out, out.strip()[:90])
code, out = pack_env(ENVH, "hub", "list", "--source", "https://hitmanlevels.com/x?y")
check("refused: a site given with a path", code == 1 and "https://host only" in out, out.strip()[:90])
code, out = pack_env(ENVH, "hub", "list", "--source", "https://hitmanlevels.com")
check("the real site over HTTPS: an answer, not a crash (no list there yet)", code in (0, 1) and out.strip(),
      out.strip()[:120])

shutil.rmtree(TMP, ignore_errors=True)
print("\n%d passed, %d failed" % (results.count(True), results.count(False)))
sys.exit(0 if all(results) else 1)
