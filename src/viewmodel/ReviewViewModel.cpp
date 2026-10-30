#include "enp/viewmodel/ReviewViewModel.h"

#include "enp/viewmodel/DummyData.h"

#include <algorithm>

namespace enp::viewmodel {

ReviewViewModel::ReviewViewModel()
    : comments_(dummy::comments()), checklist_(dummy::checklist())
{
    toolVisible_.fill(true);
    for (model::ReviewTool tool : model::kAllReviewTools) {
        std::vector<model::ReviewFinding> f = dummy::findings(tool);
        findings_.insert(findings_.end(), f.begin(), f.end());
        hasRun_[size_t(tool)] = true;
    }
}

std::vector<int> ReviewViewModel::visibleFindings() const
{
    std::vector<int> out;
    for (int i = 0; i < int(findings_.size()); ++i) {
        const model::ReviewFinding& f = findings_[size_t(i)];
        if (toolVisible(f.tool) && (showResolved.get() || !f.resolved))
            out.push_back(i);
    }
    // Errors first, then by file and line.
    std::stable_sort(out.begin(), out.end(), [this](int a, int b) {
        const model::ReviewFinding& fa = findings_[size_t(a)];
        const model::ReviewFinding& fb = findings_[size_t(b)];
        if (fa.severity != fb.severity)
            return fa.severity > fb.severity;
        if (fa.file != fb.file)
            return fa.file < fb.file;
        return fa.line < fb.line;
    });
    return out;
}

std::vector<const model::ReviewFinding*> ReviewViewModel::findingsForFile(const std::string& file) const
{
    std::vector<const model::ReviewFinding*> out;
    for (const model::ReviewFinding& f : findings_)
        if (f.file == file && !f.resolved && toolVisible(f.tool))
            out.push_back(&f);
    return out;
}

int ReviewViewModel::count(model::Severity severity) const
{
    return int(std::count_if(findings_.begin(), findings_.end(), [&](const model::ReviewFinding& f) {
        return f.severity == severity && !f.resolved;
    }));
}

int ReviewViewModel::count(model::ReviewTool tool) const
{
    return int(std::count_if(findings_.begin(), findings_.end(), [&](const model::ReviewFinding& f) {
        return f.tool == tool && !f.resolved;
    }));
}

void ReviewViewModel::runTool(model::ReviewTool tool)
{
    findings_.erase(std::remove_if(findings_.begin(), findings_.end(),
                                   [&](const model::ReviewFinding& f) { return f.tool == tool; }),
                    findings_.end());
    std::vector<model::ReviewFinding> fresh = dummy::findings(tool);
    findings_.insert(findings_.end(), fresh.begin(), fresh.end());
    hasRun_[size_t(tool)] = true;
    toolVisible_[size_t(tool)] = true;
    selectedFinding.set(-1);
    statusMessage.emit(std::string(model::reviewToolName(tool)) + ": " + std::to_string(fresh.size()) +
                       " finding(s) (dummy results)");
}

void ReviewViewModel::runAll()
{
    for (model::ReviewTool tool : model::kAllReviewTools)
        runTool(tool);
    statusMessage.emit("All review tools finished: " + std::to_string(findings_.size()) + " finding(s) (dummy results)");
}

void ReviewViewModel::clearFindings()
{
    findings_.clear();
    hasRun_.fill(false);
    selectedFinding.set(-1);
    statusMessage.emit("Cleared all findings");
}

void ReviewViewModel::openFinding(int index)
{
    if (index < 0 || index >= int(findings_.size()))
        return;
    selectedFinding.set(index);
    const model::ReviewFinding& f = findings_[size_t(index)];
    navigateRequested.emit(f.file, f.line);
    statusMessage.emit(std::string(model::reviewToolName(f.tool)) + " " + model::severityName(f.severity) + ": " +
                       f.message);
}

void ReviewViewModel::nextFinding(int direction)
{
    const std::vector<int> visible = visibleFindings();
    if (visible.empty())
        return;
    auto it = std::find(visible.begin(), visible.end(), selectedFinding.get());
    int pos = it == visible.end() ? (direction > 0 ? -1 : 0) : int(it - visible.begin());
    pos = (pos + direction + int(visible.size())) % int(visible.size());
    openFinding(visible[size_t(pos)]);
}

void ReviewViewModel::setResolved(int index, bool resolved)
{
    if (index >= 0 && index < int(findings_.size()))
        findings_[size_t(index)].resolved = resolved;
}

void ReviewViewModel::setDiff(std::string title, std::vector<model::DiffRow> rows)
{
    diffTitle_ = std::move(title);
    diff_ = std::move(rows);
}

void ReviewViewModel::addComment(const std::string& file, int line, const std::string& text)
{
    if (text.empty())
        return;
    comments_.push_back({"You", file, line, text, false});
    statusMessage.emit("Added a review comment on " + file + ":" + std::to_string(line + 1));
}

void ReviewViewModel::setCommentResolved(int index, bool resolved)
{
    if (index >= 0 && index < int(comments_.size()))
        comments_[size_t(index)].resolved = resolved;
}

void ReviewViewModel::openComment(int index)
{
    if (index >= 0 && index < int(comments_.size()))
        navigateRequested.emit(comments_[size_t(index)].file, comments_[size_t(index)].line);
}

void ReviewViewModel::setChecklistDone(int index, bool done)
{
    if (index >= 0 && index < int(checklist_.size()))
        checklist_[size_t(index)].done = done;
}

} // namespace enp::viewmodel
