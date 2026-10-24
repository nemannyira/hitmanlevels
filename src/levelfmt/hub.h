#pragma once
// The Level Hub's client side (docs/online_levels.md section 4, docs/site_api.md): the site's level list
// (index.json) signed by the site's Ed25519 key, the packages it lists downloaded over HTTPS and checked
// against it (size, SHA-256) and as every package is (OpenPackage: the author's signature); then the
// installer of share.h. A list older than one seen before is refused (no rollback to vulnerable versions).
// The site's operator signs the list offline with a key of this PC (MakeSiteKey / SignIndex).
#include <string>
#include <vector>

#include "package.h"

namespace h2::levelfmt {

constexpr const char* kHubSite = "https://hitmanlevels.com";
constexpr const char* kHubIndexPath = "/api/v1/index.json";  // index.sig beside it
constexpr size_t kMaxIndexBytes = 4u << 20;

struct HubLevel {
    std::string id, title, authorName, authorKey, baseScene, license, path, sha256;
    int version = 0;
    size_t size = 0;
};

struct HubIndex {
    long long serial = 0;  // grows with every list the site signs
    std::vector<HubLevel> levels;
    const HubLevel* Find(const std::string& id) const;
};

// The site's public keys this client believes (built in; H2LEVEL_SITE_KEYS, comma separated hex, for tests
// and the site's own staging). Empty: the Hub is off.
std::vector<std::string> SiteKeys();

// Bytes of a file of the site: `source` is "https://host" (WinHTTP, TLS 1.2+, the system's certificate
// checks, no plain HTTP, at most `max` bytes) or a local folder (tests, a mirror on disk: the signatures
// make no source trusted).
bool Fetch(const std::string& source, const std::string& path, size_t max, std::string& out, std::string* error);

// The list fetched and checked: index.sig by a site key over "H2LVL index v1\n" + index.json, the form
// canonical, its serial not below the highest this PC has seen (remembered in known.json after).
bool LoadHubIndex(const std::string& source, HubIndex& index, std::string* error);
// A listed level's package fetched and opened: its size and SHA-256 the list's, then OpenPackage, and the
// manifest's id / version / author key the list's.
bool FetchHubPackage(const std::string& source, const HubLevel& level, OpenedPackage& out, std::string* error);

// The site operator's side: a site key made on this PC (its seed sealed for this Windows user at
// `keyFile`), and an index.json signed with it (index.sig's bytes).
bool MakeSiteKey(const std::string& keyFile, std::string* publicHex, std::string* error);
bool SignIndex(const std::string& indexText, const std::string& keyFile, std::string& signature, std::string* error);

} // namespace h2::levelfmt
