#include "enp/view/AgentPanel.h"

#include "enp/view/DiffView.h"
#include "enp/view/Icons.h"
#include "enp/view/UiAutomation.h"

#include "enp/os/Os.h"

#include "IconsFontAwesome6.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cfloat>

namespace enp::view {

using viewmodel::CommandId;

namespace {

ImVec4 col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

// Keeps the next button on this line when it fits, else it goes on the next one.
void sameLineIfFits(const char* label)
{
    const float w = ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (ImGui::GetItemRectMax().x + spacing + w <= right)
        ImGui::SameLine();
}

const char* roleName(model::ChatRole role)
{
    switch (role) {
    case model::ChatRole::User: return ICON_FA_USER "  You";
    case model::ChatRole::Agent: return ICON_FA_ROBOT "  Agent";
    case model::ChatRole::Tool: return ICON_FA_TERMINAL "  Tool";
    }
    return "";
}

} // namespace

AgentPanel::AgentPanel(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale)
    : vm_(vm), fonts_(fonts), palette_(palette), s_(uiScale)
{
}

// ---- Conversation -----------------------------------------------------------------------------

void AgentPanel::renderConversation(bool compact)
{
    renderHeader(compact);
    // The set-up card sits above the conversation (which scrolls to its end), scrolling itself when tall.
    const viewmodel::NotepadViewModel::AgentSetup setup = vm_.agentSetup();
    // Opened by itself only when no agent can answer: a ready local model stands in for a missing Claude.
    const bool standIn = vm_.agent().localFallback.get() && vm_.agent().localReady();
    if (vm_.agent().showSetup.get() || (vm_.agent().live() && !setup.available && !setup.checking && !standIn)) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, ImGui::GetContentRegionAvail().y * 0.6f));
        ImGui::BeginChild("##setupArea", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);
        renderSetup(ImGui::GetContentRegionAvail().x);
        ImGui::EndChild();
    }
    const float composerH = ImGui::GetTextLineHeightWithSpacing() * 4.0f + ImGui::GetFrameHeightWithSpacing() * 1.6f;
    renderMessages(std::max(40.0f * s_, ImGui::GetContentRegionAvail().y - composerH));
    renderComposer();
}

void AgentPanel::renderHeader(bool compact)
{
    viewmodel::AgentViewModel& agent = vm_.agent();
    const float gearW = ImGui::GetFrameHeight();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    // Who answers first, then Claude's own model and mode (greyed out for the local model).
    const float choiceW = 84.0f * s_;
    const float comboW = std::clamp((ImGui::GetContentRegionAvail().x - choiceW - gearW - spacing * 3.0f) * 0.5f, 70.0f * s_,
                                    compact ? 1000.0f * s_ : 150.0f * s_);
    int choice = agent.agentChoice.get();
    ImGui::SetNextItemWidth(choiceW);
    if (ImGui::Combo("##agentChoice", &choice, agent.choiceNames().data(), int(agent.choiceNames().size())))
        agent.agentChoice.set(choice);
    ImGui::SetItemTooltip("Who answers:\nAuto: the local model for quick questions about the code, Claude Code for the rest\n"
                          "Claude: always Claude Code\nLocal: always the model on this device (offline, free)");
    registerLastItem("agent-choice", "button", "Who answers", nullptr);
    ImGui::SameLine();
    const bool localOnly = choice == int(viewmodel::AgentViewModel::Choice::Local);
    ImGui::BeginDisabled(localOnly);
    int model = agent.modelIndex.get();
    ImGui::SetNextItemWidth(comboW);
    if (ImGui::Combo("##model", &model, agent.modelNames().data(), int(agent.modelNames().size())))
        agent.modelIndex.set(model);
    ImGui::SetItemTooltip(!agent.live() ? "Model (sample data)" : vm_.agentCanStream() ? "Model for the next prompt (Default: your Claude Code setting)" : "Model (chosen by the agent CLI)");
    ImGui::SameLine();
    int autonomy = agent.autonomyIndex.get();
    ImGui::SetNextItemWidth(comboW);
    if (ImGui::Combo("##autonomy", &autonomy, agent.autonomyNames().data(), int(agent.autonomyNames().size())))
        agent.autonomyIndex.set(autonomy);
    ImGui::SetItemTooltip("Edits files: the agent changes files directly (the editor reloads them).\nPlan only: it reads and proposes, and changes nothing.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool setupOpen = agent.showSetup.get();
    if (setupOpen)
        ImGui::PushStyleColor(ImGuiCol_Button, col(palette_.accent));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y)); // the icon centred
    if (ImGui::Button(ICON_FA_GEAR "##agentSetup", ImVec2(gearW, gearW)))
        agent.showSetup.set(!setupOpen);
    ImGui::PopStyleVar();
    if (setupOpen)
        ImGui::PopStyleColor();
    registerLastItem("agent-setup", "button", "Agent setup", [&agent]() { agent.showSetup.set(!agent.showSetup.get()); });
    ImGui::SetItemTooltip("Agent setup: install Claude Code, log in, choose the agent");
    if (!compact) {
        // At the right of the row, or on a row of its own when the panel is narrow.
        const float newChatW = ImGui::CalcTextSize(ICON_FA_PLUS " New chat").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        sameLineIfFits(ICON_FA_PLUS " New chat");
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - newChatW));
        if (ImGui::Button(ICON_FA_PLUS " New chat"))
            vm_.execute(CommandId::NewChat);
    }
}

void AgentPanel::openLink(const std::string& url)
{
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0 || url.rfind("mailto:", 0) == 0)
        vm_.openLink(url);
    else // a path in the workspace, perhaps with #L12 or :12
        vm_.openFile(url.substr(0, url.find_first_of("#")));
}

void AgentPanel::bubble(const model::ChatMessage& message, size_t index, float width)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = 8.0f * s_;
    const bool user = message.role == model::ChatRole::User;
    const float bubbleW = user ? width * 0.86f : width;
    const float x0 = ImGui::GetCursorScreenPos().x + (user ? width - bubbleW : 0.0f);

    ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y));
    const ImVec2 start = ImGui::GetCursorScreenPos();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(ImVec2(start.x + pad, start.y + pad));
    ImGui::BeginGroup();
    ImGui::PushFont(nullptr, Fonts::kUiSize * 0.85f);
    // Answers say who gave them: the local model or Claude Code.
    const bool local = message.by == "Local";
    const std::string role = std::string(roleName(message.role)) + (message.by.empty() ? "" : "  ·  " + message.by);
    ImGui::TextColored(col(message.role != model::ChatRole::Agent ? palette_.textDim : local ? palette_.iconTeal : palette_.modeAgent),
                       "%s", role.c_str());
    ImGui::PopFont();
    // The wrap position is window-local, not screen space.
    if (message.role == model::ChatRole::Agent) { // the agent writes Markdown
        while (markdown_.size() <= index)
            markdown_.emplace_back();
        MarkdownView& view = markdown_[index];
        if (!view.onLink)
            view.onLink = [this](const std::string& url) { openLink(url); };
        view.render(message.text, bubbleW - pad * 2.0f, fonts_, palette_, Fonts::kUiSize, s_);
    } else {
        ImGui::PushTextWrapPos(start.x + bubbleW - pad - ImGui::GetWindowPos().x + ImGui::GetScrollX());
        if (message.role == model::ChatRole::Tool)
            ImGui::TextDisabled("%s", message.text.c_str());
        else
            ImGui::TextUnformatted(message.text.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndGroup();
    const ImVec2 end(start.x + bubbleW, ImGui::GetItemRectMax().y + pad);
    // Right-click: copy the message (as written, Markdown included).
    const std::string menuId = "##bubbleMenu" + std::to_string(index);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(start, end) && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup(menuId.c_str());
    if (ImGui::BeginPopup(menuId.c_str())) {
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy message"))
            ImGui::SetClipboardText(message.text.c_str());
        ImGui::EndPopup();
    }
    dl->ChannelsSetCurrent(0);
    const ImU32 bg = user ? palette_.bubbleUser : message.role == model::ChatRole::Agent ? palette_.bubbleAgent : palette_.bubbleTool;
    dl->AddRectFilled(start, end, bg, 8.0f * s_);
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x, end.y + 8.0f * s_));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void AgentPanel::renderMessages(float height)
{
    const viewmodel::AgentViewModel& agent = vm_.agent();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * s_, 6.0f * s_));
    ImGui::BeginChild("##messages", ImVec2(0.0f, height), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    const float width = ImGui::GetContentRegionAvail().x;
    const viewmodel::NotepadViewModel::AgentSetup setup = vm_.agentSetup();
    const bool missing = !setup.available && !setup.checking;
    if (agent.messages().empty() && agent.live() && missing && agent.localFallback.get() && agent.localReady()) {
        // No Claude Code here (a phone, say): the local model answers.
        ImGui::Spacing();
        ImGui::PushTextWrapPos(width);
        ImGui::TextDisabled("%s  Ask about this code.", ICON_FA_LIGHTBULB);
        ImGui::TextDisabled("Claude Code is not available here, so the local model answers, on this device and offline. "
                            "It reads the code you are looking at (the selection, or the lines around the caret); it "
                            "cannot edit files.");
        ImGui::PopTextWrapPos();
    } else if (agent.messages().empty() && agent.live() && !missing) {
        // A real workspace: say where the prompt goes.
        ImGui::Spacing();
        ImGui::PushTextWrapPos(width);
        ImGui::TextDisabled("%s  Ask about this code.", ICON_FA_LIGHTBULB);
        if (setup.claude)
            ImGui::TextDisabled("Your question goes to Claude Code in this folder, with the file you are on as context. It is "
                                "one conversation (New chat starts another). The agent can edit files, and can point at and "
                                "click things in this window when you ask it to.");
        else
            ImGui::TextDisabled("Your question goes to %s in this folder, with the file you are on as context.",
                                setup.command.c_str());
        ImGui::PopTextWrapPos();
    } else if (agent.messages().empty() && !agent.live()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s  Describe what you want to build or fix.", ICON_FA_LIGHTBULB);
        ImGui::TextDisabled("The agent plans, edits and runs the review tools;");
        ImGui::TextDisabled("you review the changes before they land.");
    }
    // A new session (another folder was opened) starts with fewer messages: the rendered Markdown
    // of the old ones goes too.
    if (markdown_.size() > agent.messages().size())
        markdown_.resize(agent.messages().size());
    for (const model::ChatMessage& message : agent.messages())
        bubble(message, size_t(&message - agent.messages().data()), width);
    // The last answer came from the local model: Claude Code can have a go too.
    if (!agent.busy.get() && !agent.messages().empty() && agent.messages().back().by == "Local" && agent.claudeReady()) {
        const int index = int(agent.messages().size()) - 1;
        if (ImGui::SmallButton(ICON_FA_ROBOT "  Ask Claude instead"))
            vm_.agent().askClaudeInstead(index);
        registerLastItem("agent-ask-claude", "button", "Ask Claude instead", [this, index]() { vm_.agent().askClaudeInstead(index); });
        ImGui::SetItemTooltip("Send the same question to Claude Code");
        ImGui::Spacing();
    }
    if (agent.busy.get()) {
        static const char* const dots[] = {".", "..", "..."};
        const std::string& activity = agent.activity();
        ImGui::PushTextWrapPos(width);
        ImGui::TextColored(col(palette_.modeAgent), ICON_FA_ROBOT "  %s%s", activity.empty() ? "Thinking" : activity.c_str(),
                           dots[int(ImGui::GetTime() * 3.0) % 3]);
        ImGui::PopTextWrapPos();
    }
    if (agent.messages().size() != lastMessageCount_ || agent.busy.get()) {
        lastMessageCount_ = agent.messages().size();
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

void AgentPanel::renderSetup(float width)
{
    viewmodel::AgentViewModel& agent = vm_.agent();
    const viewmodel::NotepadViewModel::AgentSetup setup = vm_.agentSetup();
    const float pad = 10.0f * s_;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col(palette_.bubbleTool));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * s_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::BeginChild("##agentSetupCard", ImVec2(width, 0.0f),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    const float inner = ImGui::GetContentRegionAvail().x;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);

    if (os::isMobile()) {
        ImGui::TextColored(col(palette_.modeAgent), ICON_FA_PLUG "  Agent setup");
        ImGui::SameLine(ImGui::GetCursorPosX() + inner - ImGui::GetFrameHeight());
        if (ImGui::SmallButton(ICON_FA_XMARK "##closeSetup"))
            agent.showSetup.set(false);
        ImGui::TextDisabled("Claude Code does not run on phones: here the agent is the local model.");
        renderLocalSetup(inner);
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::Spacing();
        return;
    }

    // Title, with a close button once there is nothing left to fix.
    ImGui::TextColored(col(palette_.modeAgent), ICON_FA_PLUG "  Agent setup");
    if ((setup.available && !agent.needsLogin()) || (agent.localFallback.get() && agent.localReady())) {
        ImGui::SameLine(ImGui::GetCursorPosX() + inner - ImGui::GetFrameHeight());
        if (ImGui::SmallButton(ICON_FA_XMARK "##closeSetup"))
            agent.showSetup.set(false);
        ImGui::SetItemTooltip("Close");
    }

    // Where things stand.
    if (setup.checking) {
        ImGui::TextDisabled("Looking for %s...", setup.claude ? "Claude Code" : setup.command.c_str());
    } else if (setup.available) {
        ImGui::TextColored(col(palette_.iconGreen), ICON_FA_CIRCLE_CHECK);
        ImGui::SameLine();
        ImGui::TextUnformatted(setup.claude ? "Claude Code is installed." : ("Using " + setup.command).c_str());
        if (!setup.executable.empty())
            ImGui::TextDisabled("%s", setup.executable.c_str());
    } else {
        ImGui::TextColored(col(palette_.iconRed), ICON_FA_CIRCLE_XMARK);
        ImGui::SameLine();
        ImGui::TextUnformatted(setup.claude ? "Claude Code was not found." : (setup.command + " was not found.").c_str());
    }
    if (agent.needsLogin()) {
        ImGui::TextColored(col(palette_.iconRed), ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine();
        ImGui::TextUnformatted("Claude Code could not sign in. Log in once in the terminal, then ask again.");
    }

    if (setup.claude) {
        // 1. Install (when missing).
        if (!setup.available && !setup.checking) {
            ImGui::Spacing();
            ImGui::TextUnformatted("1. Install Claude Code with its official installer (no administrator rights needed):");
            std::string install = viewmodel::NotepadViewModel::agentInstallCommand();
            ImGui::PushFont(fonts_.mono, 0.0f);
            ImGui::SetNextItemWidth(inner);
            ImGui::InputText("##installCommand", &install, ImGuiInputTextFlags_ReadOnly);
            ImGui::PopFont();
            if (ImGui::Button(ICON_FA_TERMINAL " Install in terminal"))
                vm_.installAgent();
            registerLastItem("agent-setup-install", "button", "Install Claude Code", [this]() { vm_.installAgent(); });
            sameLineIfFits(ICON_FA_COPY " Copy");
            if (ImGui::Button(ICON_FA_COPY " Copy"))
                ImGui::SetClipboardText(viewmodel::NotepadViewModel::agentInstallCommand().c_str());
#if defined(_WIN32)
            ImGui::TextDisabled("Or: winget install Anthropic.ClaudeCode, or npm install -g @anthropic-ai/claude-code");
#elif defined(__APPLE__)
            ImGui::TextDisabled("Or: brew install --cask claude-code, or npm install -g @anthropic-ai/claude-code");
#else
            ImGui::TextDisabled("Or: npm install -g @anthropic-ai/claude-code (or apt / dnf, see the guide)");
#endif
            ImGui::Spacing();
            ImGui::TextUnformatted("2. Log in once (needs a Claude Pro, Max, Team, Enterprise or Console account):");
        } else if (setup.available) {
            ImGui::Spacing();
            ImGui::TextDisabled("Claude Code uses its own login (a Claude Pro, Max, Team, Enterprise or Console account); "
                                "this app stores no keys.");
        }
        if (agent.needsLogin())
            ImGui::PushStyleColor(ImGuiCol_Button, col(palette_.accent));
        ImGui::BeginDisabled(!setup.available);
        if (ImGui::Button(ICON_FA_RIGHT_TO_BRACKET " Log in in terminal"))
            vm_.loginAgent();
        ImGui::EndDisabled();
        if (agent.needsLogin())
            ImGui::PopStyleColor();
        registerLastItem("agent-setup-login", "button", "Log in to Claude Code", [this]() { vm_.loginAgent(); });
        ImGui::SetItemTooltip(setup.available ? "Starts claude in the terminal: follow its sign-in steps (a browser opens), then type /exit"
                                              : "Install Claude Code first");
        sameLineIfFits(ICON_FA_ROTATE " Check again");
    }
    if (ImGui::Button(ICON_FA_ROTATE " Check again"))
        vm_.recheckAgent();
    registerLastItem("agent-setup-recheck", "button", "Check for the agent again", [this]() { vm_.recheckAgent(); });
    if (setup.claude) {
        sameLineIfFits(ICON_FA_UP_RIGHT_FROM_SQUARE " Setup guide");
        if (ImGui::Button(ICON_FA_UP_RIGHT_FROM_SQUARE " Setup guide"))
            vm_.openLink(viewmodel::NotepadViewModel::kAgentSetupUrl);
        ImGui::SetItemTooltip("%s", viewmodel::NotepadViewModel::kAgentSetupUrl);
    }

    renderLocalSetup(inner);

    // Settings.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Agent command");
    if (!editingCommand_)
        commandEdit_ = agent.command.get();
    ImGui::SetNextItemWidth(inner);
    ImGui::InputTextWithHint("##agentCommand", "claude -p  (Claude Code, the default)", &commandEdit_);
    editingCommand_ = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit())
        agent.command.set(commandEdit_);
    ImGui::SetItemTooltip("Leave empty for Claude Code. Any other command gets the prompt on stdin and answers on stdout\n"
                          "(one answer at a time, without streaming or app control).");
    bool control = agent.appControl.get();
    if (ImGui::Checkbox("Let the agent point at and click things in this window", &control))
        agent.appControl.set(control);
    ImGui::SetItemTooltip("Gives Claude Code this app's MCP tools. It uses them when you ask it to show or do something in the app,\n"
                          "with its own glowing pointer: your mouse is never moved.");

    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::Spacing();
}

// The local model in the set-up card: whether it is here, getting it, and when it answers.
void AgentPanel::renderLocalSetup(float inner)
{
    using Lab = viewmodel::EngineLabViewModel;
    using State = Lab::FileState;
    viewmodel::AgentViewModel& agent = vm_.agent();
    Lab& lab = vm_.engineLab();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(col(palette_.iconTeal), ICON_FA_MICROCHIP "  Local model (offline, on this device)");
    if (!lab.hasEngine() || !lab.info().available) {
        ImGui::TextDisabled("%s", lab.hasEngine() ? lab.info().unavailableReason.c_str() : "This build has no local model.");
        return;
    }
    const Lab::FileStatus model = lab.fileStatus(Lab::Which::Model);
    const Lab::FileStatus drafter = lab.fileStatus(Lab::Which::Drafter);
    const bool ready = lab.fileUsable(Lab::Which::Model);
    char size[32];
    std::snprintf(size, sizeof(size), "%.2f GB", double(Lab::modelFile().size) / 1e9);
    if (model.state == State::Downloading || drafter.state == State::Downloading) {
        const Lab::FileStatus& d = model.state == State::Downloading ? model : drafter;
        const float fraction = d.total ? float(double(d.have) / double(d.total)) : 0.0f;
        ImGui::TextUnformatted(lab.importing() ? "Copying the file in..." : "Downloading...");
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col(palette_.accent));
        ImGui::ProgressBar(fraction, ImVec2(inner, 0.0f));
        ImGui::PopStyleColor();
        if (!d.note.empty())
            ImGui::TextColored(col(palette_.iconOrange), "%s", d.note.c_str());
        if (ImGui::Button(ICON_FA_STOP " Stop"))
            lab.cancelDownload();
    } else if (model.state == State::Verifying || drafter.state == State::Verifying) {
        ImGui::TextDisabled("Checking the download...");
    } else if (ready) {
        ImGui::TextColored(col(palette_.iconGreen), ICON_FA_CIRCLE_CHECK);
        ImGui::SameLine();
        ImGui::TextUnformatted(lab.fileUsable(Lab::Which::Drafter) ? "Gemma 4 E2B is here, with its drafter (faster)."
                                                                   : "Gemma 4 E2B is here.");
        if (!lab.fileUsable(Lab::Which::Drafter)) {
            if (ImGui::Button(ICON_FA_DOWNLOAD " Get the drafter (77 MB, faster answers)"))
                lab.download(Lab::Which::Drafter);
            registerLastItem("agent-local-drafter", "button", "Download the drafter", [&lab]() { lab.download(Lab::Which::Drafter); });
        }
    } else {
        ImGui::TextWrapped("Gemma 4 E2B (%s) answers quick questions about your code on this device, without a network "
                           "or an account. It reads what you are looking at; it cannot edit files.", size);
        if (ImGui::Button(ICON_FA_DOWNLOAD " Download"))
            lab.download(Lab::Which::Model);
        registerLastItem("agent-local-download", "button", "Download the local model", [&lab]() { lab.download(Lab::Which::Model); });
        if (lab.canImport()) {
            sameLineIfFits(ICON_FA_FILE_IMPORT " Import file...");
            if (ImGui::Button(ICON_FA_FILE_IMPORT " Import file..."))
                lab.importFile(Lab::Which::Model);
            ImGui::SetItemTooltip("Pick the file if you downloaded it elsewhere (for example in the browser)");
        }
    }
    if (!lab.notice().empty() && lab.noticeIsError())
        ImGui::TextColored(col(palette_.iconRed), ICON_FA_TRIANGLE_EXCLAMATION "  %s", lab.notice().c_str());
    bool fallback = agent.localFallback.get();
    if (ImGui::Checkbox("Fall back to the local model", &fallback))
        agent.localFallback.set(fallback);
    ImGui::SetItemTooltip("When Claude Code is not installed, not logged in, offline or fails, the local model answers instead");
    ImGui::TextDisabled("Who answers is chosen at the top of the panel: Auto, Claude or Local.");
}

void AgentPanel::renderComposer()
{
    viewmodel::AgentViewModel& agent = vm_.agent();
    const std::string context = vm_.selectionContextLabel();
    if (!context.empty()) {
        ImGui::PushFont(nullptr, Fonts::kUiSize * 0.85f);
        ImGui::TextColored(col(palette_.textDim), ICON_FA_FILE_CODE " %s", context.c_str());
        ImGui::PopFont();
        ImGui::SetItemTooltip("Context sent with the prompt: the active file and selected lines");
    }
    if (focusPrompt_) {
        ImGui::SetKeyboardFocusHere();
        focusPrompt_ = false;
    }
    const float buttonW = ImGui::GetFrameHeight() * 1.6f;
    const float inputH = ImGui::GetTextLineHeight() * 3.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
    const bool send = ImGui::InputTextMultiline(
        "##prompt", &prompt_, ImVec2(ImGui::GetContentRegionAvail().x - buttonW - 4.0f * s_, inputH),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_WordWrap);
    registerLastItem("agent-prompt", "textbox", "Ask the agent", [this]() { focusPrompt(); });
    if (prompt_.empty() && !ImGui::IsItemActive()) {
        const ImVec2 p = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + ImGui::GetStyle().FramePadding.x, p.y + ImGui::GetStyle().FramePadding.y),
                                            palette_.textFaint, "Ask the agent...  (Enter to send, Ctrl+Enter for a new line)");
    }
    ImGui::SameLine(0.0f, 4.0f * s_);
    if (agent.busy.get()) {
        ImGui::PushStyleColor(ImGuiCol_Text, col(palette_.iconRed));
        if (ImGui::Button(ICON_FA_STOP "##stop", ImVec2(buttonW, inputH)))
            vm_.execute(CommandId::StopAgent);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Stop the agent");
    } else {
        ImGui::BeginDisabled(prompt_.empty());
        ImGui::PushStyleColor(ImGuiCol_Button, col(palette_.accent));
        const bool clicked = ImGui::Button(ICON_FA_PAPER_PLANE "##send", ImVec2(buttonW, inputH));
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Send (Enter)");
        if ((send || clicked) && !prompt_.empty()) {
            agent.submitPrompt(prompt_, context);
            prompt_.clear();
            focusPrompt_ = true;
        }
    }
}

// ---- Plan and changes -------------------------------------------------------------------------

void AgentPanel::renderPlan()
{
    viewmodel::AgentViewModel& agent = vm_.agent();
    ImGui::TextDisabled("PLAN");
    if (agent.tasks().empty())
        ImGui::TextDisabled("No plan yet.");
    for (const model::AgentTask& task : agent.tasks()) {
        switch (task.state) {
        case model::TaskState::Done:
            ImGui::TextColored(col(palette_.iconGreen), ICON_FA_CIRCLE_CHECK);
            break;
        case model::TaskState::Running:
            ImGui::TextColored(col(palette_.accent), ICON_FA_CIRCLE_NOTCH);
            break;
        case model::TaskState::Pending:
            ImGui::TextColored(col(palette_.textFaint), ICON_FA_CIRCLE);
            break;
        }
        ImGui::SameLine();
        if (task.state == model::TaskState::Done)
            ImGui::TextDisabled("%s", task.title.c_str());
        else
            ImGui::TextUnformatted(task.title.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("CHANGES  %d pending", agent.pendingChangeCount());
    for (int i = 0; i < int(agent.changes().size()); ++i) {
        const model::FileChange& change = agent.changes()[size_t(i)];
        ImGui::PushID(i);
        const bool selected = agent.selectedChange.get() == i;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##change", selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, ImGui::GetTextLineHeight())))
            agent.selectedChange.set(i);
        ImGui::SetCursorScreenPos(p);
        const char* icon = change.state == model::ChangeState::Accepted ? ICON_FA_CHECK
                           : change.state == model::ChangeState::Rejected ? ICON_FA_BAN
                                                                          : ICON_FA_PEN;
        const ImU32 iconColor = change.state == model::ChangeState::Accepted ? palette_.iconGreen
                                : change.state == model::ChangeState::Rejected ? palette_.textFaint
                                                                               : palette_.iconOrange;
        ImGui::TextColored(col(iconColor), "%s", icon);
        ImGui::SameLine();
        ImGui::TextUnformatted(change.path.c_str());
        ImGui::SameLine();
        ImGui::TextColored(col(palette_.diffAddedText), "+%d", change.stats.added);
        ImGui::SameLine(0.0f, 4.0f * s_);
        ImGui::TextColored(col(palette_.diffRemovedText), "-%d", change.stats.removed);
        ImGui::PopID();
    }
    ImGui::Spacing();
    const float half = ImGui::GetContentRegionAvail().x * 0.5f - 2.0f * s_;
    ImGui::BeginDisabled(!vm_.command(CommandId::AcceptAllChanges).canExecute());
    if (ImGui::Button(ICON_FA_CHECK_DOUBLE " Accept all", ImVec2(half, 0.0f)))
        vm_.execute(CommandId::AcceptAllChanges);
    ImGui::SameLine(0.0f, 4.0f * s_);
    if (ImGui::Button(ICON_FA_BAN " Reject all", ImVec2(half, 0.0f)))
        vm_.execute(CommandId::RejectAllChanges);
    ImGui::EndDisabled();
}

void AgentPanel::renderChangePreview(ImVec2 size)
{
    viewmodel::AgentViewModel& agent = vm_.agent();
    const model::FileChange* change = agent.currentChange();
    if (!change) {
        ImGui::TextDisabled("Select a change to review it.");
        return;
    }
    const float startY = ImGui::GetCursorPosY();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(change->path.c_str());
    ImGui::SameLine();
    ImGui::TextColored(col(palette_.diffAddedText), "+%d", change->stats.added);
    ImGui::SameLine(0.0f, 4.0f * s_);
    ImGui::TextColored(col(palette_.diffRemovedText), "-%d", change->stats.removed);
    ImGui::SameLine();
    const char* state = change->state == model::ChangeState::Accepted   ? "Accepted"
                        : change->state == model::ChangeState::Rejected ? "Rejected"
                                                                        : "Proposed";
    ImGui::TextDisabled("%s", state);

    const float buttonsW = 3.0f * 86.0f * s_;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttonsW));
    const int index = agent.selectedChange.get();
    ImGui::BeginDisabled(change->state != model::ChangeState::Proposed);
    ImGui::PushStyleColor(ImGuiCol_Button, col(palette_.diffAddedBg));
    if (ImGui::Button(ICON_FA_CHECK " Accept"))
        agent.acceptChange(index);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 4.0f * s_);
    if (ImGui::Button(ICON_FA_XMARK " Reject"))
        agent.rejectChange(index);
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 4.0f * s_);
    if (ImGui::Button(ICON_FA_FILE_CODE " Open"))
        vm_.openFile(change->path);
    ImGui::SetItemTooltip("Open the file in the editor");

    const float used = ImGui::GetCursorPosY() - startY;
    renderSideBySideDiff("##changeDiff", change->diff, fonts_, palette_, vm_.zoom.get(), s_,
                         ImVec2(size.x, std::max(1.0f, size.y - used)));
}

} // namespace enp::view
