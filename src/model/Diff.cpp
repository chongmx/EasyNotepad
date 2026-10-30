#include "enp/model/Diff.h"

#include "enp/model/TextBuffer.h"

#include <algorithm>

namespace enp::model {

std::vector<std::string> splitLines(const std::string& text)
{
    TextBuffer buffer(text);
    std::vector<std::string> lines;
    lines.reserve(size_t(buffer.lineCount()));
    for (int i = 0; i < buffer.lineCount(); ++i)
        lines.push_back(buffer.line(i));
    return lines;
}

std::vector<DiffRow> diffLines(const std::vector<std::string>& left, const std::vector<std::string>& right)
{
    // Trim the common prefix and suffix so the quadratic table only covers the changed middle.
    size_t prefix = 0;
    while (prefix < left.size() && prefix < right.size() && left[prefix] == right[prefix])
        ++prefix;
    size_t suffix = 0;
    while (suffix < left.size() - prefix && suffix < right.size() - prefix &&
           left[left.size() - 1 - suffix] == right[right.size() - 1 - suffix])
        ++suffix;

    const size_t n = left.size() - prefix - suffix;
    const size_t m = right.size() - prefix - suffix;
    std::vector<std::vector<int>> lcs(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;)
            lcs[i][j] = left[prefix + i] == right[prefix + j] ? lcs[i + 1][j + 1] + 1 : std::max(lcs[i + 1][j], lcs[i][j + 1]);

    std::vector<DiffRow> rows;
    auto same = [&](size_t l, size_t r) { rows.push_back({DiffKind::Same, int(l), int(r), left[l], right[r]}); };
    for (size_t k = 0; k < prefix; ++k)
        same(k, k);

    std::vector<size_t> removed, added;
    auto flush = [&]() {
        const size_t paired = std::min(removed.size(), added.size());
        for (size_t k = 0; k < paired; ++k)
            rows.push_back({DiffKind::Changed, int(removed[k]), int(added[k]), left[removed[k]], right[added[k]]});
        for (size_t k = paired; k < removed.size(); ++k)
            rows.push_back({DiffKind::Removed, int(removed[k]), -1, left[removed[k]], {}});
        for (size_t k = paired; k < added.size(); ++k)
            rows.push_back({DiffKind::Added, -1, int(added[k]), {}, right[added[k]]});
        removed.clear();
        added.clear();
    };

    size_t i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && left[prefix + i] == right[prefix + j]) {
            flush();
            same(prefix + i, prefix + j);
            ++i;
            ++j;
        } else if (j < m && (i == n || lcs[i][j + 1] >= lcs[i + 1][j])) {
            added.push_back(prefix + j++);
        } else {
            removed.push_back(prefix + i++);
        }
    }
    flush();
    for (size_t k = 0; k < suffix; ++k)
        same(left.size() - suffix + k, right.size() - suffix + k);
    return rows;
}

DiffStats diffStats(const std::vector<DiffRow>& rows)
{
    DiffStats stats;
    for (const DiffRow& row : rows) {
        if (row.kind == DiffKind::Added || row.kind == DiffKind::Changed)
            ++stats.added;
        if (row.kind == DiffKind::Removed || row.kind == DiffKind::Changed)
            ++stats.removed;
    }
    return stats;
}

} // namespace enp::model
