// A node of the workspace tree shown in the explorer panel.
#pragma once

#include <string>
#include <vector>

namespace enp::viewmodel {

struct ExplorerItem {
    std::string name;
    std::string path; // workspace-relative, '/' separated
    bool isDirectory = false;
    std::vector<ExplorerItem> children;
    bool scanned = true;    // folders: false until the entries were read (deep folders: when expanded)
    bool truncated = false; // folders: the per-folder limit cut the listing, not every entry is shown
};

} // namespace enp::viewmodel
