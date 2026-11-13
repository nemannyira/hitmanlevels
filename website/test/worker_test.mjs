// The Worker on wrangler dev (local D1 and R2; .dev.vars with a test SITE_SEED and test client ids):
// the pages, sign-in's start and a forged callback, sessions, the POST rules, uploads closed until the
// package check is built, the owner's take-down publishing a list the client (levelpack) accepts.
//   npx wrangler d1 migrations apply hitmanlevels --local; npx wrangler dev --local --port 8787 --ip 127.0.0.1
//   node test/worker_test.mjs ../build/Release/levelpack.exe
import { execFileSync } from "node:child_process";
import { createHash, createPrivateKey, createPublicKey, randomBytes } from "node:crypto";
import { mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const BASE = "http://127.0.0.1:8787";
const levelpack = process.argv[2];
let failed = 0;
const check = (name, ok, detail = "") => { console.log(`${ok ? "PASS" : "FAIL"} ${name} ${detail}`); if (!ok) failed++; };
const get = (p, headers = {}) => fetch(BASE + p, { headers, redirect: "manual" });
const post = (p, body, headers = {}) => fetch(BASE + p, { method: "POST", body, headers, redirect: "manual" });
const sql = (command) => {
  const f = join(tmpdir(), `h2site-${Date.now()}.sql`);
  writeFileSync(f, command);
  try {
    return execFileSync("npx", ["wrangler", "d1", "execute", "hitmanlevels", "--local", "--yes", "--file", `"${f}"`],
      { encoding: "utf8", shell: true, env: { ...process.env, WRANGLER_SEND_METRICS: "false" } });
  } finally {
    rmSync(f, { force: true });
  }
};
const b64url = (buf) => buf.toString("base64").replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");

check("the front page", (await get("/")).status === 200);
check("Get started", (await get("/get-started")).status === 200);
check("an unknown API path", (await get("/api/nothing")).status === 404);
check("not signed in", (await get("/api/me")).status === 401);
check("a POST from elsewhere is refused", (await post("/api/upload?agree=1", "x", { Origin: "https://evil.example" })).status === 403);
check("a POST without a session", (await post("/api/upload?agree=1", "x", { Origin: BASE })).status === 401);

// sign-in: off to Discord with a state and PKCE; a callback that does not match its state is refused
let r = await get("/api/auth/discord/start");
const loc = new URL(r.headers.get("Location") || "http://x/");
check("Discord's sign-in", r.status === 302 && loc.origin + loc.pathname === "https://discord.com/oauth2/authorize" &&
  loc.searchParams.get("client_id") === "test-discord-id" && loc.searchParams.get("scope") === "identify" &&
  loc.searchParams.get("code_challenge_method") === "S256" &&
  loc.searchParams.get("redirect_uri") === BASE + "/api/auth/discord/callback", loc.toString().slice(0, 90));
const oauth = (r.headers.get("Set-Cookie") || "").match(/__Host-oauth=([^;]+)/);
check("... its state kept in an HttpOnly, Secure cookie", !!oauth && /HttpOnly/.test(r.headers.get("Set-Cookie")) && /Secure/.test(r.headers.get("Set-Cookie")));
check("... a callback with another state is refused",
  (await get("/api/auth/discord/callback?code=c&state=forged", { Cookie: `__Host-oauth=${oauth && oauth[1]}` })).status === 400);
check("GitHub's sign-in has no scope (the public profile)", new URL((await get("/api/auth/github/start")).headers.get("Location")).searchParams.get("scope") === null);
check("Google's asks for openid profile only", new URL((await get("/api/auth/google/start")).headers.get("Location")).searchParams.get("scope") === "openid profile");
check("no such provider", (await get("/api/auth/facebook/start")).status === 404);

// a session as a sign-in would make it (the provider's side cannot run here)
const token = b64url(randomBytes(32));
const hash = b64url(createHash("sha256").update(token).digest());
const pid = "t" + Date.now();
sql(`INSERT INTO users (provider, provider_id, name, created, admin) VALUES ('github', '${pid}', 'Tester', 0, 1);` +
  `INSERT INTO sessions (token_hash, user_id, expires) SELECT '${hash}', id, 4102444800 FROM users WHERE provider_id = '${pid}';`);
const me = { Cookie: `__Host-session=${token}`, Origin: BASE };
r = await get("/api/me", me);
check("signed in", r.status === 200 && (await r.json()).name === "Tester");
r = await post("/api/upload", "x", { ...me, "Content-Length": "1" });
check("an upload needs the agreement", r.status === 400);
r = await post("/api/upload?agree=1", new Uint8Array(16), me);
check("uploads stay closed until the package check is built", r.status === 422, (await r.json()).error);

// the owner takes something down: the list is published, signed; the client accepts it
const level = "wt" + Date.now().toString(36);
const key = randomBytes(32).toString("hex");
sql(`INSERT INTO authors (key, user_id) SELECT '${key}', id FROM users WHERE provider_id = '${pid}';` +
  `INSERT INTO levels (id, game, author_key) VALUES ('${level}', 'h2', '${key}');` +
  `INSERT INTO versions (level_id, version, title, base_scene, license, author_name, path, size, sha256, uploaded_by, created)` +
  ` SELECT '${level}', 1, 'Worker Test', 'C1-1\\C1-1__MAIN', 'CC BY 4.0', 'Tester', '/levels/${level}/1.h2lvl', 10, '${"c".repeat(64)}', id, 0 FROM users WHERE provider_id = '${pid}';`);
r = await post("/api/admin/takedown", JSON.stringify({ level: "nosuchlevel" }), me);
check("the owner's take-down publishes the list", r.status === 200);
const text = await (await get("/api/v1/index.json")).text();
const sig = new Uint8Array(await (await get("/api/v1/index.sig")).arrayBuffer());
check("... the list holds the level", text.includes(`"id":"${level}"`) && sig.length === 64);
const seed = readFileSync(".dev.vars", "utf8").match(/SITE_SEED=([0-9a-f]{64})/)[1];
const pub = createPublicKey(createPrivateKey({ key: Buffer.from("302e020100300506032b657004220420" + seed, "hex"), format: "der", type: "pkcs8" }))
  .export({ format: "der", type: "spki" }).subarray(-32).toString("hex");
const dir = mkdtempSync(join(tmpdir(), "h2site-"));
try {
  mkdirSync(join(dir, "api", "v1"), { recursive: true });
  writeFileSync(join(dir, "api", "v1", "index.json"), text);
  writeFileSync(join(dir, "api", "v1", "index.sig"), sig);
  let out;
  try {
    out = execFileSync(levelpack, ["hub", "list", "--source", dir],
      { env: { ...process.env, HITMANLEVELS_SITE_KEYS: pub, HITMANLEVELS_HOME: join(dir, "home") }, encoding: "utf8" });
  } catch (e) { out = "ERROR " + e.stdout + e.stderr; }
  check("... the client accepts it (signed by the site key)", out.includes("Worker Test") && !out.startsWith("ERROR"), out.split("\n")[0]);
} finally {
  rmSync(dir, { recursive: true, force: true });
}
r = await post("/api/admin/takedown", JSON.stringify({ level }), me);
check("a level taken down leaves the list", !(await (await get("/api/v1/index.json")).text()).includes(level));

r = await post("/api/logout", "", me);
check("signed out", r.status === 204 && (await get("/api/me", me)).status === 401);
sql(`UPDATE users SET admin = 0 WHERE provider_id = '${pid}';`);
console.log(`\n${failed ? failed + " FAILED" : "all passed"}`);
process.exit(failed ? 1 : 0);
