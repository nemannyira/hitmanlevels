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
  const now = Math.floor(Date.now() / 1000);
  await env.DB.batch([
    env.DB.prepare("INSERT OR IGNORE INTO authors (key, user_id) VALUES (?, ?)").bind(m.author.key, user.id),
    env.DB.prepare("INSERT OR IGNORE INTO levels (id, game, author_key) VALUES (?, ?, ?)").bind(m.id, m.game, m.author.key),
    env.DB.prepare(
      `INSERT INTO versions (level_id, version, title, base_scene, license, author_name, path, size, sha256, uploaded_by, created)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
    ).bind(m.id, m.version, m.title, m.base_scene, m.license, m.author.name, path, bytes.length, sha256, user.id, now),
  ]);
  await publishList(env);
  return json({ id: m.id, version: m.version, path, checks: checked.checks });
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

// POST /api/admin/takedown { level, version? } | /api/admin/block { user? , key? }: the owner's
async function admin(request, env, action) {
  const b = await request.json().catch(() => ({}));
  if (action === "takedown" && typeof b.level === "string") {
    await (Number.isInteger(b.version)
      ? env.DB.prepare("UPDATE versions SET taken_down = 1 WHERE level_id = ? AND version = ?").bind(b.level, b.version)
      : env.DB.prepare("UPDATE levels SET taken_down = 1 WHERE id = ?").bind(b.level)).run();
  } else if (action === "block" && Number.isInteger(b.user)) {
    await env.DB.batch([
      env.DB.prepare("UPDATE users SET blocked = 1 WHERE id = ?").bind(b.user),
      env.DB.prepare("DELETE FROM sessions WHERE user_id = ?").bind(b.user),
    ]);
  } else if (action === "block" && typeof b.key === "string") {
    await env.DB.prepare("UPDATE authors SET blocked = 1 WHERE key = ?").bind(b.key).run();
  } else {
    return json({ error: "What to take down or block?" }, 400);
  }
  return json({ list: (await publishList(env)).serial });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname;
    let m;
    if (request.method === "GET") {
      if (path === "/api/v1/index.json") return file(env, "api/v1/index.json", "application/json; charset=utf-8");
      if (path === "/api/v1/index.sig") return file(env, "api/v1/index.sig", "application/octet-stream");
      if ((m = path.match(/^\/levels\/([a-z0-9]{8,32})\/([1-9][0-9]{0,8})\.h2lvl$/)))
        return file(env, `levels/${m[1]}/${m[2]}.h2lvl`, "application/octet-stream");
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
      if ((m = path.match(/^\/api\/admin\/(takedown|block)$/))) {
        if (!user.admin) return json({ error: "The owner's only" }, 403);
        return admin(request, env, m[1]);
      }
    }
    if (path.startsWith("/api/") || path.startsWith("/levels/")) return json({ error: "Not found" }, 404);
    return env.ASSETS.fetch(request);
  },
};
