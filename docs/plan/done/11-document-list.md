# 11 · One document list

**Problem.** The explorer's Open tab, the tab overflow list and the tab context menu each draw and
handle open documents in their own way.

**Design.** `DocumentListView`: one component drawing document rows (icon, name, folder hint,
unsaved dot or close button, active highlight, tooltip, middle-click close, drag reorder, context
menu) with a filter, used by the explorer Open tab and the overflow popup; one shared context menu
for tabs and rows.

**Tests.** UI sweep in both places; behaviour unchanged.
