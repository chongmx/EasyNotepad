// Code review tools: findings from the analyzers, the diff under review, inline comments
// and the review checklist. Runs on dummy data until the review services exist.
#pragma once

#include "enp/model/Diff.h"
#include "enp/model/Review.h"
#include "enp/viewmodel/Observable.h"

#include <array>
#include <string>
#include <vector>

namespace enp::viewmodel {

class ReviewViewModel {
public:
    ReviewViewModel();

    Property<int> selectedFinding{-1}; // index into findings()
    Property<bool> showResolved{false};

    // Open `file` and put the caret on the 0-based `line`.
    Event<std::string, int> navigateRequested;
    Event<std::string> statusMessage;

    // ---- Findings -----------------------------------------------------------------------
    const std::vector<model::ReviewFinding>& findings() const { return findings_; }
    // Indexes into findings() that pass the tool filter and the resolved filter.
    std::vector<int> visibleFindings() const;
    std::vector<const model::ReviewFinding*> findingsForFile(const std::string& file) const;
    int count(model::Severity severity) const;
    int count(model::ReviewTool tool) const;
    bool hasRun(model::ReviewTool tool) const { return hasRun_[size_t(tool)]; }

    bool toolVisible(model::ReviewTool tool) const { return toolVisible_[size_t(tool)]; }
    void setToolVisible(model::ReviewTool tool, bool visible) { toolVisible_[size_t(tool)] = visible; }

    void runTool(model::ReviewTool tool);
    void runAll();
    void clearFindings();
    void openFinding(int index);
    void nextFinding(int direction); // +1 / -1 through visibleFindings()
    void setResolved(int index, bool resolved);

    // ---- Diff -------------------------------------------------------------------------
    const std::string& diffTitle() const { return diffTitle_; }
    const std::vector<model::DiffRow>& diff() const { return diff_; }
    void setDiff(std::string title, std::vector<model::DiffRow> rows);

    // ---- Comments and checklist ------------------------------------------------------
    const std::vector<model::ReviewComment>& comments() const { return comments_; }
    void addComment(const std::string& file, int line, const std::string& text);
    void setCommentResolved(int index, bool resolved);
    void openComment(int index);
    const std::vector<model::ChecklistItem>& checklist() const { return checklist_; }
    void setChecklistDone(int index, bool done);

private:
    std::vector<model::ReviewFinding> findings_;
    std::array<bool, model::kAllReviewTools.size()> hasRun_{};
    std::array<bool, model::kAllReviewTools.size()> toolVisible_{};
    std::string diffTitle_;
    std::vector<model::DiffRow> diff_;
    std::vector<model::ReviewComment> comments_;
    std::vector<model::ChecklistItem> checklist_;
};

} // namespace enp::viewmodel
