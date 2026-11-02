#include "enp/view/TableReset.h"

#include "imgui.h"
#include "imgui_internal.h"

namespace enp::view {

void resetTableColumns()
{
    ImGuiTable* table = ImGui::GetCurrentTable();
    if (!table)
        return;
    for (int n = 0; n < table->ColumnsCount; ++n) {
        ImGuiTableColumn& c = table->Columns[n];
        c.IsUserEnabled = c.IsUserEnabledNextFrame = (c.Flags & ImGuiTableColumnFlags_DefaultHide) ? 0 : 1;
        c.DisplayOrder = ImGuiTableColumnIdx(n);
        table->DisplayOrderToIndex[n] = ImGuiTableColumnIdx(n);
        c.SortOrder = -1;
        const bool given = c.InitStretchWeightOrWidth > 0.0f;
        c.WidthRequest = (c.Flags & ImGuiTableColumnFlags_WidthFixed) && given ? c.InitStretchWeightOrWidth : -1.0f;
        c.StretchWeight = (c.Flags & ImGuiTableColumnFlags_WidthStretch) && given ? c.InitStretchWeightOrWidth : -1.0f;
        c.AutoFitQueue = given ? 0 : (1 << 3) - 1; // fitted over the next frames, as when it was created
        c.CannotSkipItemsQueue = (1 << 3) - 1;
    }
    table->IsSortSpecsDirty = true;
    table->IsSettingsDirty = true;
}

} // namespace enp::view
