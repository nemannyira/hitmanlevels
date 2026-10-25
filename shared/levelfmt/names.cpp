#include "names.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace h2::levelfmt {

namespace {

bool EqualNoCase(const std::string& a, const char* b)
{
    size_t n = strlen(b);
    if (a.size() != n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

} // namespace

bool ValidScriptName(const std::string& s)
{
    if (s.size() < 5 || s.size() > 36 || !EqualNoCase(s.substr(s.size() - 4), ".lua"))
        return false;
    std::string base = s.substr(0, s.size() - 4);
    for (const char* device : {"CON", "PRN", "AUX", "NUL"})  // (Windows' devices, with any extension too)
        if (EqualNoCase(base, device))
            return false;
    if (base.size() == 4 && (EqualNoCase(base.substr(0, 3), "COM") || EqualNoCase(base.substr(0, 3), "LPT")) &&
        std::isdigit(static_cast<unsigned char>(base[3])))
        return false;
    return std::all_of(base.begin(), base.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
    });
}

bool ValidOnlineId(const std::string& id)
{
    return id.size() >= 8 && id.size() <= 32 &&
           std::all_of(id.begin(), id.end(), [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'); });
}

} // namespace h2::levelfmt
