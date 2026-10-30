#include "../OsInternal.h"

#include <vector>

namespace enp::os::fs::path {

namespace {

bool isSep(char c) { return c == '/' || c == '\\'; }

bool hasDrivePrefix(std::string_view p)
{
    return p.size() >= 2 && p[1] == ':' &&
           ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'));
}

} // namespace

bool isAbsolute(std::string_view p)
{
    if (p.empty())
        return false;
    if (isSep(p[0]))
        return true;
    return hasDrivePrefix(p) && p.size() >= 3 && isSep(p[2]);
}

std::string normalize(std::string_view p)
{
    if (p.empty())
        return {};

    std::string prefix;
    size_t i = 0;
    if (hasDrivePrefix(p)) {
        prefix.assign(p.substr(0, 2));
        i = 2;
    }
    const bool absolute = i < p.size() && isSep(p[i]);
    // Preserve UNC prefix ("//server/share").
    const bool unc = prefix.empty() && p.size() > 1 && isSep(p[0]) && isSep(p[1]);
    if (absolute)
        prefix += unc ? "//" : "/";

    std::vector<std::string_view> parts;
    while (i < p.size()) {
        while (i < p.size() && isSep(p[i]))
            ++i;
        size_t start = i;
        while (i < p.size() && !isSep(p[i]))
            ++i;
        std::string_view part = p.substr(start, i - start);
        if (part.empty() || part == ".")
            continue;
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..")
                parts.pop_back();
            else if (!absolute)
                parts.push_back(part);
            continue;
        }
        parts.push_back(part);
    }

    std::string out = prefix;
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k > 0)
            out += '/';
        out.append(parts[k]);
    }
    if (out.empty())
        out = ".";
    return out;
}

std::string join(std::string_view a, std::string_view b)
{
    if (a.empty() || isAbsolute(b))
        return normalize(b);
    if (b.empty())
        return normalize(a);
    std::string s(a);
    if (!isSep(s.back()))
        s += '/';
    s.append(b);
    return normalize(s);
}

std::string parent(std::string_view p)
{
    std::string n = normalize(p);
    size_t pos = n.find_last_of('/');
    if (pos == std::string::npos)
        return hasDrivePrefix(n) ? n.substr(0, 2) + "/" : ".";
    if (pos == 0)
        return "/";
    if (pos == 2 && hasDrivePrefix(n))
        return n.substr(0, 3);
    return n.substr(0, pos);
}

std::string filename(std::string_view p)
{
    size_t pos = p.find_last_of("/\\");
    return std::string(pos == std::string_view::npos ? p : p.substr(pos + 1));
}

std::string extension(std::string_view p)
{
    std::string name = filename(p);
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0)
        return {};
    return detail::toLowerAscii(name.substr(dot));
}

} // namespace enp::os::fs::path
