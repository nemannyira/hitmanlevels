# Monocypher 4.0.3

Ed25519 (RFC 8032) for the level packages (`src/levelfmt/ed25519.cpp`).

* Source: https://github.com/LoupVaillant/Monocypher/releases/download/4.0.3/monocypher-4.0.3.tar.gz
  (released 2026-06-15; fixes a timing leak in EdDSA / Ed25519 signing)
* SHA-512 of the archive, as monocypher.org/download publishes it:
  `40904ada5c7ee4f7741733e38b69a30a4b0561cbffba5ffe7c2dce16136d540251ec0d9056ff606510d3b5b708fb8a40db7e0870d4a0b2dc17ba2bfb880f8965`
* Files taken unchanged: `src/monocypher.c`, `src/monocypher.h`,
  `src/optional/monocypher-ed25519.c`, `src/optional/monocypher-ed25519.h`, `LICENCE.md`
* Licence: 2-clause BSD or CC0 (LICENCE.md)
* Audited by Cure53 (2017); https://monocypher.org
