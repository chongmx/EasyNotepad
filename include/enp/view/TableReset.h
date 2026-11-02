// Puts a shared table's columns back to what its TableSetupColumn calls ask for: visibility
// (ImGuiTableColumnFlags_DefaultHide), order, no sort, and widths fitted to the content (or the
// width given). For one table id that shows one document after another.
//
// ImGui::TableResetSettings alone is not enough: it re-initialises only columns created this frame,
// so a column that existed keeps the previous document's width and visibility.
#pragma once

namespace enp::view {

// Call inside BeginTable, after every TableSetupColumn and before the headers or the first row.
void resetTableColumns();

} // namespace enp::view
