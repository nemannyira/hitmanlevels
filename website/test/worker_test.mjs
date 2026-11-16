// The Worker on wrangler dev (local D1 and R2; .dev.vars with a test SITE_SEED and test client ids):
// the pages, sign-in's start and a forged callback, sessions, the POST rules, real uploads (packages the
// editor's levelpack made: checked by the WebAssembly check, listed; tampered, repeated, another account's
// refused), the plays players share (a run levelpack sent: stored, refused, counted on the level's page), the
// owner's take-down publishing a list the client (levelpack) accepts.
//   npx wrangler d1 migrations apply hitmanlevels --local
//   npx wrangler dev --local --port 8787 --ip 127.0.0.1 --test-scheduled   (the daily clean-up run by hand)
//   node test/worker_test.mjs ../build/Release/levelpack.exe
import { execFileSync, spawn } from "node:child_process";
import { createHash, createPrivateKey, createPublicKey, randomBytes } from "node:crypto";
import { cpSync, existsSync, mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
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
check("not for search engines: noindex on pages and the API, robots.txt", (await get("/")).headers.get("X-Robots-Tag") === "noindex, nofollow" &&
  (await get("/api/levels")).headers.get("X-Robots-Tag") === "noindex, nofollow" && (await (await get("/robots.txt")).text()).includes("Disallow: /"));
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
sql(`INSERT INTO users (provider, provider_id, name, created, admin) VALUES ('github', '${pid}', 'Tester ${pid}', 0, 1);` +
  `INSERT INTO sessions (token_hash, user_id, expires) SELECT '${hash}', id, 4102444800 FROM users WHERE provider_id = '${pid}';`);
const me = { Cookie: `__Host-session=${token}`, Origin: BASE };
r = await get("/api/me", me);
check("signed in", r.status === 200 && (await r.json()).name === `Tester ${pid}`);
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

  // the plays players share (POST /api/v1/runs, #52): a run as levelpack sends it (version 2 installed by it, a run
  // written as the editor writes it, levelpack stats send into a folder), then posted here
  const game = join(work, "game"), site = join(work, "site"), runsDir = join(game, "Mods", "Online", id, "runs");
  mkdirSync(join(site, "api", "v1"), { recursive: true });
  mkdirSync(game);
  writeFileSync(join(game, "hitman2.exe"), "");
  pack("install", join(work, "v2.h2lvl"), "--game", game, "--yes");
  mkdirSync(runsDir);
  writeFileSync(join(runsDir, "20261114T100000Z.json"), JSON.stringify({
    format: 1, run: randomBytes(16).toString("hex"), level: id, version: 2, dll: "0.1.0", started: "2026-11-14T10:00:00Z",
    result: "completed", time: 250, rating: "SilentAssassin", stealth: 100, aggression: 1, aids: [], dev: [],
    counters: { alerts: 0, close_encounters: 0, enemies_harmed: 0, enemies_killed: 0, headshots: 0, innocents_harmed: 0,
      innocents_killed: 0, saves: 0, shots: 2 },
    events: [{ kind: "spotted", t: 100, at: [100, -5, 2000] }, { kind: "objective_done", slot: 1, t: 200 }] }));
  pack("stats", "on");
  pack("stats", "send", "--source", site, "--game", game);
  const sent = readFileSync(join(site, "api", "v1", "runs"), "utf8").trim();
  const runPost = (body) => post("/api/v1/runs", body, { "Content-Type": "application/json" });
  const another = (change) => { const x = JSON.parse(sent); change(x); x.run = randomBytes(16).toString("hex"); return JSON.stringify(x); };
  r = await runPost(sent);
  check("a run as levelpack sends it: stored (no account, no Origin)", r.status === 201, `${r.status} ${sent.slice(0, 60)}`);
  check("... the same run again: 409", (await runPost(sent)).status === 409);
  r = await runPost(another((x) => { x.package_sha256 = "0".repeat(64); }));
  check("refused: another package than the listed one", r.status === 422, (await r.json()).error);
  check("refused: played with testing aids", (await runPost(another((x) => { x.aids = ["god mode"]; }))).status === 400);
  check("refused: counters out of range", (await runPost(another((x) => { x.counters.shots = -1; }))).status === 400);
  check("refused: a rating on a failed run", (await runPost(another((x) => { x.result = "failed"; }))).status === 400);
  check("refused: not JSON", (await runPost("{")).status === 400);
  const unrated = (result, time) => another((x) => { x.result = result; x.time = time; delete x.rating; delete x.stealth; delete x.aggression; });
  check("a failed and a left run", (await runPost(unrated("failed", 90))).status === 201 &&
    (await runPost(unrated("left", 30))).status === 201);
  const plays = (await (await get(`/api/levels/${id}`)).json()).plays;
  check("the level's page: 3 plays, 1 finished, 1 Silent Assassin, its median time", plays.plays === 3 &&
    plays.finished === 1 && plays.silent_assassin === 1 && plays.median_time === 250 &&
    plays.ratings.length === 1 && plays.ratings[0].rating === "SilentAssassin", JSON.stringify(plays));
  const inList = (await (await get("/api/levels")).json()).levels.find((l) => l.id === id);
  check("... the list: its plays and finished", inList.plays === 3 && inList.finished === 1);
  // the author's page (#53): the level's runs for its author only, the file the editor reads
  r = await get(`/api/author/levels/${id}/plays`, me);
  const mine = r.status === 200 ? await r.json() : {};
  check("the author's plays: the 3 runs with their places", mine.level === id && mine.runs.length === 3 &&
    mine.runs.some((x) => x.events.some((e) => e.kind === "spotted" && e.at && e.at[2] === 2000)), JSON.stringify(mine).slice(0, 100));
  check("... another account's: not found", (await get(`/api/author/levels/${id}/plays`, other)).status === 404);
  check("... signed out: sign in first", (await get(`/api/author/levels/${id}/plays`)).status === 401);
  r = await get(`/api/author/levels/${id}/plays?download=1`, me);
  check("... as a file to download (plays.json), not cached", /attachment; filename="plays.json"/.test(r.headers.get("Content-Disposition")) &&
    /no-store/.test(r.headers.get("Cache-Control")));
  // level 2 (#54): levelpack signed in (a device code approved on the site), a run's ticket, the boards, a video the
  // owner verifies, the player's page, the name shown, signing out
  check("a device code nobody approved: pending, then an unknown one refused",
    (await post("/api/v1/device/token", JSON.stringify({ device_code: "x".repeat(43) }))).status === 400);
  const signin = spawn(levelpack, ["signin", "--source", BASE], { env: { ...process.env, HITMANLEVELS_HOME: join(work, "home") } });
  let signinOut = "";
  signin.stdout.on("data", (d) => { signinOut += d; });
  const signedIn = new Promise((res) => signin.on("exit", res));
  for (let i = 0; i < 50 && !/[A-Z]{4}-[A-Z]{4}/.test(signinOut); i++) await new Promise((res) => setTimeout(res, 100));
  const userCode = (signinOut.match(/[A-Z]{4}-[A-Z]{4}/) || [""])[0];
  check("levelpack signin shows a code and the device page", userCode && signinOut.includes(BASE + "/device.html"), signinOut.slice(0, 80));
  check("... a wrong code is refused on the site", (await post("/api/device/approve", JSON.stringify({ code: "BBBB-BBBB" }), me)).status === 404);
  check("... the code approved by the signed-in account", (await post("/api/device/approve", JSON.stringify({ code: userCode }), me)).status === 200);
  const signinCode = await signedIn;
  check("... levelpack signed in (its token in the Credential Manager)", signinCode === 0 && signinOut.includes(`Signed in as Tester ${pid}`), signinOut.slice(-80));
  const packAt = (...args) => pack(...args, "--source", BASE, "--game", game);
  check("stats status names the account", packAt("stats", "status").includes(`Signed in: Tester ${pid}`));
  const ranked = (fileName, change) => {
    const x = JSON.parse(sent);
    for (const k of ["game", "package_sha256", "ticket"]) delete x[k];
    x.run = randomBytes(16).toString("hex");
    change(x);
    packAt("stats", "ticket", id, "2", fileName);
    writeFileSync(join(runsDir, fileName), JSON.stringify(x));
    return x.run;
  };
  const quick = ranked("20261114T110000Z.json", () => {});
  check("a ticket taken for a run", readFileSync(join(runsDir, "20261114T110000Z.ticket"), "utf8").includes('"ticket"'));
  packAt("stats", "send");
  check("... sent with it and its ticket (to runs\\sent)", existsSync(join(runsDir, "sent", "20261114T110000Z.ticket")));
  const board1 = (await (await get(`/api/levels/${id}`)).json()).boards;
  check("a run longer than the time since its ticket: not on the boards", !board1.unverified.length, JSON.stringify(board1));
  const fair = ranked("20261114T110100Z.json", (x) => { x.time = 240; });
  sql(`UPDATE tickets SET issued = issued - 1000 WHERE id = '${JSON.parse(readFileSync(join(runsDir, "20261114T110100Z.ticket"), "utf8")).ticket}';`);
  packAt("stats", "send");
  let lp = await (await get(`/api/levels/${id}`)).json();
  check("... one the site saw take its time: on the unverified board", lp.boards.unverified.length === 1 &&
    lp.boards.unverified[0].name === `Tester ${pid}` && lp.boards.unverified[0].time === 240 && !lp.boards.verified.length, JSON.stringify(lp.boards));
  const meId = (await (await get("/api/me", me)).json()).id;
  const pl = await (await get(`/api/players/${meId}`)).json();
  check("the player's page: the level, its plays, the best time", pl.levels.some((l) => l.id === id && l.best === 240 && l.plays >= 2), JSON.stringify(pl.levels));
  // badges (docs/badges.md): the ranked run's (Silent Assassin in 240 s, spotted, no saves, two shots), the profile's
  const lb = pl.badges.levels.find((l) => l.id === id);
  const got = lb ? lb.badges.map((b) => b.badge).sort().join(",") : "";
  check("badges: the ranked run's (not the unranked one's)",
    got === "board_top3,clean_hands,finished,first_to_finish,no_saves,silent_assassin" && lb.mastery === 3, got + " mastery " + (lb && lb.mastery));
  const prof = pl.badges.profile.map((b) => b.badge).sort().join(",");
  check("... the profile's: the upload, a level finished, first on its board", prof === "board_holder,finished_1,first_level", prof);
  const lvb = (await (await get(`/api/levels/${id}`)).json()).badges;
  check("... the level's page: its players and theirs", lvb.all.played === 1 && lvb.all.finished === 1 && !lvb.all.ghost, JSON.stringify(lvb));
  check("... none it did not earn (two shots: not No Shots Fired; spotted: not Ghost)",
    !pl.badges.levels.some((l) => l.badges.some((b) => b.badge === "no_shots" || b.badge === "ghost")));
  // the play history: the player's own always, others' only when the player made it public (off at first)
  check("the play history: not public at first", pl.history_public === false &&
    (await get(`/api/players/${meId}/history`)).status === 403 && (await get(`/api/players/${meId}/history`, other)).status === 403);
  let hist = await (await get(`/api/players/${meId}/history`, me)).json();
  check("... the player's own: each play sent signed in, newest first, and the totals",
    hist.public === false && hist.runs.length === hist.totals.plays && hist.totals.plays >= 2 && hist.totals.levels >= 1 &&
    hist.runs.every((x) => x.level_id && ["completed", "failed", "left"].includes(x.result) && x.received) &&
    hist.runs.every((x, i) => !i || hist.runs[i - 1].received >= x.received) && hist.totals.finished >= 2 &&
    hist.totals.time >= hist.runs.reduce((s, x) => s + x.time, 0), JSON.stringify(hist.totals));
  check("... the setting refused without a true / false", (await post("/api/me/history", JSON.stringify({ public: "yes" }), me)).status === 400);
  check("... made public", (await (await post("/api/me/history", JSON.stringify({ public: true }), me)).json()).public === true);
  hist = await (await get(`/api/players/${meId}/history`)).json();
  check("... then anyone sees it", hist.public === true && hist.totals.plays >= 2 &&
    (await (await get(`/api/players/${meId}`)).json()).history_public === true);
  await post("/api/me/history", JSON.stringify({ public: false }), me);
  check("... and hidden again", (await get(`/api/players/${meId}/history`)).status === 403);
  check("... signed out: the setting needs an account", (await post("/api/me/history", JSON.stringify({ public: true }), { Origin: BASE })).status === 401);
  const myRuns = (await (await get("/api/me/runs", me)).json()).runs;
  check("the player's runs on the boards (for a video)", myRuns.length === 1 && myRuns[0].id === fair && quick !== fair);
  check("a video that is not https:// refused", (await post("/api/me/video", JSON.stringify({ run: fair, url: "javascript:alert(1)" }), me)).status === 400);
  check("another account cannot link a video to it", (await post("/api/me/video", JSON.stringify({ run: fair, url: "https://example.com/v" }), other)).status === 404);
  check("a video linked", (await post("/api/me/video", JSON.stringify({ run: fair, url: "https://example.com/v" }), me)).status === 200);
  const vids = (await (await get("/api/admin/overview", me)).json()).videos;
  check("... in the owner's overview to watch", vids.some((v) => v.id === fair && !v.verified));
  await post("/api/admin/verify", JSON.stringify({ run: fair }), me);
  lp = await (await get(`/api/levels/${id}`)).json();
  check("... verified: on the verified board", lp.boards.verified.length === 1 && lp.boards.verified[0].time === 240);
  check("... and the Verified Runner badge", (await (await get(`/api/players/${meId}`)).json()).badges.profile.some((b) => b.badge === "verified_runner"));
  r = await post("/api/me/name", JSON.stringify({ name: "Agent ‮47" }), me);
  check("the name shown changed (direction marks taken out)", (await r.json()).name === "Agent 47" &&
    (await (await get(`/api/levels/${id}`)).json()).boards.verified[0].name === "Agent 47");
  await post("/api/me/name", JSON.stringify({ name: `Tester ${pid}` }), me);
  pack("signout", "--source", BASE);
  check("levelpack signout: not signed in any more", packAt("stats", "status").includes("Signed in: no"));

  await post("/api/admin/takedown", JSON.stringify({ level: id }), me);
  check("refused: a run of a level taken down", (await runPost(another(() => {}))).status === 422);

  // an escalation (its stages read on upload; the numbers and boards per stage): the example escalation exported
  cpSync(join("..", "h2", "examples", "missions", "TheAccountantEscalation"), join(work, "Esc"), { recursive: true });
  pack("export", join(work, "Esc"), join(work, "esc.h2lvl"));
  const esc = readFileSync(join(work, "esc.h2lvl"));
  const escId = JSON.parse(readFileSync(join(work, "Esc", "level.json"), "utf8")).package.id;
  r = await post("/api/upload?agree=1", esc, me);
  check("an escalation uploaded", r.status === 200, JSON.stringify(await r.json()).slice(0, 80));
  const escSha = createHash("sha256").update(esc).digest("hex");
  const stageRun = (stage, result, time) => runPost(JSON.stringify({
    format: 1, game: "h2", level: escId, version: 1, package_sha256: escSha, run: randomBytes(16).toString("hex"),
    dll: "0.1.0", result, time, stage, aids: [], dev: [], events: [],
    counters: { alerts: 0, close_encounters: 0, enemies_harmed: 0, enemies_killed: 0, headshots: 0, innocents_harmed: 0,
      innocents_killed: 0, saves: 0, shots: 0 } }));
  await stageRun(1, "completed", 100);
  await stageRun(1, "failed", 50);
  await stageRun(2, "left", 30);
  const escRun = randomBytes(16).toString("hex");  // (a ranked Silent Assassin run of stage 2, as level 2 stores one)
  sql(`INSERT INTO runs (id, level_id, version, received, result, time, rating, stealth, aggression, counters, stage, events, dll,
       user_id, ranked) SELECT '${escRun}', '${escId}', 1, 0, 'completed', 222, 'SilentAssassin', 100, 1, '{}', 2, '[]', '0.1.0', id, 1
       FROM users WHERE provider_id = '${pid}';`);
  const escPage = await (await get(`/api/levels/${escId}`)).json();
  check("its page: the stages, by name", escPage.stages.length === 3 && escPage.stages[0].name === "The Books" &&
    escPage.stages[2].text.length > 0, JSON.stringify(escPage.stages).slice(0, 80));
  const s1 = escPage.by_stage[0], s2 = escPage.by_stage[1];
  check("... the numbers per stage (2 plays of stage 1, 1 finished; 2 of stage 2)", s1.plays.plays === 2 && s1.plays.finished === 1 &&
    s2.plays.plays === 2 && escPage.plays.plays === 4, JSON.stringify(escPage.by_stage.map((b) => b.plays.plays)));
  check("... the boards per stage (the stage 2 run on stage 2's only)", !s1.boards.unverified.length &&
    s2.boards.unverified.length === 1 && s2.boards.unverified[0].time === 222);
  check("... the list says it has 3 stages", (await (await get("/api/levels")).json()).levels.find((l) => l.id === escId).stages === 3);
  check("... and so does the signed list", (await (await get("/api/v1/index.json")).text()).includes(`"stages":3`));
  await post("/api/admin/takedown", JSON.stringify({ level: escId }), me);
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
if ((await get("/__scheduled?cron=17+3+*+*+*")).status === 404)  // (wrangler dev with its Local Explorer: its own route)
  await fetch(BASE + "/cdn-cgi/local/explorer/api/local/scheduled?worker=hitmanlevels",
    { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ cron: "17 3 * * *" }) });
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
check("the owner cannot block themself", (await post("/api/admin/block", JSON.stringify({ user: ov.users.find((u) => u.name === `Tester ${pid}`).id }), me)).status === 400);
r = await post("/api/admin/delete-user", JSON.stringify({ user: otherId }), me);
ov = await (await get("/api/admin/overview", me)).json();
check("an account deleted with its reports", r.status === 200 && !ov.users.some((u) => u.id === otherId) &&
  !ov.reports.some((x) => x.reporter === `Someone ${pid2}`));

r = await post("/api/logout", "", me);
check("signed out", r.status === 204 && (await get("/api/me", me)).status === 401);
sql(`UPDATE users SET admin = 0 WHERE provider_id = '${pid}';`);
console.log(`\n${failed ? failed + " FAILED" : "all passed"}`);
process.exit(failed ? 1 : 0);
