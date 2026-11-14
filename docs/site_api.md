# hitmanlevels.com: what the client expects of the site

The client side of the Level Hub is built (`shared/levelfmt/hub.h`, `levelpack hub ...`, issue #6);
this is the contract the site has to keep. What the client reads is static files; the site behind
them (accounts, uploads, reports) is decided below (docs/online_levels.md sections 7-9).

## Files

| Path | Content |
|---|---|
| `/api/v1/index.json` | the list of published levels (below), in canonical JSON: compact, keys sorted, UTF-8 |
| `/api/v1/index.sig` | 64 bytes: the site key's Ed25519 signature of `"H2LVL index v1\n"` + index.json's bytes |
| `/levels/<id>/<version>.h2lvl` | the packages, as their authors signed them (docs/online_levels.md section 3) |

Paths are made of letters, digits, `/ . _ -` only, start with `/`, hold no `..`, at most 200
characters. Everything is served over HTTPS (TLS 1.2 or 1.3, a certificate the system trusts); the
client never uses plain HTTP and does not follow a redirect from HTTPS to HTTP.

## index.json

```json
{"format":1,"levels":[{"author":{"key":"<64 hex>","name":"Tester"},"base_scene":"C1-1\\C1-1__MAIN",
"game":"h2","id":"cewarg3802v86kr4","license":"CC BY 4.0","path":"/levels/cewarg3802v86kr4/2.h2lvl",
"sha256":"<64 hex>","size":8188,"title":"Courier","version":2}],"serial":5}
```

* `serial`: grows with every list the site signs. A client refuses a list with a lower serial than one
  it has seen (no rollback to a list that still names a version the site took down).
* each entry: the package's `game` (`h2`: Hitman 2, `.h2lvl`; later `h3`, `h4`), `id`, `version`, `title`, `author` (name and key as in its manifest),
  `base_scene`, `license`, its `path`, `size` and `sha256`. The client downloads the package, checks
  size and SHA-256 against the list, opens it as every package (the author's signature, the strict
  level check) and checks that its id, version and author key are the list's. Then it installs it as
  a package passed on by hand (the same questions about new authors, other authors, older versions).

## Runs (the play statistics, docs/play_stats.md)

`levelpack stats send` (#51; the player turned sharing on) posts each play of a downloaded level, one
request a run:

```
POST /api/v1/runs        Content-Type: application/json
{"aids":[],"counters":{"alerts":0,...,"shots":2},"dev":[],"dll":"0.1.0","events":[...],"format":1,
"game":"h2","level":"<id>","package_sha256":"<64 hex>","rating":"SilentAssassin","result":"completed",
"run":"<32 hex>","started":"2026-11-14T10:05:59Z","stealth":100,"aggression":1,"time":13,"version":3}
```

The run as the editor wrote it (docs/play_stats.md section 1; canonical JSON) plus the `game` and the
installed package's `package_sha256`, which with `level` and `version` must name a listed package. The
client takes the answer:

| Status | The client |
|---|---|
| 200, 201 | stored: the run goes to `runs\sent` |
| 409 | that run id is stored already: to `runs\sent` too |
| 400, 422 | refused (`{"error": "..."}` shown): to `runs\refused`, never sent again |
| anything else, no answer | kept, sent the next time (the rest waits too) |

Runs with `aids` or `dev` are never sent. No account, no IP kept (#52).

## Signing

The site key signs the list, never the packages (their authors do). With uploads and no review before
listing (below), the site signs the list itself after each change: the key is a Workers secret
(encrypted, not readable back from the dashboard), used by the Worker only (Ed25519 in Workers'
WebCrypto). `levelpack site keygen|sign` stay for a staging copy and for tests:

```
levelpack site keygen site.key            (prints the public key)
levelpack site sign index.json site.key   (writes index.sig beside it)
```

Who holds the Cloudflare account can sign any list: the account needs two-factor sign-in. A forged
list still cannot change a package (its author signs it; the client asks about new authors and warns
when another key signs a level it has).

The public key goes into the client (`kSiteKeys` in `shared/levelfmt/hub.cpp`, room for two so one can be
rotated). The site's key was made 2026-11-13 (`website/tools/make_site_key.mjs`: the seed straight into the
Worker's `SITE_SEED`, never shown): `239ef6a67cdacbfb7c7e5c031f5ebc5c97744e9d0d7047a76f1b3e47167b28e2`. For a staging copy of the site, `HITMANLEVELS_SITE_KEYS` (comma
separated hex) names other keys, and `--source <folder>` reads a site from disk (tests).

## Decided (the owner, 2026-11-13)

* **Hosting**: Cloudflare, behind hitmanlevels.com: Pages for the site's own pages, a Worker for the
  API and uploads, D1 for the accounts and the levels' records, R2 for the packages (to be enabled
  in the dashboard by the owner: it asks for billing details).
* **Accounts**: sign-in with Discord, GitHub or Google (OAuth 2.0 with PKCE); no passwords of the
  site's own. Kept: the provider's user id, a display name; no e-mail unless a provider gives it
  and it is needed.
* **Uploads**: any signed-in user. A level's author key is bound to the account that first uploads
  it; a package signed with that key from another account is refused (no one re-uploads another's
  level as theirs).
* **No review before listing**: an upload that passes the checks is listed at once. Players can
  report a level; the owner (admin) can take a level or a version down and block an account or an
  author key. The owner's page is `/admin.html` (reports, levels, accounts; an account deleted with
  its levels, packages and reports on request). An account becomes the owner's once, after its first
  sign-in: `npx wrangler d1 execute hitmanlevels --remote --command "UPDATE users SET admin = 1 WHERE
  id = <its id>"`. Every day at 03:17 UTC the Worker deletes reports dealt with more than a year ago and
  sessions that ended (privacy.html).
* **Checks on upload**: the package reader and the strict level check of the client
  (`shared/levelfmt`, `h2/src/levelfmt`), built to WebAssembly and run by the Worker: the site lists
  exactly what the client accepts, and a hostile file meets the parser in WebAssembly's sandbox.
  Then the author's signature, the id / version / key against what the site has (versions only
  grow), the size limits.
* **Language**: English.
* **Look**: after Hitman: Contracts' mood (night, rain, film noir: near-black, silver grey type, one
  blood-red accent; the menus' label left, value right), not its logos, art or fonts (IO
  Interactive's): a logo of the site's own and a "not affiliated with IO Interactive" notice.

## Still to do (the owner)

* ~~enable R2; two-factor sign-in on the Cloudflare account~~ (done 2026-11-13);
* the OAuth apps at Discord, GitHub and Google, named "Hitman Levels": redirect
  `https://hitmanlevels.com/api/auth/<provider>/callback` (and `http://localhost:8787/...` for
  `wrangler dev` where the provider takes a second one); scopes: Discord `identify`, GitHub none
  (public profile), Google `openid profile` (no e-mail). Their client ids and secrets go in as
  Workers secrets (`DISCORD_CLIENT_ID`, `DISCORD_CLIENT_SECRET`, `GITHUB_...`, `GOOGLE_...`), typed by
  the owner into `wrangler secret put`;
* terms of use and a privacy notice (GDPR: the account's id and name, an author's name and key, the
  reports; play records later).
