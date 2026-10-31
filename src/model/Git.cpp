#include "enp/model/Git.h"

#include <algorithm>
#include <cstdlib>
#include <map>

namespace enp::model::git {

namespace {

std::vector<std::string_view> split(std::string_view text, char separator)
{
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find(separator, start);
        if (end == std::string_view::npos) {
            if (start < text.size())
                parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

int toInt(std::string_view s)
{
    int value = 0;
    bool negative = false;
    size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-'))
        negative = s[i++] == '-';
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i)
        value = value * 10 + (s[i] - '0');
    return negative ? -value : value;
}

int64_t toInt64(std::string_view s)
{
    int64_t value = 0;
    for (char c : s)
        if (c >= '0' && c <= '9')
            value = value * 10 + (c - '0');
    return value;
}

// The word after `n` spaces-separated fields: porcelain v2 lines end with a path that may
// contain spaces, so it is everything after the fixed fields.
std::string_view afterFields(std::string_view line, int fields)
{
    size_t pos = 0;
    for (int i = 0; i < fields; ++i) {
        pos = line.find(' ', pos);
        if (pos == std::string_view::npos)
            return {};
        ++pos;
    }
    return line.substr(pos);
}

char simpleStatus(char c)
{
    switch (c) {
    case 'A': return 'A';
    case 'C': return 'A';
    case 'D': return 'D';
    case 'R': return 'R';
    default: return 'M'; // M, T
    }
}

} // namespace

std::vector<std::string> lines(std::string_view text, char separator)
{
    std::vector<std::string> out;
    for (std::string_view part : split(text, separator)) {
        if (separator == '\n' && !part.empty() && part.back() == '\r')
            part.remove_suffix(1);
        out.emplace_back(part);
    }
    return out;
}

// ---- Status -------------------------------------------------------------------------------------

Status parseStatus(std::string_view text)
{
    Status status;
    const std::vector<std::string_view> records = split(text, '\0');
    for (size_t i = 0; i < records.size(); ++i) {
        const std::string_view r = records[i];
        if (r.size() < 2)
            continue;
        if (r[0] == '#') {
            if (r.rfind("# branch.oid ", 0) == 0) {
                const std::string_view oid = r.substr(13);
                status.initial = oid == "(initial)";
                status.head = std::string(oid);
            } else if (r.rfind("# branch.head ", 0) == 0) {
                const std::string_view head = r.substr(14);
                status.detached = head == "(detached)";
                status.branch = status.detached ? status.head.substr(0, 7) : std::string(head);
            } else if (r.rfind("# branch.upstream ", 0) == 0) {
                status.upstream = std::string(r.substr(18));
            } else if (r.rfind("# branch.ab ", 0) == 0) {
                const std::vector<std::string_view> ab = split(r.substr(12), ' ');
                if (ab.size() == 2) {
                    status.ahead = toInt(ab[0]);
                    status.behind = -toInt(ab[1]);
                }
            }
            continue;
        }
        if (r[0] == '?') {
            status.changes.push_back({std::string(r.substr(2)), 'U', Area::Untracked, {}, {}});
            continue;
        }
        if (r[0] == 'u') {
            status.changes.push_back({std::string(afterFields(r, 10)), '!', Area::Conflicts, {}, {}});
            continue;
        }
        if (r[0] != '1' && r[0] != '2')
            continue;
        const char x = r[2], y = r[3];
        std::string path(afterFields(r, r[0] == '1' ? 8 : 9));
        std::string orig;
        if (r[0] == '2' && i + 1 < records.size())
            orig = std::string(records[++i]);
        // <sub> is "N..." for a file, "S<c><m><u>" for a submodule: new commit, modified, untracked.
        std::string submodule;
        if (r.size() > 8 && r[5] == 'S') {
            auto add = [&](const char* what) { submodule += (submodule.empty() ? "" : ", ") + std::string(what); };
            if (r[6] == 'C')
                add("new commits");
            if (r[7] == 'M')
                add("modified files");
            if (r[8] == 'U')
                add("untracked files");
            if (submodule.empty())
                submodule = "submodule";
        }
        if (x != '.')
            status.changes.push_back({path, simpleStatus(x), Area::Staged, orig, submodule.empty() ? std::string() : "new commit staged"});
        if (y != '.')
            status.changes.push_back({path, simpleStatus(y), Area::Changes, {}, submodule});
    }
    return status;
}

// ---- Branches -----------------------------------------------------------------------------------

const char* branchFormat()
{
    return "%(refname)%00%(refname:short)%00%(upstream:short)%00%(upstream:track,nobracket)%00%(HEAD)%00%(creatordate:unix)";
}

std::vector<Branch> parseBranches(std::string_view text)
{
    std::vector<Branch> branches;
    for (std::string_view line : split(text, '\n')) {
        const std::vector<std::string_view> f = split(line, '\0');
        if (f.size() < 6)
            continue;
        Branch b;
        if (f[0].rfind("refs/heads/", 0) == 0)
            b.kind = RefKind::Local;
        else if (f[0].rfind("refs/remotes/", 0) == 0) {
            b.kind = RefKind::Remote;
            if (f[0].size() >= 5 && f[0].substr(f[0].size() - 5) == "/HEAD")
                continue; // origin/HEAD is an alias
        } else if (f[0].rfind("refs/tags/", 0) == 0)
            b.kind = RefKind::Tag;
        else
            continue;
        b.name = std::string(f[1]);
        b.upstream = std::string(f[2]);
        for (std::string_view part : split(f[3], ',')) {
            while (!part.empty() && part.front() == ' ')
                part.remove_prefix(1);
            if (part.rfind("ahead ", 0) == 0)
                b.ahead = toInt(part.substr(6));
            else if (part.rfind("behind ", 0) == 0)
                b.behind = toInt(part.substr(7));
            else if (part == "gone")
                b.gone = true;
        }
        b.current = f[4] == "*";
        b.lastUsed = toInt64(f[5]);
        branches.push_back(std::move(b));
    }
    return branches;
}

// ---- Changed files ------------------------------------------------------------------------------

std::vector<FileChange> parseFileChanges(std::string_view nameStatus, std::string_view numstat)
{
    std::vector<FileChange> files;
    const std::vector<std::string_view> ns = split(nameStatus, '\0');
    for (size_t i = 0; i < ns.size(); ++i) {
        if (ns[i].empty())
            continue;
        const char kind = ns[i][0];
        if ((kind == 'R' || kind == 'C') && i + 2 < ns.size()) {
            files.push_back({std::string(ns[i + 2]), kind == 'R' ? 'R' : 'A', 0, 0});
            i += 2;
        } else if (i + 1 < ns.size()) {
            files.push_back({std::string(ns[i + 1]), simpleStatus(kind), 0, 0});
            ++i;
        }
    }
    // numstat: "added\tremoved\tpath\0", or for a rename "added\tremoved\t\0old\0new\0".
    const std::vector<std::string_view> nums = split(numstat, '\0');
    for (size_t i = 0; i < nums.size(); ++i) {
        const std::vector<std::string_view> f = split(nums[i], '\t');
        if (f.size() < 2)
            continue;
        std::string path;
        if (f.size() >= 3 && !f[2].empty())
            path = std::string(f[2]);
        else if (i + 2 < nums.size()) {
            path = std::string(nums[i + 2]);
            i += 2;
        }
        for (FileChange& file : files)
            if (file.path == path) {
                file.added = f[0] == "-" ? 0 : toInt(f[0]);
                file.removed = f[1] == "-" ? 0 : toInt(f[1]);
            }
    }
    return files;
}

// ---- Log ----------------------------------------------------------------------------------------

const char* logFormat()
{
    return "%H%x1f%h%x1f%P%x1f%an%x1f%at%x1f%D%x1f%s%x1e";
}

std::vector<Commit> parseLog(std::string_view text)
{
    std::vector<Commit> commits;
    for (std::string_view record : split(text, '\x1e')) {
        while (!record.empty() && (record.front() == '\n' || record.front() == '\r'))
            record.remove_prefix(1);
        const std::vector<std::string_view> f = split(record, '\x1f');
        if (f.size() < 6)
            continue;
        Commit c;
        c.hash = std::string(f[0]);
        c.shortHash = std::string(f[1]);
        for (std::string_view p : split(f[2], ' '))
            if (!p.empty())
                c.parents.emplace_back(p);
        c.author = std::string(f[3]);
        c.time = toInt64(f[4]);
        c.refs = std::string(f[5]);
        c.message = f.size() > 6 ? std::string(f[6]) : std::string();
        commits.push_back(std::move(c));
    }
    return commits;
}

void layoutGraph(std::vector<Commit>& commits, int maxLanes)
{
    // Each lane holds the commit it is waiting for; a commit takes the leftmost lane waiting for
    // it (others waiting for it merge into it), and passes its lane on to its first parent.
    std::vector<std::string> lanes;
    auto find = [](const std::vector<std::string>& v, const std::string& hash) {
        const auto it = std::find(v.begin(), v.end(), hash);
        return it == v.end() ? -1 : int(it - v.begin());
    };
    auto freeLane = [](std::vector<std::string>& v) {
        const auto it = std::find(v.begin(), v.end(), std::string());
        if (it != v.end())
            return int(it - v.begin());
        v.emplace_back();
        return int(v.size()) - 1;
    };
    for (size_t r = 0; r < commits.size(); ++r) {
        Commit& c = commits[r];
        int lane = find(lanes, c.hash);
        if (lane < 0)
            lane = freeLane(lanes);
        c.lane = std::min(lane, maxLanes - 1);
        std::vector<std::string> next = lanes;
        for (std::string& h : next)
            if (h == c.hash)
                h.clear();
        std::vector<int> parentLanes;
        if (!c.parents.empty()) {
            next[size_t(lane)] = c.parents[0];
            parentLanes.push_back(lane);
        }
        for (size_t p = 1; p < c.parents.size(); ++p) {
            int j = find(next, c.parents[p]);
            if (j < 0) {
                j = freeLane(next);
                next[size_t(j)] = c.parents[p];
            }
            parentLanes.push_back(j);
        }
        // Edges to the next row: parents leave from this commit's dot; other lanes pass straight
        // down, except into the next commit's dot when they wait for it.
        c.edges.clear();
        if (r + 1 < commits.size()) {
            const std::string& nextHash = commits[r + 1].hash;
            const int nextLane = find(next, nextHash);
            for (int i = 0; i < int(next.size()); ++i) {
                if (next[size_t(i)].empty())
                    continue;
                const bool fromDot = std::find(parentLanes.begin(), parentLanes.end(), i) != parentLanes.end();
                const int from = fromDot ? lane : i;
                const int to = next[size_t(i)] == nextHash && nextLane >= 0 ? nextLane : i;
                const std::pair<int, int> edge(std::min(from, maxLanes - 1), std::min(to, maxLanes - 1));
                if (std::find(c.edges.begin(), c.edges.end(), edge) == c.edges.end())
                    c.edges.push_back(edge);
            }
        }
        while (!next.empty() && next.back().empty())
            next.pop_back();
        lanes = std::move(next);
    }
}

// ---- Stashes ------------------------------------------------------------------------------------

std::vector<Stash> parseStashList(std::string_view text)
{
    std::vector<Stash> stashes;
    for (std::string_view line : split(text, '\n')) {
        const std::vector<std::string_view> f = split(line, '\x1f');
        if (f.size() < 2)
            continue;
        Stash s;
        s.ref = std::string(f[0]);
        std::string_view subject = f[1];
        // "WIP on main: 1a2b3c4 subject" or "On main: the name given"
        const bool wip = subject.rfind("WIP on ", 0) == 0;
        if (wip || subject.rfind("On ", 0) == 0) {
            const size_t start = wip ? 7 : 3;
            const size_t colon = subject.find(": ", start);
            s.branch = std::string(subject.substr(start, colon == std::string_view::npos ? std::string_view::npos : colon - start));
            s.message = wip || colon == std::string_view::npos ? "WIP on " + s.branch : std::string(subject.substr(colon + 2));
        } else
            s.message = std::string(subject);
        s.time = f.size() > 2 ? toInt64(f[2]) : 0;
        stashes.push_back(std::move(s));
    }
    return stashes;
}

// ---- Blame --------------------------------------------------------------------------------------

std::vector<BlameLine> parseBlame(std::string_view text)
{
    std::map<std::string, BlameLine> commits;
    std::vector<std::pair<int, std::string>> order; // final line -> hash
    std::string current;
    int finalLine = 0;
    for (std::string_view line : split(text, '\n')) {
        if (line.empty())
            continue;
        if (line[0] == '\t') {
            order.emplace_back(finalLine, current);
            continue;
        }
        const size_t space = line.find(' ');
        const std::string_view key = line.substr(0, space);
        const std::string_view value = space == std::string_view::npos ? std::string_view() : line.substr(space + 1);
        if (key.size() == 40 && key.find_first_not_of("0123456789abcdef") == std::string_view::npos) {
            current = std::string(key);
            const std::vector<std::string_view> f = split(value, ' ');
            finalLine = f.size() >= 2 ? toInt(f[1]) : finalLine + 1;
            commits[current].hash = current;
        } else if (key == "author")
            commits[current].author = std::string(value);
        else if (key == "author-time")
            commits[current].time = toInt64(value);
        else if (key == "summary")
            commits[current].summary = std::string(value);
    }
    std::sort(order.begin(), order.end());
    std::vector<BlameLine> out;
    for (const auto& [lineNumber, hash] : order)
        out.push_back(commits[hash]);
    return out;
}

// ---- Small helpers ------------------------------------------------------------------------------

std::string relativeTime(int64_t time, int64_t now)
{
    const int64_t d = std::max<int64_t>(0, now - time);
    auto ago = [](int64_t n, const char* unit) {
        return std::to_string(n) + " " + unit + (n == 1 ? "" : "s") + " ago";
    };
    if (d < 60)
        return "just now";
    if (d < 3600)
        return ago(d / 60, "minute");
    if (d < 86400)
        return ago(d / 3600, "hour");
    if (d < 2 * 86400)
        return "yesterday";
    if (d < 7 * 86400)
        return ago(d / 86400, "day");
    if (d < 30 * 86400)
        return ago(d / (7 * 86400), "week");
    if (d < 365 * 86400)
        return ago(d / (30 * 86400), "month");
    return ago(d / (365 * 86400), "year");
}

bool validBranchName(std::string_view name)
{
    if (name.empty() || name == "@" || name.front() == '-' || name.front() == '/' || name.back() == '/' || name.back() == '.')
        return false;
    if (name.size() >= 5 && name.substr(name.size() - 5) == ".lock")
        return false;
    if (name.find("..") != std::string_view::npos || name.find("@{") != std::string_view::npos ||
        name.find("//") != std::string_view::npos || name.find("/.") != std::string_view::npos || name.front() == '.')
        return false;
    for (char c : name)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f || c == ' ' || c == '~' || c == '^' || c == ':' || c == '?' ||
            c == '*' || c == '[' || c == '\\')
            return false;
    return true;
}

// ---- Conflicts ----------------------------------------------------------------------------------

std::vector<ConflictBlock> findConflicts(const std::vector<std::string>& text)
{
    auto marker = [](const std::string& line, char c) {
        if (line.size() < 7 || line.compare(0, 7, std::string(7, c)) != 0)
            return false;
        return line.size() == 7 || line[7] == ' ' || (c == '=' && line.find_first_not_of(" \t\r", 7) == std::string::npos);
    };
    std::vector<ConflictBlock> blocks;
    for (int i = 0; i < int(text.size()); ++i) {
        if (!marker(text[size_t(i)], '<'))
            continue;
        ConflictBlock b;
        b.start = i;
        b.current = text[size_t(i)].size() > 8 ? text[size_t(i)].substr(8) : std::string();
        int j = i + 1;
        for (; j < int(text.size()) && !marker(text[size_t(j)], '='); ++j) {
            if (marker(text[size_t(j)], '<'))
                break;
            if (marker(text[size_t(j)], '|') && b.base < 0)
                b.base = j;
        }
        if (j >= int(text.size()) || !marker(text[size_t(j)], '='))
            continue;
        b.middle = j;
        int k = j + 1;
        while (k < int(text.size()) && !marker(text[size_t(k)], '>') && !marker(text[size_t(k)], '<'))
            ++k;
        if (k >= int(text.size()) || !marker(text[size_t(k)], '>'))
            continue;
        b.end = k;
        b.incoming = text[size_t(k)].size() > 8 ? text[size_t(k)].substr(8) : std::string();
        blocks.push_back(b);
        i = k;
    }
    return blocks;
}

std::vector<std::string> resolveConflict(const std::vector<std::string>& text, const ConflictBlock& block, Resolution resolution)
{
    std::vector<std::string> out;
    const int currentEnd = block.base >= 0 ? block.base : block.middle;
    if (resolution != Resolution::Incoming)
        for (int i = block.start + 1; i < currentEnd; ++i)
            out.push_back(text[size_t(i)]);
    if (resolution != Resolution::Current)
        for (int i = block.middle + 1; i < block.end; ++i)
            out.push_back(text[size_t(i)]);
    return out;
}

// ---- Partial staging ----------------------------------------------------------------------------

std::pair<int, int> hunkAt(const std::vector<DiffRow>& rows, int row)
{
    if (row < 0 || row >= int(rows.size()) || rows[size_t(row)].kind == DiffKind::Same)
        return {row, row};
    int first = row, last = row;
    while (first > 0 && rows[size_t(first - 1)].kind != DiffKind::Same)
        --first;
    while (last + 1 < int(rows.size()) && rows[size_t(last + 1)].kind != DiffKind::Same)
        ++last;
    return {first, last};
}

std::string buildPatch(const std::string& path, const std::string& oldText, const std::vector<DiffRow>& rows,
                       const std::vector<bool>& chosen)
{
    // splitLines() gives a text ending in a newline one more, empty, line: that one is not real.
    const std::string eol = oldText.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const bool oldFinal = !oldText.empty() && oldText.back() == '\n';
    int oldReal = 0;
    for (char c : oldText)
        oldReal += c == '\n' ? 1 : 0;
    if (!oldText.empty() && !oldFinal)
        ++oldReal;
    int newLast = -1; // the last real line on the right
    for (const DiffRow& r : rows)
        newLast = std::max(newLast, r.rightLine);
    struct Out {
        char kind;
        std::string text;
        bool oldLast;
    };
    std::vector<Out> out;
    int oldCount = 0, newCount = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const DiffRow& r = rows[i];
        const bool pick = i < chosen.size() && chosen[i];
        const bool left = r.leftLine >= 0 && r.leftLine < oldReal;
        // The right side's trailing empty line is the newline of the line before: skip it.
        const bool right = r.rightLine >= 0 && !(r.rightLine == newLast && r.rightText.empty() && r.rightLine > 0);
        switch (r.kind) {
        case DiffKind::Same:
            if (left)
                out.push_back({' ', r.leftText, r.leftLine == oldReal - 1});
            break;
        case DiffKind::Removed:
            if (left)
                out.push_back({pick ? '-' : ' ', r.leftText, r.leftLine == oldReal - 1});
            break;
        case DiffKind::Added:
            if (right && pick)
                out.push_back({'+', r.rightText, false});
            break;
        case DiffKind::Changed:
            if (left)
                out.push_back({pick ? '-' : ' ', r.leftText, r.leftLine == oldReal - 1});
            if (right && pick)
                out.push_back({'+', r.rightText, false});
            break;
        }
    }
    for (const Out& o : out) {
        oldCount += o.kind != '+' ? 1 : 0;
        newCount += o.kind != '-' ? 1 : 0;
    }
    std::string patch = "diff --git a/" + path + " b/" + path + "\n--- a/" + path + "\n+++ b/" + path + "\n";
    patch += "@@ -" + std::to_string(oldCount ? 1 : 0) + "," + std::to_string(oldCount) + " +" + std::to_string(newCount ? 1 : 0) +
             "," + std::to_string(newCount) + " @@\n";
    for (const Out& o : out) {
        patch += o.kind;
        patch += o.text;
        if (o.oldLast && !oldFinal)
            patch += "\n\\ No newline at end of file\n";
        else
            patch += eol;
    }
    return patch;
}

} // namespace enp::model::git
