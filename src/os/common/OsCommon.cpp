#include "../OsInternal.h"

#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace enp::os {

namespace {
std::vector<std::string> g_arguments;
bool g_initialized = false;
} // namespace

bool init(const InitParams& params)
{
    if (g_initialized)
        return true;
    g_arguments.clear();
    g_initialized = detail::platformInit(params, g_arguments);
    return g_initialized;
}

void shutdown()
{
    if (!g_initialized)
        return;
    detail::platformShutdown();
    g_initialized = false;
}

const std::vector<std::string>& arguments()
{
    return g_arguments;
}

double now()
{
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

namespace log {

void write(Level level, const char* fmt, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    detail::writeLogSink(level, buffer);
}

} // namespace log

namespace detail {

std::string toLowerAscii(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}

std::string shellQuote(const std::string& s)
{
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

bool isUrl(std::string_view s)
{
    if (s.rfind("mailto:", 0) == 0)
        return true;
    size_t pos = s.find("://");
    if (pos == std::string_view::npos || pos == 0)
        return false;
    for (size_t i = 0; i < pos; ++i) {
        char c = s[i];
        if (!(std::isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

std::string percentDecode(std::string_view s)
{
    auto hexValue = [](char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hexValue(s[i + 1]);
            int lo = hexValue(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += char(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

} // namespace detail

} // namespace enp::os
