// Line diff for side-by-side comparison (compare-with-saved, agent changes).
#pragma once

#include <string>
#include <vector>

namespace enp::model {

enum class DiffKind { Same, Added, Removed, Changed };

// One side-by-side row. A line number of -1 means that side is empty on this row.
struct DiffRow {
    DiffKind kind = DiffKind::Same;
    int leftLine = -1;
    int rightLine = -1;
    std::string leftText;
    std::string rightText;
};

struct DiffStats {
    int added = 0;
    int removed = 0;
};

// LCS-based line diff. Adjacent removed/added runs are paired into Changed rows.
std::vector<DiffRow> diffLines(const std::vector<std::string>& left, const std::vector<std::string>& right);
std::vector<std::string> splitLines(const std::string& text);
DiffStats diffStats(const std::vector<DiffRow>& rows);

} // namespace enp::model
