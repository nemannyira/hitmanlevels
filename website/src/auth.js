// Sign-in with Discord, GitHub or Google (OAuth 2.0 authorization code with PKCE and a state; docs/site_api.md):
// the site keeps the account's number there and a name to show; no password, no e-mail.

const PROVIDERS = {
  discord: {
    authorize: "https://discord.com/oauth2/authorize",
    token: "https://discord.com/api/oauth2/token",
    scope: "identify",
    user: "https://discord.com/api/users/@me",
    who: (u) => ({ id: u.id, name: u.global_name || u.username }),
  },
  github: {
    authorize: "https://github.com/login/oauth/authorize",
    token: "https://github.com/login/oauth/access_token",
    scope: "",  // the public profile only
    user: "https://api.github.com/user",
    who: (u) => ({ id: u.id, name: u.login }),
  },
  google: {
    authorize: "https://accounts.google.com/o/oauth2/v2/auth",
    token: "https://oauth2.googleapis.com/token",
    scope: "openid profile",
    user: "https://openidconnect.googleapis.com/v1/userinfo",
    who: (u) => ({ id: u.sub, name: u.name }),
  },
};

const SESSION_DAYS = 30;

function b64url(bytes) {
  return btoa(String.fromCharCode(...new Uint8Array(bytes))).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}
function random(n) {
  return b64url(crypto.getRandomValues(new Uint8Array(n)));
}
async function sha256(text) {
  return b64url(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text)));
}

// the origin to send people back to: the site's own, or wrangler dev's (never one the request names)
export function origin(request, env) {
  const o = new URL(request.url).origin;
  return o === "http://localhost:8787" || o === "http://127.0.0.1:8787" ? o : env.SITE_ORIGIN;
}

export function cookie(request, name) {
  const m = (request.headers.get("Cookie") || "").match(new RegExp("(?:^|;\\s*)" + name + "=([^;]*)"));
  return m ? m[1] : "";
}

function setCookie(name, value, maxAge) {
  return `${name}=${value}; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=${maxAge}`;
}

// a name as the site shows it: no control or direction-changing characters, at most 32 characters
export function cleanName(name) {
  const s = String(name || "").replace(/[\p{C}​-‏‪-‮⁦-⁩]/gu, "").trim();
  return [...s].slice(0, 32).join("") || "player";
}

// GET /api/auth/<provider>/start: off to the provider, the state and the PKCE verifier in a short cookie
export async function start(request, env, provider) {
  const p = PROVIDERS[provider];
  const id = env[provider.toUpperCase() + "_CLIENT_ID"];
  if (!p || !id) return new Response("No such sign-in", { status: 404 });
  const state = random(24), verifier = random(48);
  const url = new URL(p.authorize);
  url.search = new URLSearchParams({
    response_type: "code", client_id: id, redirect_uri: `${origin(request, env)}/api/auth/${provider}/callback`,
    state, code_challenge: await sha256(verifier), code_challenge_method: "S256", ...(p.scope ? { scope: p.scope } : {}),
  });
  return new Response(null, {
    status: 302,
    headers: { Location: url.toString(), "Set-Cookie": setCookie("__Host-oauth", `${provider}.${state}.${verifier}`, 600) },
  });
}

// GET /api/auth/<provider>/callback: the code for a token, the token for the account; a session
export async function callback(request, env, provider) {
  const p = PROVIDERS[provider];
  const url = new URL(request.url);
  const [cp, state, verifier] = cookie(request, "__Host-oauth").split(".");
  if (!p || cp !== provider || !state || url.searchParams.get("state") !== state || !url.searchParams.get("code"))
    return new Response("The sign-in did not come back as it left (try again)", { status: 400 });
  const key = provider.toUpperCase();
  const tokenRes = await fetch(p.token, {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded", Accept: "application/json" },
    body: new URLSearchParams({
      grant_type: "authorization_code", code: url.searchParams.get("code"), code_verifier: verifier,
      redirect_uri: `${origin(request, env)}/api/auth/${provider}/callback`,
      client_id: env[key + "_CLIENT_ID"], client_secret: env[key + "_CLIENT_SECRET"],
    }),
  });
  const token = tokenRes.ok ? await tokenRes.json() : null;
  if (!token || !token.access_token) return new Response("The sign-in was refused", { status: 502 });
  const userRes = await fetch(p.user, { headers: { Authorization: `Bearer ${token.access_token}`, "User-Agent": "hitmanlevels.com" } });
  const who = userRes.ok ? p.who(await userRes.json()) : null;
  if (!who || who.id === undefined || who.id === null) return new Response("The account could not be read", { status: 502 });

  const now = Math.floor(Date.now() / 1000);
  const user = await env.DB.prepare(
    `INSERT INTO users (provider, provider_id, name, created) VALUES (?, ?, ?, ?)
     ON CONFLICT (provider, provider_id) DO UPDATE SET provider = provider RETURNING id, blocked`,
  ).bind(provider, String(who.id), cleanName(who.name), now).first();
  if (user.blocked) return new Response("This account is blocked", { status: 403 });
  const session = random(32);
  await env.DB.prepare("INSERT INTO sessions (token_hash, user_id, expires) VALUES (?, ?, ?)")
    .bind(await sha256(session), user.id, now + SESSION_DAYS * 86400).run();
  const headers = new Headers({ Location: "/upload.html" });
  headers.append("Set-Cookie", setCookie("__Host-session", session, SESSION_DAYS * 86400));
  headers.append("Set-Cookie", setCookie("__Host-oauth", "", 0));
  return new Response(null, { status: 302, headers });
}

// the signed-in account of a request (null: none, expired, blocked)
export async function currentUser(request, env) {
  const session = cookie(request, "__Host-session");
  if (!session) return null;
  return env.DB.prepare(
    `SELECT u.id, u.name, u.provider, u.admin FROM sessions s JOIN users u ON u.id = s.user_id
      WHERE s.token_hash = ? AND s.expires > ? AND u.blocked = 0`,
  ).bind(await sha256(session), Math.floor(Date.now() / 1000)).first();
}

// POST /api/logout
export async function logout(request, env) {
  const session = cookie(request, "__Host-session");
  if (session) await env.DB.prepare("DELETE FROM sessions WHERE token_hash = ?").bind(await sha256(session)).run();
  return new Response(null, { status: 204, headers: { "Set-Cookie": setCookie("__Host-session", "", 0) } });
}
