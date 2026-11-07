#include "enp/model/viewer/NumberFormat.h"

#include <cctype>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

namespace enp::model::viewer {
namespace {

const char* const kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                               "July",    "August",   "September", "October", "November", "December"};
const char* const kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

std::string lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Splits a code into its sections at ';' outside quotes and brackets.
std::vector<std::string> sections(std::string_view code)
{
    std::vector<std::string> out(1);
    bool quoted = false;
    int bracket = 0;
    for (std::size_t i = 0; i < code.size(); ++i) {
        const char c = code[i];
        if (c == '\\' && i + 1 < code.size()) {
            out.back() += c;
            out.back() += code[++i];
            continue;
        }
        if (c == '"')
            quoted = !quoted;
        else if (!quoted && c == '[')
            ++bracket;
        else if (!quoted && c == ']')
            --bracket;
        if (c == ';' && !quoted && bracket == 0) {
            out.emplace_back();
            continue;
        }
        out.back() += c;
    }
    return out;
}

// "General": up to 11 significant digits, no trailing zeros, exponent for very large or small.
std::string general(double v)
{
    if (v == 0.0)
        return "0";
    if (!std::isfinite(v))
        return "#NUM!";
    const double a = std::fabs(v);
    char buf[64];
    if (a >= 1e11 || a < 1e-9) {
        std::snprintf(buf, sizeof(buf), "%.5E", v);
        std::string s(buf);
        // 1.23450E+12 -> 1.2345E+12
        const std::size_t e = s.find('E');
        std::string mant = s.substr(0, e), exp = s.substr(e);
        while (mant.find('.') != std::string::npos && (mant.back() == '0' || mant.back() == '.')) {
            const bool dot = mant.back() == '.';
            mant.pop_back();
            if (dot)
                break;
        }
        // E+012 style is not used: keep two exponent digits at least.
        return mant + exp;
    }
    const int intDigits = a >= 1.0 ? int(std::floor(std::log10(a))) + 1 : 1;
    const int decimals = std::max(0, 10 - intDigits + (a < 1.0 ? 1 : 0));
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0')
            s.pop_back();
        if (s.back() == '.')
            s.pop_back();
    }
    if (s == "-0")
        s = "0";
    return s;
}

struct Civil {
    int year = 1900, month = 1, day = 1, weekday = 0;
    int hour = 0, minute = 0, second = 0;
    double fraction = 0.0; // of a second
};

// Days from 1970-01-01 to y-m-d (proleptic Gregorian), and back (Howard Hinnant's algorithms).
long long daysFromCivil(long long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civilFromDays(long long z, int& y, int& m, int& d)
{
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long yy = (long long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = int(doy - (153 * mp + 2) / 5 + 1);
    m = int(mp < 10 ? mp + 3 : mp - 9);
    y = int(yy + (m <= 2));
}

Civil civil(double serial, bool date1904)
{
    Civil c;
    double days = std::floor(serial);
    double frac = serial - days;
    long long epochDays;
    if (date1904) {
        epochDays = daysFromCivil(1904, 1, 1) + (long long)days;
    } else {
        // Excel's 1900 system counts a 29 February 1900 that never was: serial 60.
        if (days == 60) {
            c.year = 1900, c.month = 2, c.day = 29, c.weekday = 3;
            epochDays = 0;
        }
        epochDays = daysFromCivil(1899, 12, 31) + (long long)days - (days > 60 ? 1 : 0);
    }
    if (!(days == 60 && !date1904)) {
        civilFromDays(epochDays, c.year, c.month, c.day);
        c.weekday = int(((epochDays % 7) + 11) % 7); // 1970-01-01 was a Thursday (4)
    }
    double secs = frac * 86400.0;
    double whole = std::floor(secs + 1e-6);
    if (whole >= 86400)
        whole = 86399;
    c.fraction = std::max(0.0, secs - whole);
    const int s = int(whole);
    c.hour = s / 3600;
    c.minute = (s / 60) % 60;
    c.second = s % 60;
    return c;
}

std::string two(int v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d", v);
    return buf;
}

// Groups the digits of an integer string with commas.
std::string group(const std::string& digits)
{
    std::string out;
    const int n = int(digits.size());
    for (int i = 0; i < n; ++i) {
        out += digits[size_t(i)];
        const int left = n - i - 1;
        if (left > 0 && left % 3 == 0)
            out += ',';
    }
    return out;
}

std::string formatDate(double v, const std::string& code, bool date1904)
{
    const Civil c = civil(v, date1904);
    const std::string lc = lower(code);
    const bool ampm = lc.find("am/pm") != std::string::npos || lc.find("a/p") != std::string::npos;
    std::string out;
    bool lastWasHour = false;
    for (std::size_t i = 0; i < code.size();) {
        const char ch = char(std::tolower(static_cast<unsigned char>(code[i])));
        if (code[i] == '"') {
            const std::size_t end = code.find('"', i + 1);
            out += code.substr(i + 1, (end == std::string::npos ? code.size() : end) - i - 1);
            i = end == std::string::npos ? code.size() : end + 1;
            continue;
        }
        if (code[i] == '\\' && i + 1 < code.size()) {
            out += code[i + 1];
            i += 2;
            continue;
        }
        if (code[i] == '[') { // [h] elapsed hours, [$-409] locale tags
            const std::size_t end = code.find(']', i);
            const std::string tag = lower(code.substr(i + 1, (end == std::string::npos ? code.size() : end) - i - 1));
            if (tag == "h" || tag == "hh") {
                out += std::to_string(int(std::floor(v * 24.0 + 1e-9)));
                lastWasHour = true; // the m that follows is minutes
            } else if (tag == "mm" || tag == "m") {
                out += std::to_string(int(std::floor(v * 1440.0 + 1e-9)));
            } else if (tag == "ss" || tag == "s") {
                out += std::to_string(int(std::floor(v * 86400.0 + 1e-9)));
            }
            i = end == std::string::npos ? code.size() : end + 1;
            continue;
        }
        std::size_t run = 1;
        while (i + run < code.size() && std::tolower(static_cast<unsigned char>(code[i + run])) == ch)
            ++run;
        if (ch == 'y') {
            out += run <= 2 ? two(c.year % 100) : std::to_string(c.year);
            lastWasHour = false;
        } else if (ch == 'm') {
            // m after h (or before s) is minutes.
            bool minutes = lastWasHour;
            if (!minutes) {
                std::size_t j = i + run;
                while (j < code.size() && (code[j] == ':' || code[j] == ' '))
                    ++j;
                if (j < code.size() && std::tolower(static_cast<unsigned char>(code[j])) == 's')
                    minutes = true;
            }
            if (minutes)
                out += run >= 2 ? two(c.minute) : std::to_string(c.minute);
            else if (run == 1)
                out += std::to_string(c.month);
            else if (run == 2)
                out += two(c.month);
            else if (run == 3)
                out += std::string(kMonths[c.month - 1]).substr(0, 3);
            else if (run == 5)
                out += kMonths[c.month - 1][0];
            else
                out += kMonths[c.month - 1];
            lastWasHour = false;
        } else if (ch == 'd') {
            if (run == 1)
                out += std::to_string(c.day);
            else if (run == 2)
                out += two(c.day);
            else if (run == 3)
                out += std::string(kDays[c.weekday]).substr(0, 3);
            else
                out += kDays[c.weekday];
            lastWasHour = false;
        } else if (ch == 'h') {
            int h = c.hour;
            if (ampm) {
                h %= 12;
                if (h == 0)
                    h = 12;
            }
            out += run >= 2 ? two(h) : std::to_string(h);
            lastWasHour = true;
        } else if (ch == 's') {
            out += run >= 2 ? two(c.second) : std::to_string(c.second);
            // .0, .00: fractions of a second
            if (i + run < code.size() && code[i + run] == '.') {
                std::size_t zeros = 0;
                while (i + run + 1 + zeros < code.size() && code[i + run + 1 + zeros] == '0')
                    ++zeros;
                if (zeros) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%.*f", int(zeros), c.fraction);
                    out += std::string(buf).substr(1);
                    run += 1 + zeros;
                }
            }
            lastWasHour = false;
        } else if (ch == 'a' && (lower(code.substr(i, 5)) == "am/pm")) {
            out += c.hour < 12 ? "AM" : "PM";
            run = 5;
        } else if (ch == 'a' && (lower(code.substr(i, 3)) == "a/p")) {
            out += c.hour < 12 ? "A" : "P";
            run = 3;
        } else {
            if (code[i] != '_' && code[i] != '*')
                out += code.substr(i, run);
            else
                ++run; // _x (a space as wide as x) and *x (fill): the next character goes too
        }
        i += run;
    }
    return out;
}

// A numeric section: literal text, 0 # ? placeholders, '.', ',' (grouping, or scaling at the end), '%', E+.
std::string formatNumeric(double v, const std::string& code)
{
    std::string prefix, suffix;
    int intZeros = 0, intDigitsTotal = 0, decimals = 0, decimalZeros = 0;
    bool grouping = false, percent = false, seenDot = false, seenDigit = false, exponent = false;
    int expDigits = 0;
    int scaleCommas = 0;
    std::string* literal = &prefix;
    for (std::size_t i = 0; i < code.size(); ++i) {
        const char c = code[i];
        if (c == '"') {
            const std::size_t end = code.find('"', i + 1);
            *literal += code.substr(i + 1, (end == std::string::npos ? code.size() : end) - i - 1);
            i = end == std::string::npos ? code.size() : end;
            continue;
        }
        if (c == '\\' && i + 1 < code.size()) {
            *literal += code[++i];
            continue;
        }
        if (c == '[') { // [$€-407], [Red], [>100]
            const std::size_t end = code.find(']', i);
            const std::string tag = code.substr(i + 1, (end == std::string::npos ? code.size() : end) - i - 1);
            if (!tag.empty() && tag[0] == '$') {
                const std::size_t dash = tag.find('-');
                *literal += tag.substr(1, dash == std::string::npos ? std::string::npos : dash - 1);
            }
            i = end == std::string::npos ? code.size() : end;
            continue;
        }
        if (c == '_' || c == '*') {
            if (c == '_')
                *literal += ' ';
            ++i;
            continue;
        }
        if (exponent) {
            if (c == '0' || c == '#')
                ++expDigits;
            else if (c != '+' && c != '-')
                *literal += c;
            continue;
        }
        if (c == '0' || c == '#' || c == '?') {
            seenDigit = true;
            literal = &suffix;
            if (seenDot) {
                ++decimals;
                if (c == '0')
                    ++decimalZeros;
            } else {
                ++intDigitsTotal;
                if (c == '0')
                    ++intZeros;
                scaleCommas = 0;
            }
            continue;
        }
        if (c == '.' && !seenDot) {
            seenDot = true;
            literal = &suffix;
            continue;
        }
        if (c == ',' && seenDigit && !seenDot) {
            // A comma between digits groups; commas after the last digit divide by 1000 each.
            std::size_t j = i + 1;
            if (j < code.size() && (code[j] == '0' || code[j] == '#' || code[j] == '?'))
                grouping = true;
            else
                ++scaleCommas;
            continue;
        }
        if ((c == 'E' || c == 'e') && seenDigit && i + 1 < code.size() && (code[i + 1] == '+' || code[i + 1] == '-')) {
            exponent = true;
            continue;
        }
        if (c == '%')
            percent = true;
        *literal += c;
    }
    (void)intDigitsTotal;
    double a = std::fabs(v);
    if (percent)
        a *= 100.0;
    for (int i = 0; i < scaleCommas; ++i)
        a /= 1000.0;
    std::string body;
    char buf[128];
    if (exponent) {
        int e = a == 0.0 ? 0 : int(std::floor(std::log10(a)));
        double m = a == 0.0 ? 0.0 : a / std::pow(10.0, e);
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, m);
        if (std::string(buf).rfind("10", 0) == 0) { // rounding carried: 9.99 -> 10.0
            ++e;
            std::snprintf(buf, sizeof(buf), "%.*f", decimals, m / 10.0);
        }
        std::string exp = std::to_string(std::abs(e));
        while (int(exp.size()) < std::max(1, expDigits))
            exp = "0" + exp;
        body = std::string(buf) + "E" + (e < 0 ? "-" : "+") + exp;
    } else {
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, a);
        std::string s(buf);
        std::string intPart = s.substr(0, s.find('.'));
        std::string decPart = decimals ? s.substr(s.find('.') + 1) : std::string();
        // Optional decimals (#): trailing zeros beyond the required ones go.
        while (int(decPart.size()) > decimalZeros && !decPart.empty() && decPart.back() == '0')
            decPart.pop_back();
        if (intPart == "0" && intZeros == 0)
            intPart.clear();
        while (int(intPart.size()) < intZeros)
            intPart = "0" + intPart;
        if (grouping)
            intPart = group(intPart);
        body = intPart;
        if (!decPart.empty() || (seenDot && decimalZeros > 0))
            body += "." + decPart;
        else if (seenDot && decimals == 0)
            body += ".";
        if (body.empty())
            body = seenDigit ? "" : "";
    }
    return prefix + body + suffix;
}

// A fraction section ("# ?/?", "# ??/??", "?/4"): the whole part, then the closest fraction with
// as many denominator digits as the code has (or the code's fixed denominator). Empty when the
// section is not a fraction.
std::string formatFraction(double v, const std::string& code)
{
    std::size_t slash = std::string::npos;
    bool quoted = false;
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (code[i] == '"')
            quoted = !quoted;
        else if (!quoted && code[i] == '/')
            slash = i;
    }
    auto placeholder = [](char c) { return c == '0' || c == '#' || c == '?'; };
    if (slash == std::string::npos || slash == 0 || !placeholder(code[slash - 1]))
        return std::string();
    std::size_t numStart = slash;
    while (numStart > 0 && placeholder(code[numStart - 1]))
        --numStart;
    const int numWidth = int(slash - numStart);
    // The denominator: placeholders (digits allowed) or a fixed number.
    std::size_t denEnd = slash + 1;
    bool fixed = true;
    while (denEnd < code.size() && (placeholder(code[denEnd]) || std::isdigit(static_cast<unsigned char>(code[denEnd])))) {
        if (placeholder(code[denEnd]))
            fixed = false;
        ++denEnd;
    }
    const int denWidth = int(denEnd - slash - 1);
    if (denWidth == 0)
        return std::string();
    // The whole part: placeholders before the numerator, apart from it (by a space or text).
    std::size_t wholeEnd = numStart;
    while (wholeEnd > 0 && !placeholder(code[wholeEnd - 1]))
        --wholeEnd;
    std::size_t wholeStart = wholeEnd;
    while (wholeStart > 0 && placeholder(code[wholeStart - 1]))
        --wholeStart;
    const bool whole = wholeEnd > wholeStart;
    auto literal = [](const std::string& part) {
        std::string out;
        for (std::size_t i = 0; i < part.size(); ++i) {
            if (part[i] == '"')
                continue;
            if (part[i] == '\\' && i + 1 < part.size()) {
                out += part[++i];
            } else if (part[i] == '_' || part[i] == '*') {
                if (part[i] == '_')
                    out += ' ';
                ++i;
            } else if (part[i] == '[') {
                const std::size_t end = part.find(']', i);
                i = end == std::string::npos ? part.size() : end;
            } else {
                out += part[i];
            }
        }
        return out;
    };
    const double a = std::fabs(v);
    long long intPart = whole ? (long long)std::floor(a) : 0;
    const double frac = a - double(intPart);
    long long num = 0, den = 1;
    if (fixed) {
        den = std::max(1, std::atoi(code.substr(slash + 1, size_t(denWidth)).c_str()));
        num = std::llround(frac * double(den));
    } else {
        long long maxDen = 1;
        for (int i = 0; i < std::min(denWidth, 4); ++i)
            maxDen *= 10;
        maxDen -= 1;
        double best = 2.0;
        for (long long d = 1; d <= maxDen; ++d) {
            const long long n = std::llround(frac * double(d));
            const double err = std::fabs(frac - double(n) / double(d));
            if (err < best - 1e-12) {
                best = err;
                num = n;
                den = d;
            }
        }
    }
    if (whole && num == den && !fixed) {
        ++intPart;
        num = 0;
    }
    std::string out = literal(code.substr(0, wholeStart));
    const std::string between = literal(code.substr(wholeEnd, numStart - wholeEnd));
    const std::string after = literal(code.substr(denEnd));
    std::string numText = std::to_string(num), denText = std::to_string(den);
    if (whole && num == 0) { // a whole number: the fraction's room left blank
        out += std::to_string(intPart);
        out += std::string(between.size() + size_t(numWidth) + 1 + size_t(denWidth), ' ');
        return out + after;
    }
    if (whole && intPart > 0)
        out += std::to_string(intPart) + between;
    else if (whole)
        out += std::string(size_t(wholeEnd - wholeStart) + between.size(), ' ');
    while (int(numText.size()) < numWidth)
        numText = " " + numText;
    if (!fixed)
        while (int(denText.size()) < denWidth)
            denText += " ";
    return out + numText + "/" + denText + after;
}

// The colour a section's tag names ([Red], [Color10]), or 0.
std::uint32_t sectionColor(const std::string& section)
{
    for (std::size_t i = section.find('['); i != std::string::npos; i = section.find('[', i + 1)) {
        const std::size_t end = section.find(']', i);
        if (end == std::string::npos)
            break;
        const std::string tag = lower(std::string_view(section).substr(i + 1, end - i - 1));
        auto pack = [](unsigned r, unsigned g, unsigned b) { return std::uint32_t(r) | (g << 8) | (b << 16) | 0xFF000000u; };
        if (tag == "black") return pack(0, 0, 0);
        if (tag == "white") return pack(255, 255, 255);
        if (tag == "red") return pack(255, 0, 0);
        if (tag == "green") return pack(0, 255, 0);
        if (tag == "blue") return pack(0, 0, 255);
        if (tag == "yellow") return pack(255, 255, 0);
        if (tag == "magenta") return pack(255, 0, 255);
        if (tag == "cyan") return pack(0, 255, 255);
        if (tag.rfind("color", 0) == 0) {
            static const unsigned kIndexed[8][3] = {{0, 0, 0}, {255, 255, 255}, {255, 0, 0}, {0, 255, 0},
                                                    {0, 0, 255}, {255, 255, 0}, {255, 0, 255}, {0, 255, 255}};
            const int n = std::atoi(tag.c_str() + 5);
            if (n >= 1 && n <= 56) {
                const unsigned* c = kIndexed[(n - 1) % 8];
                return pack(c[0], c[1], c[2]);
            }
        }
    }
    return 0;
}

bool hasNumberPlaceholders(const std::string& code)
{
    bool quoted = false;
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (code[i] == '"')
            quoted = !quoted;
        else if (!quoted && code[i] == '\\')
            ++i;
        else if (!quoted && (code[i] == '0' || code[i] == '#' || code[i] == '?'))
            return true;
    }
    return false;
}

} // namespace

std::string_view builtinNumberFormat(int id)
{
    switch (id) {
    case 0: return "General";
    case 1: return "0";
    case 2: return "0.00";
    case 3: return "#,##0";
    case 4: return "#,##0.00";
    case 5: return "$#,##0_);($#,##0)";
    case 6: return "$#,##0_);[Red]($#,##0)";
    case 7: return "$#,##0.00_);($#,##0.00)";
    case 8: return "$#,##0.00_);[Red]($#,##0.00)";
    case 9: return "0%";
    case 10: return "0.00%";
    case 11: return "0.00E+00";
    case 12: return "# ?/?";
    case 13: return "# ?\?/?\?"; // "??/" would be a trigraph
    case 14: return "m/d/yyyy";
    case 15: return "d-mmm-yy";
    case 16: return "d-mmm";
    case 17: return "mmm-yy";
    case 18: return "h:mm AM/PM";
    case 19: return "h:mm:ss AM/PM";
    case 20: return "h:mm";
    case 21: return "h:mm:ss";
    case 22: return "m/d/yyyy h:mm";
    case 37: return "#,##0_);(#,##0)";
    case 38: return "#,##0_);[Red](#,##0)";
    case 39: return "#,##0.00_);(#,##0.00)";
    case 40: return "#,##0.00_);[Red](#,##0.00)";
    case 45: return "mm:ss";
    case 46: return "[h]:mm:ss";
    case 47: return "mm:ss.0";
    case 48: return "##0.0E+0";
    case 49: return "@";
    default: return {};
    }
}

bool isDateFormat(std::string_view code)
{
    bool quoted = false;
    for (std::size_t i = 0; i < code.size(); ++i) {
        const char c = char(std::tolower(static_cast<unsigned char>(code[i])));
        if (code[i] == '"') {
            quoted = !quoted;
            continue;
        }
        if (quoted)
            continue;
        if (code[i] == '\\') {
            ++i;
            continue;
        }
        if (code[i] == '[') { // [h] [mm] [ss] are elapsed time; [Red] [$-409] are not dates
            const std::size_t end = code.find(']', i);
            const std::string tag = lower(code.substr(i + 1, end == std::string_view::npos ? 0 : end - i - 1));
            if (tag == "h" || tag == "hh" || tag == "m" || tag == "mm" || tag == "s" || tag == "ss")
                return true;
            i = end == std::string_view::npos ? code.size() : end;
            continue;
        }
        if (c == 'y' || c == 'd' || c == 'h' || c == 's' || c == 'm')
            return true;
    }
    return false;
}

std::string formatNumber(double value, std::string_view codeIn, bool date1904, std::uint32_t* color)
{
    const std::string code(codeIn);
    if (code.empty() || lower(code) == "general")
        return general(value);
    const std::vector<std::string> parts = sections(code);
    std::string section = parts[0];
    bool absolute = false; // a negative section shows the magnitude (its own signs and brackets)
    if (value < 0 && parts.size() >= 2 && !parts[1].empty()) {
        section = parts[1];
        absolute = true;
    } else if (value == 0 && parts.size() >= 3 && !parts[2].empty()) {
        section = parts[2];
    }
    if (color)
        *color = sectionColor(section);
    if (lower(section) == "general")
        return absolute ? general(std::fabs(value)) : general(value);
    if (section == "@")
        return general(value);
    if (isDateFormat(section))
        return value < 0 ? "#######" : formatDate(value, section, date1904);
    if (!hasNumberPlaceholders(section)) { // only text ("-" for zero, "Total")
        std::string out;
        for (std::size_t i = 0; i < section.size(); ++i) {
            if (section[i] == '"')
                continue;
            if (section[i] == '\\' && i + 1 < section.size()) {
                out += section[++i];
                continue;
            }
            if (section[i] == '_' || section[i] == '*') {
                ++i;
                if (section[i - 1] == '_')
                    out += ' ';
                continue;
            }
            if (section[i] == '[') {
                const std::size_t end = section.find(']', i);
                i = end == std::string::npos ? section.size() : end;
                continue;
            }
            out += section[i];
        }
        return out;
    }
    std::string out = formatFraction(value, section);
    if (out.empty())
        out = formatNumeric(value, section);
    // The sign: only when the section is the positive one used for a negative value (and the number
    // does not round to zero).
    if (value < 0 && !absolute) {
        bool nonZero = false;
        for (char c : out)
            if (c >= '1' && c <= '9')
                nonZero = true;
        if (nonZero)
            out = "-" + out;
    }
    return out;
}

} // namespace enp::model::viewer
