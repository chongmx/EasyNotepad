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
};

} // namespace enp::viewmodel
