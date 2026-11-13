// The Worker on wrangler dev (local D1 and R2; .dev.vars with a test SITE_SEED and test client ids):
// the pages, sign-in's start and a forged callback, sessions, the POST rules, real uploads (packages the
// editor's levelpack made: checked by the WebAssembly check, listed; tampered, repeated, another account's
// refused), the owner's take-down publishing a list the client (levelpack) accepts.
//   npx wrangler d1 migrations apply hitmanlevels --local
//   npx wrangler dev --local --port 8787 --ip 127.0.0.1 --test-scheduled   (the daily clean-up run by hand)
//   node test/worker_test.mjs ../build/Release/levelpack.exe
import { execFileSync } from "node:child_process";
import { createHash, createPrivateKey, createPublicKey, randomBytes } from "node:crypto";
import { cpSync, mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
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
check("not a package: refused by the check", r.status === 422, (await r.json()).error);

// real packages: an author key of a test home, the example level Courier exported twice (versions 1, 2)
const work = mkdtempSync(join(tmpdir(), "h2pkg-"));
const pack = (...args) => execFileSync(levelpack, args, { env: { ...process.env, HITMANLEVELS_HOME: join(work, "home") }, encoding: "utf8" });
const pid2 = "u" + Date.now();
const token2 = b64url(randomBytes(32));
sql(`INSERT INTO users (provider, provider_id, name, created) VALUES ('discord', '${pid2}', 'Someone ${pid2}', 0);` +
  `INSERT INTO sessions (token_hash, user_id, expires) SELECT '${b64url(createHash("sha256").update(token2).digest())}', id, 4102444800 FROM users WHERE provider_id = '${pid2}';`);
const other = { Cookie: `__Host-session=${token2}`, Origin: BASE };
try {
  pack("author", "Worker Tester", "CC BY 4.0");
  cpSync(join("..", "h2", "examples", "missions", "Courier"), join(work, "Courier"), { recursive: true });
  pack("export", join(work, "Courier"), join(work, "v1.h2lvl"));
  pack("export", join(work, "Courier"), join(work, "v2.h2lvl"));
  const v1 = readFileSync(join(work, "v1.h2lvl")), v2 = readFileSync(join(work, "v2.h2lvl"));
  const id = JSON.parse(readFileSync(join(work, "Courier", "level.json"), "utf8")).package.id;

  r = await post("/api/upload?agree=1", v1, me);
  let body = await r.json();
  check("a package the editor made: checked, listed", r.status === 200 && body.id === id && body.version === 1, JSON.stringify(body).slice(0, 120));
  check("... in the list", (await (await get("/api/v1/index.json")).text()).includes(`"id":"${id}"`));
  r = await get(`/levels/${id}/1.h2lvl`);
  check("... its file served as uploaded", r.status === 200 && Buffer.from(await r.arrayBuffer()).equals(v1));
  check("the same version again is refused", (await post("/api/upload?agree=1", v1, me)).status === 409);

  const tampered = Buffer.from(v1);
  const at = tampered.indexOf(Buffer.from('"title"'));  // (a byte of a signed file changed)
  tampered[at + 1] ^= 0x20;
  r = await post("/api/upload?agree=1", tampered, me);
  check("a package changed after it was signed is refused", r.status === 422, (await r.json()).error);

  r = await post("/api/upload?agree=1", v2, other);
  check("another account cannot upload with the author's key", r.status === 403, (await r.json()).error);
  r = await post("/api/upload?agree=1", v2, me);
  check("the author's next version", r.status === 200 && (await r.json()).version === 2);
  check("... the list names version 2", (await (await get("/api/v1/index.json")).text()).includes(`"path":"/levels/${id}/2.h2lvl"`));

  // the site's own pages' data: the list, the level's page (its README from the package), the downloads
  const readme = readFileSync(join(work, "Courier", "README.md"), "utf8");
  const listed = (await (await get("/api/levels")).json()).levels.find((l) => l.id === id);
  check("the pages' list: the level at version 2, its blurb from its README", listed && listed.version === 2 &&
    listed.blurb.length > 0 && readme.replace(/\s+/g, " ").includes(listed.blurb.replace(/\.\.\.$/, "").slice(0, 40)),
    listed && listed.blurb.slice(0, 60));
  const page = await (await get(`/api/levels/${id}`)).json();
  check("the level's page: its README, both versions, the author's key", page.readme === readme &&
    page.versions.length === 2 && /^[0-9a-f]{64}$/.test(page.author.key) && page.path === `/levels/${id}/2.h2lvl`);
  check("... a downloaded package counted", page.downloads >= 1, String(page.downloads));
  check("no page for a level not listed", (await get("/api/levels/nosuchlevel1")).status === 404);
  await post("/api/admin/takedown", JSON.stringify({ level: id }), me);
} finally {
  rmSync(work, { recursive: true, force: true });
}

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

// the owner's page: the overview, a report dealt with, the daily clean-up, a level put back, an account
// blocked, unblocked and deleted with everything of it
check("the overview is the owner's only", (await get("/api/admin/overview", other)).status === 403);
check("a report", (await post("/api/report", JSON.stringify({ level, version: 1, reason: "test report" }), other)).status === 200);
let ov = await (await get("/api/admin/overview", me)).json();
const rep = ov.reports.find((x) => x.level_id === level && x.reason === "test report");
check("... in the owner's overview, open", rep && !rep.handled);
await post("/api/admin/handled", JSON.stringify({ report: rep.id }), me);
sql(`INSERT INTO reports (level_id, user_id, reason, created, handled, handled_at)
     SELECT '${level}', id, 'an old one', 0, 1, 1000 FROM users WHERE provider_id = '${pid}';`);
await get("/__scheduled?cron=17+3+*+*+*");
ov = await (await get("/api/admin/overview", me)).json();
check("... dealt with; the clean-up deletes reports dealt with over a year ago only",
  ov.reports.some((x) => x.id === rep.id && x.handled) && !ov.reports.some((x) => x.reason === "an old one"));
r = await post("/api/admin/restore", JSON.stringify({ level }), me);
check("a level put back is in the list again", r.status === 200 && (await (await get("/api/v1/index.json")).text()).includes(level));
await post("/api/admin/takedown", JSON.stringify({ level }), me);
const otherId = ov.users.find((u) => u.name === `Someone ${pid2}`).id;
await post("/api/admin/block", JSON.stringify({ user: otherId }), me);
check("a blocked account is signed out", (await get("/api/me", other)).status === 401);
await post("/api/admin/unblock", JSON.stringify({ user: otherId }), me);
check("the owner cannot block themself", (await post("/api/admin/block", JSON.stringify({ user: ov.users.find((u) => u.admin).id }), me)).status === 400);
r = await post("/api/admin/delete-user", JSON.stringify({ user: otherId }), me);
ov = await (await get("/api/admin/overview", me)).json();
check("an account deleted with its reports", r.status === 200 && !ov.users.some((u) => u.id === otherId) &&
  !ov.reports.some((x) => x.reporter === `Someone ${pid2}`));

r = await post("/api/logout", "", me);
check("signed out", r.status === 204 && (await get("/api/me", me)).status === 401);
sql(`UPDATE users SET admin = 0 WHERE provider_id = '${pid}';`);
console.log(`\n${failed ? failed + " FAILED" : "all passed"}`);
process.exit(failed ? 1 : 0);
