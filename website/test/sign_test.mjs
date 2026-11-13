// The site's list as the Worker signs it (src/sitelist.js), read by the client (levelpack hub list): the
// canonical form and the signature must be what shared/levelfmt/hub.cpp accepts.
//   node website/test/sign_test.mjs <levelpack.exe>
import { execFileSync } from "node:child_process";
import { createPrivateKey, createPublicKey, randomBytes } from "node:crypto";
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { signList, canonical, hex } from "../src/sitelist.js";

const levelpack = process.argv[2];
const seed = randomBytes(32).toString("hex");
const der = Buffer.from("302e020100300506032b657004220420" + seed, "hex");
const pub = createPublicKey(createPrivateKey({ key: der, format: "der", type: "pkcs8" }))
  .export({ format: "der", type: "spki" }).subarray(-32).toString("hex");

const list = {
  format: 1, serial: 7,
  levels: [{ author: { key: "a".repeat(64), name: "Tester Ünő" }, base_scene: "C1-1\C1-1__MAIN", game: "h2",
    id: "cewarg3802v86kr4", license: "CC BY 4.0", path: "/levels/cewarg3802v86kr4/2.h2lvl",
    sha256: "b".repeat(64), size: 8188, title: "Courier \"quoted\"", version: 2 }],
};
let failed = 0;
const check = (name, ok, detail = "") => { console.log(`${ok ? "PASS" : "FAIL"} ${name} ${detail}`); if (!ok) failed++; };

check("canonical: keys sorted, compact", canonical({ b: 1, a: [true, null, "x"] }) === '{"a":[true,null,"x"],"b":1}');
const dir = mkdtempSync(join(tmpdir(), "h2site-"));
try {
  mkdirSync(join(dir, "api", "v1"), { recursive: true });
  const { text, sig } = await signList(list, seed);
  writeFileSync(join(dir, "api", "v1", "index.json"), text);
  writeFileSync(join(dir, "api", "v1", "index.sig"), sig);
  const run = (keys) => {
    try {
      return execFileSync(levelpack, ["hub", "list", "--source", dir], {
        env: { ...process.env, HITMANLEVELS_SITE_KEYS: keys, HITMANLEVELS_HOME: join(dir, "home") }, encoding: "utf8" });
    } catch (e) { return "ERROR " + (e.stdout || "") + (e.stderr || ""); }
  };
  const out = run(pub);
  check("the client takes the Worker's signed list", out.includes("Courier") && !out.startsWith("ERROR"), out.trim().split("\n")[0]);
  const other = randomBytes(32).toString("hex");
  const otherPub = createPublicKey(createPrivateKey({ key: Buffer.from("302e020100300506032b657004220420" + other, "hex"), format: "der", type: "pkcs8" }))
    .export({ format: "der", type: "spki" }).subarray(-32).toString("hex");
  check("... not with another site key", run(otherPub).startsWith("ERROR"));
  writeFileSync(join(dir, "api", "v1", "index.json"), text.replace("Courier", "Kourier"));
  check("... not a changed list", run(pub).startsWith("ERROR"));
} finally {
  rmSync(dir, { recursive: true, force: true });
}
console.log(`\n${failed ? "FAILED" : "all passed"}`);
process.exit(failed ? 1 : 0);
