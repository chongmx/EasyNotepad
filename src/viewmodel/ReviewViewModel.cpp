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
    if (live_) {
        const std::string file = activeFile_ ? activeFile_() : std::string();
        auto substitute = [&](std::string command) {
            for (const auto& [key, value] : {std::pair<std::string, std::string>{"{file}", file}, {"{workspace}", workspace_}})
                for (size_t at = command.find(key); at != std::string::npos; at = command.find(key, at + value.size()))
                    command.replace(at, key.size(), "\"" + value + "\"");
            return command;
        };
        if (tool == model::ReviewTool::AgentReview) {
            if (!agent_ || !agent_->available()) {
                replaceFindings(tool, {}, "Agent Review needs an agent CLI (Claude Code's `claude`, or EASYNOTEPAD_AGENT_CLI)");
                return;
            }
            if (file.empty()) {
                statusMessage.emit("Agent Review: open a file to review");
                return;
            }
            running_[size_t(tool)] = true;
            statusMessage.emit("Agent Review: asking the agent about " + file + "...");
            const std::string prompt = "Review the file " + file + " for bugs, risky code and unclear parts. Answer ONLY with "
                                       "findings, one per line, in exactly this format:\n<path>:<line>: <error|warning|info>: "
                                       "<message>\nNo other text. If there is nothing to report, answer with nothing.";
            agent_->run(prompt, workspace_, [this, tool](bool ok, const std::string& output) {
                running_[size_t(tool)] = false;
                replaceFindings(tool, ok ? model::parseToolOutput(output, tool, workspace_) : std::vector<model::ReviewFinding>{},
                                ok ? std::string() : "Agent Review failed: " + output.substr(0, 200));
            });
            return;
        }
        const std::optional<std::string> command = processes_ ? processes_->setting(settingName(tool)) : std::nullopt;
        if (!command) {
            replaceFindings(tool, {}, std::string(model::reviewToolName(tool)) + " is not configured: set " + settingName(tool) +
                                          " to a command (e.g. \"clang-tidy {file}\" or \"ruff check {file}\")");
            return;
        }
        running_[size_t(tool)] = true;
        statusMessage.emit(std::string(model::reviewToolName(tool)) + ": running " + substitute(*command) + "...");
        processes_->run(substitute(*command), workspace_, [this, tool](const interfaces::ProcessResult& result) {
            running_[size_t(tool)] = false;
            if (!result.started) {
                replaceFindings(tool, {}, std::string(model::reviewToolName(tool)) + " could not run: " + result.error);
                return;
            }
            replaceFindings(tool, model::parseToolOutput(result.output, tool, workspace_), {});
        });
        return;
    }
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

const char* ReviewViewModel::settingName(model::ReviewTool tool)
{
    switch (tool) {
    case model::ReviewTool::Lint: return "EASYNOTEPAD_LINT";
    case model::ReviewTool::StaticAnalysis: return "EASYNOTEPAD_STATIC_ANALYSIS";
    case model::ReviewTool::Security: return "EASYNOTEPAD_SECURITY";
    case model::ReviewTool::Style: return "EASYNOTEPAD_STYLE";
    case model::ReviewTool::Complexity: return "EASYNOTEPAD_COMPLEXITY";
    case model::ReviewTool::AgentReview: return "EASYNOTEPAD_AGENT_CLI";
    }
    return "";
}

void ReviewViewModel::goLive(interfaces::IProcessService* processes, interfaces::IAgentCliService* agent, const std::string& workspace,
                             std::function<std::string()> activeFile)
{
    live_ = true;
    processes_ = processes;
    agent_ = agent;
    workspace_ = workspace;
    activeFile_ = std::move(activeFile);
    findings_.clear();
    comments_.clear();
    checklist_.clear();
    hasRun_.fill(false);
    running_.fill(false);
    selectedFinding.set(-1);
}

void ReviewViewModel::replaceFindings(model::ReviewTool tool, std::vector<model::ReviewFinding> fresh, const std::string& note)
{
    findings_.erase(std::remove_if(findings_.begin(), findings_.end(), [&](const model::ReviewFinding& f) { return f.tool == tool; }),
                    findings_.end());
    const size_t count = fresh.size();
    findings_.insert(findings_.end(), std::make_move_iterator(fresh.begin()), std::make_move_iterator(fresh.end()));
    hasRun_[size_t(tool)] = note.empty();
    toolVisible_[size_t(tool)] = true;
    selectedFinding.set(-1);
    statusMessage.emit(note.empty() ? std::string(model::reviewToolName(tool)) + ": " + std::to_string(count) + " finding(s)" : note);
}

} // namespace enp::viewmodel
