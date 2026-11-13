// The list of published levels the Level Hub reads (docs/site_api.md): index.json in its canonical form
// (compact, keys sorted: shared/levelfmt/hub.cpp refuses any other spelling) and index.sig, the site key's
// Ed25519 signature of "H2LVL index v1\n" + index.json.

const PREFIX = "H2LVL index v1\n";

// JSON as nlohmann::json's dump(): no spaces, object keys sorted (by their UTF-16 code units, as JS sorts
// them; the keys here are ASCII), integers only
export function canonical(value) {
  if (value === null || typeof value === "boolean") return JSON.stringify(value);
  if (typeof value === "number") {
    if (!Number.isSafeInteger(value)) throw new Error("only integers in the list");
    return String(value);
  }
  if (typeof value === "string") return JSON.stringify(value);
  if (Array.isArray(value)) return "[" + value.map(canonical).join(",") + "]";
  const keys = Object.keys(value).sort();
  return "{" + keys.map((k) => JSON.stringify(k) + ":" + canonical(value[k])).join(",") + "}";
}

export function hex(bytes) {
  return [...new Uint8Array(bytes)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function unhex(text) {
  if (!/^[0-9a-f]*$/.test(text) || text.length % 2) throw new Error("not hex");
  return Uint8Array.from(text.match(/../g) || [], (h) => parseInt(h, 16));
}

// the site key from its 32-byte seed (the SITE_SEED secret, 64 hex) as WebCrypto takes it: PKCS#8
async function siteKey(seedHex) {
  const seed = unhex(seedHex);
  if (seed.length !== 32) throw new Error("SITE_SEED: 64 hex digits");
  const pkcs8 = new Uint8Array([...unhex("302e020100300506032b657004220420"), ...seed]);
  return crypto.subtle.importKey("pkcs8", pkcs8, { name: "Ed25519" }, false, ["sign"]);
}

// index.json's text and its 64-byte signature
export async function signList(list, seedHex) {
  const text = canonical(list);
  const message = new TextEncoder().encode(PREFIX + text);
  const sig = await crypto.subtle.sign({ name: "Ed25519" }, await siteKey(seedHex), message);
  return { text, sig: new Uint8Array(sig) };
}

// the list as the database has it now: each level's newest version not taken down, of a level and an
// author key not taken down / blocked; written to R2 (api/v1/index.json, .sig) with the next serial
export async function publishList(env) {
  const { results } = await env.DB.prepare(
    `SELECT v.level_id, v.version, v.title, v.base_scene, v.license, v.author_name, v.path, v.size, v.sha256,
            l.game, l.author_key
       FROM versions v JOIN levels l ON l.id = v.level_id JOIN authors a ON a.key = l.author_key
      WHERE v.taken_down = 0 AND l.taken_down = 0 AND a.blocked = 0
        AND v.version = (SELECT MAX(v2.version) FROM versions v2 WHERE v2.level_id = v.level_id AND v2.taken_down = 0)
      ORDER BY v.level_id`,
  ).all();
  const row = await env.DB.prepare("UPDATE meta SET value = CAST(value AS INTEGER) + 1 WHERE key = 'serial' RETURNING value").first();
  const list = {
    format: 1,
    serial: Number(row.value),
    levels: results.map((r) => ({
      author: { key: r.author_key, name: r.author_name },
      base_scene: r.base_scene,
      game: r.game,
      id: r.level_id,
      license: r.license,
      path: r.path,
      sha256: r.sha256,
      size: r.size,
      title: r.title,
      version: r.version,
    })),
  };
  const { text, sig } = await signList(list, env.SITE_SEED);
  await env.FILES.put("api/v1/index.json", text, { httpMetadata: { contentType: "application/json; charset=utf-8" } });
  await env.FILES.put("api/v1/index.sig", sig, { httpMetadata: { contentType: "application/octet-stream" } });
  return list;
}
