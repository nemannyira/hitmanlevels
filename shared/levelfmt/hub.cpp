#include "hub.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <winhttp.h>

#include <algorithm>
#include <cctype>

#include "crypto.h"
#include "files.h"
#include "json.hpp"
#include "games.h"
#include "names.h"

namespace h2::levelfmt {

using nlohmann::json;
using namespace files;

namespace {

constexpr char kIndexPrefix[] = "H2LVL index v1\n";
// the site's public keys (two: one can be rotated). The first made 2026-11-13 by website/tools/make_site_key.mjs:
// its secret half is the Worker's SITE_SEED only (docs/site_api.md "Signing")
constexpr const char* kSiteKeys[] = {"239ef6a67cdacbfb7c7e5c031f5ebc5c97744e9d0d7047a76f1b3e47167b28e2", nullptr};

bool HttpsGet(const std::wstring& host, const std::wstring& path, size_t max, std::string& out, std::string* error)
{
    out.clear();
    HINTERNET session = WinHttpOpen(L"levelpack/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return Fail(error, "WinHTTP is not there");
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x2000;  // TLS 1.2, 1.3 (WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3)
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;  // (a Windows without TLS 1.3)
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof redirects);
    WinHttpSetTimeouts(session, 10000, 10000, 20000, 30000);
    bool ok = false;
    std::string why = "no answer from the site";
    HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    // (no WINHTTP_OPTION_SECURITY_FLAGS: the certificate checks stay as the system makes them)
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            why = "the site answered " + std::to_string(status);
        } else {
            ok = true;
            for (;;) {
                DWORD avail = 0, got = 0;
                if (!WinHttpQueryDataAvailable(request, &avail)) {
                    ok = false;
                    why = "the download broke off";
                    break;
                }
                if (avail == 0)
                    break;
                if (out.size() + avail > max) {
                    ok = false;
                    why = "larger than " + std::to_string(max) + " bytes";
                    break;
                }
                size_t at = out.size();
                out.resize(at + avail);
                if (!WinHttpReadData(request, &out[at], avail, &got)) {
                    ok = false;
                    why = "the download broke off";
                    break;
                }
                out.resize(at + got);
            }
        }
    } else if (GetLastError() == ERROR_WINHTTP_SECURE_FAILURE) {
        why = "the site's certificate is not right (not connected)";
    }
    if (request)
        WinHttpCloseHandle(request);
    if (connect)
        WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (!ok)
        out.clear();
    return ok ? true : Fail(error, why);
}

bool HexKey(const std::string& hex, uint8_t key[32]) { return crypto::FromHex(hex, key, 32); }

} // namespace

const HubLevel* HubIndex::Find(const std::string& id) const
{
    for (const HubLevel& l : levels)
        if (l.id == id)
            return &l;
    return nullptr;
}

std::vector<std::string> SiteKeys()
{
    std::vector<std::string> keys;
    for (const char* k : kSiteKeys)
        if (k)
            keys.push_back(k);
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(L"HITMANLEVELS_SITE_KEYS", buf, 1024);
    if (n && n < 1024) {
        std::string list = Utf8(std::wstring(buf, n));
        for (size_t at = 0; at <= list.size();) {
            size_t comma = list.find(',', at);
            std::string k = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            uint8_t key[32];
            if (HexKey(k, key))
                keys.push_back(k);
            if (comma == std::string::npos)
                break;
            at = comma + 1;
        }
    }
    return keys;
}

bool Fetch(const std::string& source, const std::string& path, size_t max, std::string& out, std::string* error)
{
    // the path is ours ("/api/v1/index.json", "/levels/<id>/<version>.h2lvl"): letters, digits, / . _ -
    if (path.empty() || path[0] != '/' || path.find("..") != std::string::npos || path.size() > 200 ||
        !std::all_of(path.begin(), path.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '.' || c == '_' || c == '-';
        }))
        return Fail(error, "not a path of the site: " + path.substr(0, 80));
    if (source.compare(0, 8, "https://") == 0) {
        std::string host = source.substr(8);
        if (host.empty() || host.find_first_of("/:@?#\\") != std::string::npos)
            return Fail(error, "the site is given as https://host only");
        return HttpsGet(std::wstring(host.begin(), host.end()), std::wstring(path.begin(), path.end()), max, out, error);
    }
    if (source.compare(0, 5, "http:") == 0)
        return Fail(error, "plain HTTP is not used: https:// only");
    std::string file = source;
    for (char c : path)
        file += c == '/' ? '\\' : c;
    if (!ReadFileA(file, out, max))
        return Fail(error, file + ": missing or larger than " + std::to_string(max) + " bytes");
    return true;
}

bool LoadHubIndex(const std::string& source, HubIndex& index, std::string* error)
{
    index = {};
    std::vector<std::string> keys = SiteKeys();
    if (keys.empty())
        return Fail(error, "no site key built in yet: the Hub is off");
    std::string text, sig;
    if (!Fetch(source, kHubIndexPath, kMaxIndexBytes, text, error))
        return false;
    std::string sigPath = std::string(kHubIndexPath).substr(0, std::string(kHubIndexPath).size() - 4) + "sig";
    if (!Fetch(source, sigPath, 64, sig, error))
        return false;
    std::string message = kIndexPrefix + text;
    bool signedOk = false;
    for (const std::string& k : keys) {
        uint8_t key[32];
        if (sig.size() == 64 && HexKey(k, key) &&
            crypto::Ed25519Verify(reinterpret_cast<const uint8_t*>(sig.data()),
                                  reinterpret_cast<const uint8_t*>(message.data()), message.size(), key))
            signedOk = true;
    }
    if (!signedOk)
        return Fail(error, "the level list is not signed by the site's key (changed on the way, or not the site)");
    json j = json::parse(text, nullptr, false);
    if (!j.is_object() || j.dump() != text)
        return Fail(error, "the level list is not in its one canonical form");
    if (!j.value("format", json()).is_number_integer() || j["format"].get<long long>() != 1 ||
        !j.value("serial", json()).is_number_integer() || !j.value("levels", json()).is_array())
        return Fail(error, "a level list this client does not know (a newer one's?)");
    index.serial = j["serial"].get<long long>();
    for (const json& l : j["levels"]) {
        HubLevel h;
        if (!l.is_object() || !l.value("game", json()).is_string() || !l.value("id", json()).is_string() || !l.value("version", json()).is_number_integer() ||
            !l.value("path", json()).is_string() || !l.value("sha256", json()).is_string() ||
            !l.value("size", json()).is_number_unsigned() || !l.value("author", json()).is_object())
            return Fail(error, "the level list has an entry that is not a level");
        h.game = l["game"].get<std::string>();
        h.id = l["id"].get<std::string>();
        h.version = int(std::min<long long>(l["version"].get<long long>(), kMaxPackageVersion));
        h.path = l["path"].get<std::string>();
        h.sha256 = l["sha256"].get<std::string>();
        h.size = l["size"].get<size_t>();
        h.title = l.value("title", json()).is_string() ? l["title"].get<std::string>() : "";
        h.baseScene = l.value("base_scene", json()).is_string() ? l["base_scene"].get<std::string>() : "";
        h.license = l.value("license", json()).is_string() ? l["license"].get<std::string>() : "";
        h.authorName = l["author"].value("name", json()).is_string() ? l["author"]["name"].get<std::string>() : "";
        h.authorKey = l["author"].value("key", json()).is_string() ? l["author"]["key"].get<std::string>() : "";
        uint8_t d[32];
        if (!FindGame(h.game) || !ValidOnlineId(h.id) || h.version < 1 || h.size > kMaxPackageBytes || !crypto::FromHex(h.sha256, d, 32) ||
            !HexKey(h.authorKey, d) || !ValidManifestText(h.title, 64) || !ValidManifestText(h.authorName, 40))
            return Fail(error, "the level list's entry " + h.id.substr(0, 32) + " is not a level's");
        index.levels.push_back(h);
    }
    // never an older list than one seen: a replayed list would bring back versions the site took down
    std::wstring knownPath = DataDir(true) + L"known.json";
    json known = ReadJsonW(knownPath);
    if (!known.is_object())
        known = json::object();
    long long seen = known.value("hub_serial", json()).is_number_integer() ? known["hub_serial"].get<long long>() : 0;
    if (index.serial < seen)
        return Fail(error, "an older level list (" + std::to_string(index.serial) + ") than one seen before (" +
                               std::to_string(seen) + "): not used");
    if (index.serial > seen) {
        known["hub_serial"] = index.serial;
        WriteFileAtomic(knownPath, known.dump(2));
    }
    return true;
}

bool FetchHubPackage(const std::string& source, const HubLevel& level, OpenedPackage& out, std::string* error)
{
    std::string bytes;
    if (!Fetch(source, level.path, kMaxPackageBytes, bytes, error))
        return false;
    crypto::Sha256Digest d = crypto::Sha256(bytes.data(), bytes.size());
    if (bytes.size() != level.size || crypto::Hex(d.data(), d.size()) != level.sha256)
        return Fail(error, "the package is not the one the site's list names (size / SHA-256)");
    if (!OpenPackage(bytes, out, error))
        return false;
    if (out.manifest.game != level.game || out.manifest.id != level.id || out.manifest.version != level.version || out.manifest.authorKey != level.authorKey)
        return Fail(error, "the package's game, id, version or author is not the list's");
    return true;
}

bool MakeSiteKey(const std::string& keyFile, std::string* publicHex, std::string* error)
{
    uint8_t seed[32], pub[32];
    if (!crypto::RandomBytes(seed, 32))
        return Fail(error, "no random bytes from the system");
    crypto::Ed25519PublicKey(seed, pub);
    std::string sealed;
    bool ok = Protect(std::string(reinterpret_cast<char*>(seed), 32), sealed) &&
              WriteNewFileW(GamePath(keyFile), sealed, false);
    SecureZeroMemory(seed, sizeof seed);
    if (!ok)
        return Fail(error, keyFile + ": not written (is it there already?)");
    if (publicHex)
        *publicHex = crypto::Hex(pub, 32);
    return true;
}

bool SignIndex(const std::string& indexText, const std::string& keyFile, std::string& signature, std::string* error)
{
    json j = json::parse(indexText, nullptr, false);
    if (!j.is_object() || j.dump() != indexText)
        return Fail(error, "index.json is not in its canonical form (compact, keys sorted)");
    std::string sealed, seed;
    if (!ReadFileW(GamePath(keyFile), sealed, 4096) || !Unprotect(sealed, seed) || seed.size() != 32)
        return Fail(error, keyFile + ": not a site key of this Windows user");
    uint8_t pub[32];
    crypto::Ed25519PublicKey(reinterpret_cast<const uint8_t*>(seed.data()), pub);
    std::string message = kIndexPrefix + indexText;
    signature.assign(64, '\0');
    crypto::Ed25519Sign(reinterpret_cast<uint8_t*>(&signature[0]), reinterpret_cast<const uint8_t*>(message.data()),
                        message.size(), reinterpret_cast<const uint8_t*>(seed.data()), pub);
    SecureZeroMemory(&seed[0], seed.size());
    return true;
}

} // namespace h2::levelfmt
