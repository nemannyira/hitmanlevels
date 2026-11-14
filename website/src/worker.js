// hitmanlevels.com's Worker (docs/site_api.md): the pages from public/, the Level Hub's files from R2, sign-in,
// uploads, reports and the owner's take-downs. The list of levels is signed here after every change.

import { callback, currentUser, logout, start } from "./auth.js";
import { publishList, hex } from "./sitelist.js";
import { checkPackage } from "./check.js";

const MAX_PACKAGE = 2 * 1024 * 1024;  // as the client's reader (docs/online_levels.md section 3)

function json(value, status = 200) {
  return new Response(JSON.stringify(value), { status, headers: { "Content-Type": "application/json; charset=utf-8" } });
}

// a POST from the site's own pages only (the session cookie is SameSite=Lax; this closes the rest)
function sameOrigin(request, env) {
  const o = request.headers.get("Origin");
  return o === env.SITE_ORIGIN || o === "http://localhost:8787" || o === "http://127.0.0.1:8787";
}

// GET /api/v1/index.json|index.sig, /levels/<id>/<version>.h2lvl: as stored
async function file(env, key, type) {
  const obj = await env.FILES.get(key);
  if (!obj) return new Response("Not found", { status: 404 });
  return new Response(obj.body, {
    headers: { "Content-Type": type, "Cache-Control": key.startsWith("api/") ? "no-cache" : "public, max-age=31536000, immutable" },
  });
}

// POST /api/upload (the package's bytes; ?agree=1): checked as the client checks it, then listed
async function upload(request, env, user) {
  if (new URL(request.url).searchParams.get("agree") !== "1")
    return json({ error: "Agree to the terms first (it is your work, under the license it names)." }, 400);
  const size = Number(request.headers.get("Content-Length") || 0);
  if (!size || size > MAX_PACKAGE) return json({ error: "A package is at most 2 MB." }, 413);
  const bytes = new Uint8Array(await request.arrayBuffer());
  if (bytes.length > MAX_PACKAGE) return json({ error: "A package is at most 2 MB." }, 413);

  const checked = await checkPackage(bytes);  // the client's own reader and level check (WebAssembly)
  if (!checked.ok) return json({ error: checked.error, checks: checked.checks }, 422);
  const m = checked.manifest;  // id, version, title, base_scene, game, license, author { key, name }

  const author = await env.DB.prepare("SELECT user_id, blocked FROM authors WHERE key = ?").bind(m.author.key).first();
  if (author && author.blocked) return json({ error: "This author key is blocked.", checks: checked.checks }, 403);
  if (author && author.user_id !== user.id)
    return json({ error: "This author key belongs to another account: only its first uploader may upload with it.", checks: checked.checks }, 403);
  const level = await env.DB.prepare("SELECT author_key, taken_down FROM levels WHERE id = ?").bind(m.id).first();
  if (level && level.author_key !== m.author.key)
    return json({ error: "A level with this id is another author's.", checks: checked.checks }, 403);
  if (level && level.taken_down) return json({ error: "This level was taken down.", checks: checked.checks }, 403);
  const newest = await env.DB.prepare("SELECT MAX(version) AS v FROM versions WHERE level_id = ?").bind(m.id).first();
  if (newest && newest.v !== null && m.version <= newest.v)
    return json({ error: `Version ${m.version} is not newer than version ${newest.v} on the site.`, checks: checked.checks }, 409);

  const path = `/levels/${m.id}/${m.version}.h2lvl`;
  const sha256 = hex(await crypto.subtle.digest("SHA-256", bytes));
  await env.FILES.put(path.slice(1), bytes, { httpMetadata: { contentType: "application/octet-stream" } });
  const page = checked.page;
  if (page.preview)
    await env.FILES.put(`levels/${m.id}/${m.version}/preview.png`, page.preview, { httpMetadata: { contentType: "image/png" } });
  const now = Math.floor(Date.now() / 1000);
  await env.DB.batch([
    env.DB.prepare("INSERT OR IGNORE INTO authors (key, user_id) VALUES (?, ?)").bind(m.author.key, user.id),
    env.DB.prepare("INSERT OR IGNORE INTO levels (id, game, author_key) VALUES (?, ?, ?)").bind(m.id, m.game, m.author.key),
    env.DB.prepare(
      `INSERT INTO versions (level_id, version, title, base_scene, license, author_name, path, size, sha256, uploaded_by, created,
                             readme, briefing, objectives, preview)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
    ).bind(m.id, m.version, m.title, m.base_scene, m.license, m.author.name, path, bytes.length, sha256, user.id, now,
      page.readme, page.briefing, JSON.stringify(page.objectives), page.preview ? 1 : 0),
  ]);
  await publishList(env);
  return json({ id: m.id, version: m.version, path, checks: checked.checks });
}

// the levels listed (as the signed list: each level's newest version not taken down, of an author not blocked),
// for the site's own pages: GET /api/levels, GET /api/levels/<id>
const LISTED = `FROM versions v JOIN levels l ON l.id = v.level_id JOIN authors a ON a.key = l.author_key
  WHERE v.taken_down = 0 AND l.taken_down = 0 AND a.blocked = 0
    AND v.version = (SELECT MAX(v2.version) FROM versions v2 WHERE v2.level_id = v.level_id AND v2.taken_down = 0)`;

// a line for the list: the laptop's briefing, else the README's text (its headings and Markdown marks left out)
function blurb(v) {
  const readme = (v.readme || "").split("\n").filter((l) => !/^\s*#/.test(l)).join(" ").replace(/[*_`>]/g, "");
  const t = (v.briefing || readme).replace(/\s+/g, " ").trim();
  return t.length > 180 ? t.slice(0, 177).replace(/\s\S*$/, "") + "..." : t;
}

async function levels(env) {
  const { results } = await env.DB.prepare(
    `SELECT v.level_id, l.game, v.title, v.base_scene, v.author_name, v.version, v.created, v.license, v.size,
            v.briefing, v.readme, v.preview, l.downloads ${LISTED} ORDER BY v.created DESC`).all();
  return json({ levels: results.map((v) => ({
    id: v.level_id, game: v.game, title: v.title, base_scene: v.base_scene, author: v.author_name, version: v.version,
    updated: v.created, license: v.license, size: v.size, blurb: blurb(v), downloads: v.downloads,
    preview: v.preview ? `/levels/${v.level_id}/${v.version}/preview.png` : null,
  })) });
}

async function level(env, id) {
  const v = await env.DB.prepare(
    `SELECT v.*, l.game, l.author_key, l.downloads ${LISTED} AND v.level_id = ?`).bind(id).first();
  if (!v) return json({ error: "No such level" }, 404);
  const { results } = await env.DB.prepare(
    "SELECT version, created, size FROM versions WHERE level_id = ? AND taken_down = 0 ORDER BY version DESC").bind(id).all();
  return json({
    id, game: v.game, title: v.title, base_scene: v.base_scene, author: { name: v.author_name, key: v.author_key },
    version: v.version, updated: v.created, license: v.license, size: v.size, downloads: v.downloads,
    readme: v.readme, briefing: v.briefing, objectives: JSON.parse(v.objectives || "[]"), path: v.path,
    preview: v.preview ? `/levels/${id}/${v.version}/preview.png` : null, versions: results,
  });
}

// POST /api/report { level, version?, reason }
async function report(request, env, user) {
  const body = await request.json().catch(() => null);
  const reason = body && typeof body.reason === "string" ? body.reason.trim().slice(0, 2000) : "";
  if (!body || typeof body.level !== "string" || !reason) return json({ error: "Which level, and why?" }, 400);
  await env.DB.prepare("INSERT INTO reports (level_id, version, user_id, reason, created) VALUES (?, ?, ?, ?, ?)")
    .bind(body.level, Number.isInteger(body.version) ? body.version : null, user.id, reason, Math.floor(Date.now() / 1000)).run();
  return json({ ok: true });
}

// the owner's (users.admin = 1): GET /api/admin/overview (the reports, levels, accounts) and the actions
// POST /api/admin/<action> { ... }: takedown / restore { level, version? }, block / unblock { user | key },
// handled { report }, delete-user { user } (an account and everything of it: privacy.html "your rights")
async function adminOverview(env) {
  const [reports, levels, users] = await env.DB.batch([
    env.DB.prepare(
      `SELECT r.id, r.level_id, r.version, r.reason, r.created, r.handled, r.handled_at, u.name AS reporter,
              (SELECT title FROM versions v WHERE v.level_id = r.level_id ORDER BY version DESC LIMIT 1) AS title
         FROM reports r JOIN users u ON u.id = r.user_id ORDER BY r.handled, r.created DESC LIMIT 500`),
    env.DB.prepare(
      `SELECT l.id, l.taken_down, l.downloads, l.author_key, a.blocked AS key_blocked, a.user_id, u.name AS account,
              (SELECT title FROM versions v WHERE v.level_id = l.id ORDER BY version DESC LIMIT 1) AS title,
              (SELECT author_name FROM versions v WHERE v.level_id = l.id ORDER BY version DESC LIMIT 1) AS author,
              (SELECT json_group_array(json_object('version', version, 'taken_down', taken_down, 'created', created))
                 FROM versions v WHERE v.level_id = l.id) AS versions
         FROM levels l JOIN authors a ON a.key = l.author_key JOIN users u ON u.id = a.user_id ORDER BY title`),
    env.DB.prepare(
      `SELECT u.id, u.name, u.provider, u.created, u.blocked, u.admin,
              (SELECT COUNT(*) FROM levels l JOIN authors a ON a.key = l.author_key WHERE a.user_id = u.id) AS levels
         FROM users u ORDER BY u.created DESC LIMIT 1000`),
  ]);
  return json({
    reports: reports.results,
    levels: levels.results.map((l) => ({ ...l, versions: JSON.parse(l.versions) })),
    users: users.results,
  });
}

async function deleteUser(env, id) {
  const levels = await env.DB.prepare(
    "SELECT l.id FROM levels l JOIN authors a ON a.key = l.author_key WHERE a.user_id = ?").bind(id).all();
  for (const { id: level } of levels.results) {
    const files = await env.FILES.list({ prefix: `levels/${level}/` });
    if (files.objects.length) await env.FILES.delete(files.objects.map((o) => o.key));
  }
  await env.DB.batch([
    env.DB.prepare(`DELETE FROM versions WHERE level_id IN
      (SELECT l.id FROM levels l JOIN authors a ON a.key = l.author_key WHERE a.user_id = ?)`).bind(id),
    env.DB.prepare(`DELETE FROM levels WHERE author_key IN (SELECT key FROM authors WHERE user_id = ?)`).bind(id),
    env.DB.prepare("DELETE FROM authors WHERE user_id = ?").bind(id),
    env.DB.prepare("DELETE FROM reports WHERE user_id = ?").bind(id),
    env.DB.prepare("DELETE FROM sessions WHERE user_id = ?").bind(id),
    env.DB.prepare("DELETE FROM users WHERE id = ?").bind(id),
  ]);
}

async function admin(request, env, action, me) {
  const b = await request.json().catch(() => ({}));
  const level = typeof b.level === "string" ? b.level : null, version = Number.isInteger(b.version) ? b.version : null;
  const flag = action === "takedown" || action === "block" ? 1 : 0;
  if ((action === "takedown" || action === "restore") && level) {
    await (version !== null
      ? env.DB.prepare("UPDATE versions SET taken_down = ? WHERE level_id = ? AND version = ?").bind(flag, level, version)
      : env.DB.prepare("UPDATE levels SET taken_down = ? WHERE id = ?").bind(flag, level)).run();
  } else if ((action === "block" || action === "unblock") && Number.isInteger(b.user)) {
    if (b.user === me.id) return json({ error: "Not your own account" }, 400);
    await env.DB.batch([
      env.DB.prepare("UPDATE users SET blocked = ? WHERE id = ?").bind(flag, b.user),
      env.DB.prepare("DELETE FROM sessions WHERE user_id = ? AND ? = 1").bind(b.user, flag),
    ]);
  } else if ((action === "block" || action === "unblock") && typeof b.key === "string") {
    await env.DB.prepare("UPDATE authors SET blocked = ? WHERE key = ?").bind(flag, b.key).run();
  } else if (action === "handled" && Number.isInteger(b.report)) {
    await env.DB.prepare("UPDATE reports SET handled = 1, handled_at = ? WHERE id = ?")
      .bind(Math.floor(Date.now() / 1000), b.report).run();
    return json({ ok: true });
  } else if (action === "delete-user" && Number.isInteger(b.user)) {
    if (b.user === me.id) return json({ error: "Not your own account" }, 400);
    await deleteUser(env, b.user);
  } else {
    return json({ error: "What to do, to what?" }, 400);
  }
  return json({ list: (await publishList(env)).serial });
}

// every day: reports dealt with more than a year ago and sessions that ended are deleted (privacy.html)
async function cleanUp(env) {
  const now = Math.floor(Date.now() / 1000);
  await env.DB.batch([
    env.DB.prepare("DELETE FROM reports WHERE handled = 1 AND handled_at < ?").bind(now - 365 * 86400),
    env.DB.prepare("DELETE FROM sessions WHERE expires < ?").bind(now),
  ]);
}

// every answer, the pages' too: not to be indexed while the site is not open (robots.txt says the same)
function noIndex(res) {
  const out = new Response(res.body, res);
  out.headers.set("X-Robots-Tag", "noindex, nofollow");
  return out;
}

export default {
  async fetch(request, env, ctx) {
    return noIndex(await route(request, env, ctx));
  },

  async scheduled(event, env, ctx) {
    ctx.waitUntil(cleanUp(env));
  },
};

async function route(request, env, ctx) {
  const url = new URL(request.url);
  const path = url.pathname;
  let m;
  if (request.method === "GET") {
    if (path === "/api/v1/index.json") return file(env, "api/v1/index.json", "application/json; charset=utf-8");
    if (path === "/api/v1/index.sig") return file(env, "api/v1/index.sig", "application/octet-stream");
    if ((m = path.match(/^\/levels\/([a-z0-9]{8,32})\/([1-9][0-9]{0,8})\.h2lvl$/))) {
      const res = await file(env, `levels/${m[1]}/${m[2]}.h2lvl`, "application/octet-stream");
      if (res.ok) ctx.waitUntil(env.DB.prepare("UPDATE levels SET downloads = downloads + 1 WHERE id = ?").bind(m[1]).run());
      return res;
    }
    if ((m = path.match(/^\/levels\/([a-z0-9]{8,32})\/([1-9][0-9]{0,8})\/preview\.png$/)))
      return file(env, `levels/${m[1]}/${m[2]}/preview.png`, "image/png");
    if ((m = path.match(/^\/downloads\/(hitmanlevels-[a-z0-9.-]+\.zip)$/)))  // the releases (tools.html)
      return file(env, `downloads/${m[1]}`, "application/zip");
    if (path === "/api/levels") return levels(env);
    if (path === "/api/admin/overview") {
      const user = await currentUser(request, env);
      return user && user.admin ? adminOverview(env) : json({ error: "The owner's only" }, 403);
    }
    if ((m = path.match(/^\/api\/levels\/([a-z0-9]{8,32})$/))) return level(env, m[1]);
    if ((m = path.match(/^\/api\/auth\/(discord|github|google)\/start$/))) return start(request, env, m[1]);
    if ((m = path.match(/^\/api\/auth\/(discord|github|google)\/callback$/))) return callback(request, env, m[1]);
    if (path === "/api/me") {
      const user = await currentUser(request, env);
      if (!user) return json({ error: "Not signed in" }, 401);
      // the account's author keys and levels (the upload page's box)
      const keys = await env.DB.prepare("SELECT key FROM authors WHERE user_id = ?").bind(user.id).all();
      const levels = await env.DB.prepare(
        `SELECT l.id, v.title, MAX(v.version) AS version FROM levels l JOIN authors a ON a.key = l.author_key
           JOIN versions v ON v.level_id = l.id WHERE a.user_id = ? AND l.taken_down = 0 GROUP BY l.id ORDER BY v.title`,
      ).bind(user.id).all();
      return json({ name: user.name, provider: user.provider, admin: !!user.admin,
        keys: keys.results.map((k) => k.key), levels: levels.results });
    }
  }
  if (request.method === "POST" && path.startsWith("/api/")) {
    if (!sameOrigin(request, env)) return json({ error: "From the site's own pages only" }, 403);
    if (path === "/api/logout") return logout(request, env);
    const user = await currentUser(request, env);
    if (!user) return json({ error: "Sign in first" }, 401);
    if (path === "/api/upload") return upload(request, env, user);
    if (path === "/api/report") return report(request, env, user);
    if ((m = path.match(/^\/api\/admin\/(takedown|restore|block|unblock|handled|delete-user)$/))) {
      if (!user.admin) return json({ error: "The owner's only" }, 403);
      return admin(request, env, m[1], user);
    }
  }
  if (path.startsWith("/api/") || path.startsWith("/levels/") || path.startsWith("/downloads/")) return json({ error: "Not found" }, 404);
  return env.ASSETS.fetch(request);
}
