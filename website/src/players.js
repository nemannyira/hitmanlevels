// Level 2 of the play statistics (docs/play_stats.md section 4, #54): levelpack signed in to a site account (a
// device code the player approves on the site; levelpack keeps its token), run tickets (the site's time when a
// run starts), the boards (the fastest Silent Assassin runs: unverified, and verified by the owner from a video),
// a player's page.

import { cleanName, origin, random, sha256 } from "./auth.js";

const DEVICE_SECONDS = 600;
const CODE_LETTERS = "BCDFGHJKLMNPQRSTVWXZ";  // (no vowels: no words; no look-alikes)
const TIME_MARGIN = 30;  // seconds a run may be shorter than its ticket's time (clocks, the map's load)
const BOARD = `r.ranked = 1 AND r.result = 'completed' AND r.rating = 'SilentAssassin'`;

function json(value, status = 200) {
  return new Response(JSON.stringify(value), { status, headers: { "Content-Type": "application/json; charset=utf-8" } });
}
const now = () => Math.floor(Date.now() / 1000);

// POST /api/v1/device: a sign-in of levelpack starts; it asks for its token with device_code (every interval)
export async function deviceStart(request, env) {
  const letters = crypto.getRandomValues(new Uint8Array(8));
  const userCode = Array.from(letters, (b) => CODE_LETTERS[b % CODE_LETTERS.length]).join("");
  const device = random(32);
  await env.DB.prepare("INSERT INTO device_codes (device_hash, user_code, expires) VALUES (?, ?, ?)")
    .bind(await sha256(device), userCode, now() + DEVICE_SECONDS).run();
  return json({ device_code: device, user_code: userCode.slice(0, 4) + "-" + userCode.slice(4),
    verification_uri: origin(request, env) + "/device.html", expires_in: DEVICE_SECONDS, interval: 5 });
}

// POST /api/device/approve { code } (the site's device page, signed in): that sign-in is this account's
export async function deviceApprove(request, env, user) {
  const b = await request.json().catch(() => ({}));
  const code = String(b.code || "").toUpperCase().replace(/[^A-Z]/g, "");
  const r = await env.DB.prepare("UPDATE device_codes SET user_id = ? WHERE user_code = ? AND expires > ? AND user_id IS NULL")
    .bind(user.id, code, now()).run();
  return r.meta.changes ? json({ ok: true }) : json({ error: "No such code (or it ran out: start again in levelpack)." }, 404);
}

// POST /api/v1/device/token { device_code }: 428 while not approved; then the token, once
export async function deviceToken(request, env) {
  const b = await request.json().catch(() => ({}));
  const hash = await sha256(String(b.device_code || ""));
  const d = await env.DB.prepare("SELECT user_id, expires FROM device_codes WHERE device_hash = ?").bind(hash).first();
  if (!d || d.expires <= now()) return json({ error: "expired" }, 400);
  if (d.user_id === null) return json({ error: "pending" }, 428);
  await env.DB.prepare("DELETE FROM device_codes WHERE device_hash = ?").bind(hash).run();
  const u = await env.DB.prepare("SELECT name, blocked FROM users WHERE id = ?").bind(d.user_id).first();
  if (!u || u.blocked) return json({ error: "This account is blocked" }, 403);
  const token = random(32);
  await env.DB.prepare("INSERT INTO client_tokens (token_hash, user_id, created) VALUES (?, ?, ?)")
    .bind(await sha256(token), d.user_id, now()).run();
  return json({ token, name: u.name });
}

// levelpack's account (Authorization: Bearer <token>); null: none, unknown, blocked
export async function clientUser(request, env) {
  const m = (request.headers.get("Authorization") || "").match(/^Bearer ([A-Za-z0-9_-]{20,100})$/);
  if (!m) return null;
  return env.DB.prepare(
    "SELECT u.id, u.name FROM client_tokens t JOIN users u ON u.id = t.user_id WHERE t.token_hash = ? AND u.blocked = 0",
  ).bind(await sha256(m[1])).first();
}

// POST /api/v1/signout: levelpack's token forgotten
export async function clientSignout(request, env) {
  const m = (request.headers.get("Authorization") || "").match(/^Bearer ([A-Za-z0-9_-]{20,100})$/);
  if (m) await env.DB.prepare("DELETE FROM client_tokens WHERE token_hash = ?").bind(await sha256(m[1])).run();
  return new Response(null, { status: 204 });
}

// POST /api/v1/tickets { level, version } (signed in): the site's time when a run of a listed level starts
export async function ticket(request, env, user) {
  const b = await request.json().catch(() => ({}));
  if (typeof b.level !== "string" || !/^[a-z0-9]{8,32}$/.test(b.level) || !Number.isInteger(b.version))
    return json({ error: "Which level and version?" }, 400);
  const listed = await env.DB.prepare(
    `SELECT 1 FROM versions v JOIN levels l ON l.id = v.level_id WHERE v.level_id = ? AND v.version = ? AND v.taken_down = 0
        AND l.taken_down = 0`).bind(b.level, b.version).first();
  if (!listed) return json({ error: "Not a listed level's version." }, 422);
  const id = Array.from(crypto.getRandomValues(new Uint8Array(16)), (x) => x.toString(16).padStart(2, "0")).join("");
  const issued = now();
  await env.DB.prepare("INSERT INTO tickets (id, user_id, level_id, version, issued) VALUES (?, ?, ?, ?, ?)")
    .bind(id, user.id, b.level, b.version, issued).run();
  return json({ ticket: id, issued });
}

// a run sent signed in: whether it goes on the boards (its ticket, once: the site saw at least its time pass
// between the ticket and now; its events in order and within its time). "" ranked, else why not.
export async function rankRun(env, user, r) {
  if (typeof r.ticket !== "string" || !/^[0-9a-f]{32}$/.test(r.ticket)) return "no ticket";
  const t = await env.DB.prepare(
    "SELECT issued FROM tickets WHERE id = ? AND user_id = ? AND level_id = ? AND version = ? AND used = 0")
    .bind(r.ticket, user.id, r.level, r.version).first();
  if (!t) return "not a ticket of this run";
  await env.DB.prepare("UPDATE tickets SET used = 1 WHERE id = ?").bind(r.ticket).run();
  if (now() - t.issued < r.time - TIME_MARGIN) return "shorter than the time since its ticket";
  for (let i = 0; i < r.events.length; i++)
    if (r.events[i].t > r.time + 5 || (i && r.events[i].t < r.events[i - 1].t)) return "its events out of order";
  return "";
}

// a level's boards: each player's fastest Silent Assassin run (unverified: every ranked one; verified: watched);
// `stage`: an escalation's stage (its times are compared within it only)
export async function boards(env, id, stage) {
  const s = stage ? `AND r.stage = ${Number(stage)}` : "";
  const q = (verified) => env.DB.prepare(
    `SELECT u.id, u.name, MIN(r.time) AS time FROM runs r JOIN users u ON u.id = r.user_id
      WHERE r.level_id = ? AND ${BOARD} AND u.blocked = 0 ${verified ? "AND r.verified = 1" : ""} ${s}
      GROUP BY u.id ORDER BY time, u.id LIMIT 10`).bind(id);
  const [unverified, verified] = await env.DB.batch([q(false), q(true)]);
  return { unverified: unverified.results, verified: verified.results };
}

// GET /api/players/<id>: a player's page (the listed levels they played, their best Silent Assassin times)
export async function player(env, id) {
  const u = await env.DB.prepare("SELECT id, name, created FROM users WHERE id = ? AND blocked = 0").bind(id).first();
  if (!u) return json({ error: "No such player" }, 404);
  const { results } = await env.DB.prepare(
    `SELECT r.level_id AS id, (SELECT title FROM versions v WHERE v.level_id = r.level_id ORDER BY version DESC LIMIT 1) AS title,
            r.stage, COUNT(*) AS plays, SUM(r.result = 'completed') AS finished,
            MIN(CASE WHEN ${BOARD} THEN r.time END) AS best, MAX(CASE WHEN ${BOARD} THEN r.verified ELSE 0 END) AS verified
       FROM runs r JOIN levels l ON l.id = r.level_id WHERE r.user_id = ? AND l.taken_down = 0
      GROUP BY r.level_id, r.stage ORDER BY title, r.stage`).bind(id).all();
  return json({ id: u.id, name: u.name, joined: u.created, levels: results });
}

// GET /api/me/runs (signed in): the player's runs that can go on the verified board (a video can be linked)
export async function myRuns(env, user) {
  const { results } = await env.DB.prepare(
    `SELECT r.id, r.level_id, (SELECT title FROM versions v WHERE v.level_id = r.level_id ORDER BY version DESC LIMIT 1) AS title,
            r.stage, r.time, r.received, r.video, r.verified FROM runs r WHERE r.user_id = ? AND ${BOARD}
      ORDER BY r.received DESC LIMIT 200`).bind(user.id).all();
  return json({ runs: results });
}

// POST /api/me/video { run, url } (signed in): a video of one of the player's runs; the owner watches it
export async function setVideo(request, env, user) {
  const b = await request.json().catch(() => ({}));
  const url = String(b.url || "").trim();
  if (url && (!/^https:\/\/[^\s<>"]{4,200}$/.test(url) || url.length > 210)) return json({ error: "A video's https:// address." }, 400);
  const r = await env.DB.prepare(`UPDATE runs SET video = ?, verified = 0 WHERE id = ? AND user_id = ? AND ranked = 1`)
    .bind(url || null, String(b.run || ""), user.id).run();
  return r.meta.changes ? json({ ok: true }) : json({ error: "Not a run of yours on the boards." }, 404);
}

// POST /api/me/name { name } (signed in): the name the site shows (uploads, boards, the player's page)
export async function setName(request, env, user) {
  const b = await request.json().catch(() => ({}));
  const name = cleanName(b.name);
  await env.DB.prepare("UPDATE users SET name = ? WHERE id = ?").bind(name, user.id).run();
  return json({ name });
}
