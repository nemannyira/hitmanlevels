// The site's signing key (docs/site_api.md "Signing"): a new Ed25519 key, its seed handed straight to
// `wrangler secret put SITE_SEED` (never shown, never written to a file), its public key printed: that one
// goes into the client (kSiteKeys in shared/levelfmt/hub.cpp).
//   cd website; node tools/make_site_key.mjs
// Run again only to replace the key: lists signed with the old one are then refused by clients that do not
// have the new key yet (the client has room for two keys, for such a change).
import { execFileSync } from "node:child_process";
import { createPrivateKey, createPublicKey, randomBytes } from "node:crypto";

const seed = randomBytes(32);
const pkcs8 = Buffer.concat([Buffer.from("302e020100300506032b657004220420", "hex"), seed]);
const pub = createPublicKey(createPrivateKey({ key: pkcs8, format: "der", type: "pkcs8" }))
  .export({ format: "der", type: "spki" }).subarray(-32).toString("hex");
execFileSync("npx", ["wrangler", "secret", "put", "SITE_SEED"], {
  input: seed.toString("hex"),
  stdio: ["pipe", "inherit", "inherit"],
  shell: true,
  env: { ...process.env, WRANGLER_SEND_METRICS: "false" },
});
seed.fill(0);
pkcs8.fill(0);
console.log("\nThe site's public key (for kSiteKeys in shared/levelfmt/hub.cpp):\n" + pub);
