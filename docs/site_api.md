# hitmanlevels.com: what the client expects of the site

The client side of the Level Hub is built (`shared/levelfmt/hub.h`, `levelpack hub ...`, issue #6);
this is the contract the site has to keep. Everything here is static files: the site can be a plain
static host (for example Cloudflare Pages or R2 behind the hitmanlevels.com domain), with uploads,
accounts and moderation as a separate part later (docs/online_levels.md sections 7-9).

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

## Signing

The site key signs the list, never the packages (their authors do). The key is made and kept on the
operator's PC, sealed for that Windows user, and the list is signed there:

```
levelpack site keygen site.key            (once: prints the public key)
levelpack site sign index.json site.key   (writes index.sig beside it)
```

The public key goes into the client (`kSiteKeys` in `shared/levelfmt/hub.cpp`, room for two so one can be
rotated); until then the Hub is off. For a staging copy of the site, `HITMANLEVELS_SITE_KEYS` (comma
separated hex) names other keys, and `--source <folder>` reads a site from disk (tests).

## Still to decide (the owner)

* hosting of the static files (Cloudflare Pages / R2 fit the domain, the free tiers cover a level list
  of a few MB) and who publishes;
* uploads and accounts (sign-in through GitHub / Discord / Google, or accounts of the site's own);
* moderation: who looks at a level before it is listed;
* terms of use and privacy (GDPR: an author's name and key, play records later).
