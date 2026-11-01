#include "enp/view/EngineLabView.h"

#include "enp/view/Fit.h"
#include "enp/view/UiAutomation.h"

#include "IconsFontAwesome6.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cstdio>

namespace enp::view {

using FileState = viewmodel::EngineLabViewModel::FileState;
using Record = viewmodel::EngineLabViewModel::Record;

namespace {

ImVec4 col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

std::string format(const char* fmt, double v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), fmt, v);
    return buf;
}

std::string gigabytes(uint64_t bytes) { return format("%.2f GB", double(bytes) / 1e9); }

} // namespace

EngineLabView::EngineLabView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale)
    : vm_(vm), fonts_(fonts), palette_(palette), s_(uiScale)
{
}

bool EngineLabView::section(const char* icon, const char* title)
{
    ImGui::Spacing();
    const std::string label = std::string(icon) + "  " + title + "###lab-" + title;
    ImGui::PushStyleColor(ImGuiCol_Header, col(palette_.panelHeaderBg));
    const bool open = ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopStyleColor();
    registerLastItem(std::string("engine-lab:section:") + title, "button", title, nullptr);
    return open;
}

void EngineLabView::renderSummary()
{
    viewmodel::EngineLabViewModel& l = lab();
    if (l.running()) {
        ImGui::TextColored(col(palette_.accent), ICON_FA_GAUGE_HIGH "  %s...", l.stage().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel"))
            l.cancelRun();
        return;
    }
    if (l.downloading()) {
        const auto st = l.fileStatus(l.fileStatus(Which::Model).state == viewmodel::EngineLabViewModel::FileState::Downloading
                                         ? Which::Model : Which::Drafter);
        ImGui::TextColored(col(palette_.accent), ICON_FA_DOWNLOAD "  Downloading: %s of %s", gigabytes(st.have).c_str(),
                           gigabytes(st.total).c_str());
        return;
    }
    const std::vector<Record>& history = l.history();
    if (history.empty() || !history.front().result.ok) {
        ImGui::TextDisabled("Developer view: how fast the on-device model runs here.");
        return;
    }
    const Record& r = history.front();
    ImGui::Text("Latest: %.1f tok/s generating, %.0f tok/s prompt", r.result.generateTokensPerSecond,
                r.result.promptTokensPerSecond);
    ImGui::TextDisabled("%s%s, %s", r.result.backend.c_str(), r.settings.drafterPath.empty() ? "" : " + drafter", r.when.c_str());
}

// A small labelled figure: the label dim above, the value below (larger for the headline ones).
void EngineLabView::stat(const char* label, const std::string& value, float width, bool big)
{
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, col(palette_.textDim));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::PushFont(nullptr, big ? Fonts::kUiSize * 1.6f : 0.0f);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopFont();
    ImGui::EndGroup();
    const float used = ImGui::GetItemRectSize().x;
    if (used < width)
        ImGui::SameLine(0.0f, width - used);
    else
        ImGui::SameLine();
}

void EngineLabView::render(bool fullScreen, ImVec2 areaMin, ImVec2 areaMax)
{
    if (!open_)
        return;
    lab().refresh();
    const ImVec2 area(areaMax.x - areaMin.x, areaMax.y - areaMin.y);
    // The body scrolls (a child), not the window: the header stays put.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse;
    if (fullScreen) {
        ImGui::SetNextWindowPos(areaMin);
        ImGui::SetNextWindowSize(area);
        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    } else {
        ImGui::SetNextWindowPos(ImVec2(areaMin.x + area.x * 0.5f, areaMin.y + area.y * 0.5f), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(fit::windowSize(ImVec2(720.0f * s_, 680.0f * s_)), ImGuiCond_Appearing);
    }
    bool open = true;
    if (!ImGui::Begin(ICON_FA_FLASK "  Engine Lab###engine-lab", &open, flags)) {
        ImGui::End();
        open_ = open;
        return;
    }
    if (registerWindowClose("engine-lab:close", "Close Engine Lab"))
        open = false;
    ImGui::PushTextWrapPos(0.0f);
    // A fixed header with the state at a glance; the sections scroll below it (also by finger).
    renderSummary();
    // What just happened, or went wrong (the phone layout has no status bar to show it).
    if (!lab().notice().empty()) {
        const bool error = lab().noticeIsError();
        ImGui::TextColored(col(error ? palette_.iconRed : palette_.textDim), "%s  %s",
                           error ? ICON_FA_TRIANGLE_EXCLAMATION : ICON_FA_CIRCLE_INFO, lab().notice().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_XMARK "##notice"))
            lab().clearNotice();
        ImGui::SetItemTooltip("Dismiss");
    }
    ImGui::Separator();
    ImGui::BeginChild("##lab-body", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    ImGui::PushTextWrapPos(0.0f);
    renderDevice();
    if (vm_.engineLab().info().available) {
        if (section(ICON_FA_DOWNLOAD, "Model files")) {
            renderFile(Which::Model);
            ImGui::Spacing();
            renderFile(Which::Drafter);
        }
        renderRun();
        renderLatest();
        renderHistory();
    }
    ImGui::Spacing();
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopTextWrapPos();
    renderMobileDataPrompt();
    ImGui::End();
    open_ = open;
}

// "You are on mobile data": a large download asks before it uses the data plan.
void EngineLabView::renderMobileDataPrompt()
{
    viewmodel::EngineLabViewModel& l = lab();
    const char* title = "Download on mobile data?###engine-lab-mobile-data";
    if (l.confirmationPending() && !ImGui::IsPopupOpen(title))
        ImGui::OpenPopup(title);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    fit::constrainNextWindow();
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    const auto& file = l.pendingDownload() == Which::Model ? viewmodel::EngineLabViewModel::modelFile()
                                                           : viewmodel::EngineLabViewModel::drafterFile();
    const uint64_t have = lab().fileStatus(l.pendingDownload()).have;
    const uint64_t left = file.size > have ? file.size - have : 0;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(380.0f * s_, ImGui::GetMainViewport()->WorkSize.x - 60.0f * s_));
    ImGui::TextColored(col(palette_.iconOrange), ICON_FA_TRIANGLE_EXCLAMATION "  You are on mobile data.");
    ImGui::TextWrapped("%s needs about %s, which counts against your data plan.", file.label, gigabytes(left).c_str());
    ImGui::PopTextWrapPos();
    bool always = l.allowMobileData.get();
    if (ImGui::Checkbox("Don't ask again", &always))
        l.allowMobileData.set(always);
    ImGui::Spacing();
    const bool go = ImGui::Button(ICON_FA_DOWNLOAD "  Download");
    registerLastItem("engine-lab:mobile-data:download", "button", "Download on mobile data", [&l]() { l.confirmMobileDownload(); });
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
    registerLastItem("engine-lab:mobile-data:cancel", "button", "Cancel", [&l]() { l.cancelConfirmation(); });
    if (go || cancel || !l.confirmationPending()) {
        if (go)
            l.confirmMobileDownload();
        else
            l.cancelConfirmation();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EngineLabView::renderDevice()
{
    const interfaces::EngineInfo& info = lab().info();
    if (!section(ICON_FA_MICROCHIP, "This device"))
        return;
    if (!lab().hasEngine()) {
        ImGui::TextColored(col(palette_.iconRed), "No engine service in this build.");
        return;
    }
    if (!info.socName.empty())
        ImGui::Text("Chip: %s", info.socName.c_str());
    // Phones report the model (e.g. SM-S928B) where desktops name the CPU.
    ImGui::Text(info.socName.empty() ? "CPU: %s, %d threads" : "Phone: %s, %d CPU threads",
                info.cpuName.empty() ? "unknown" : info.cpuName.c_str(), info.cpuThreads);
    if (!info.cpuFeatures.empty())
        ImGui::TextDisabled("CPU features: %s", info.cpuFeatures.c_str());
    if (info.memoryTotal)
        ImGui::Text("Memory: %s, %s available", gigabytes(info.memoryTotal).c_str(), gigabytes(info.memoryAvailable).c_str());
    for (const interfaces::EngineDevice& d : info.devices) {
        if (d.kind == "CPU")
            continue;
        ImGui::Text("%s %s: %s", d.kind.c_str(), d.name.c_str(), d.description.c_str());
        if (d.memoryTotal)
            ImGui::SameLine(), ImGui::TextDisabled("(%s)", gigabytes(d.memoryTotal).c_str());
    }
    if (info.available && std::none_of(info.devices.begin(), info.devices.end(), [](const auto& d) { return d.kind != "CPU"; }))
        ImGui::TextDisabled("No GPU reachable by this build (CPU only).");
    if (!info.engineVersion.empty())
        ImGui::TextDisabled("Engine: %s", info.engineVersion.c_str());
    if (!info.available)
        ImGui::TextColored(col(palette_.iconRed), ICON_FA_TRIANGLE_EXCLAMATION "  %s", info.unavailableReason.c_str());
}

void EngineLabView::renderFile(Which which)
{
    const auto& file = which == Which::Model ? viewmodel::EngineLabViewModel::modelFile()
                                             : viewmodel::EngineLabViewModel::drafterFile();
    const viewmodel::EngineLabViewModel::FileStatus st = lab().fileStatus(which);
    const char* id = which == Which::Model ? "model" : "drafter";
    ImGui::PushID(id);

    ImGui::TextUnformatted(file.label);
    ImGui::SameLine();
    ImU32 chipColour = palette_.textDim;
    std::string chip;
    switch (st.state) {
    case FileState::Missing: chip = "not downloaded, " + gigabytes(file.size); break;
    case FileState::Partial: chip = "partly downloaded: " + gigabytes(st.have) + " of " + gigabytes(st.total); break;
    case FileState::Downloading:
        chip = (lab().importing() ? "copying " : "downloading ") + gigabytes(st.have) + " of " + gigabytes(st.total);
        chipColour = palette_.accent;
        break;
    case FileState::Verifying: chip = "checking the checksum"; chipColour = palette_.accent; break;
    case FileState::Unverified: chip = "downloaded, not checked yet"; chipColour = palette_.iconOrange; break;
    case FileState::Ready: chip = ICON_FA_CIRCLE_CHECK " ready, checksum matches"; chipColour = palette_.iconGreen; break;
    case FileState::Mismatch: chip = ICON_FA_TRIANGLE_EXCLAMATION " checksum does not match"; chipColour = palette_.iconRed; break;
    case FileState::Custom: chip = "your file, " + gigabytes(st.have) + " (not checked)"; break;
    }
    ImGui::TextColored(col(chipColour), "%s", chip.c_str());

    if (st.state == FileState::Downloading || st.state == FileState::Verifying) {
        const float fraction = st.state == FileState::Verifying ? float(st.verifyFraction)
                               : st.total ? float(double(st.have) / double(st.total)) : 0.0f;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col(palette_.accent));
        ImGui::ProgressBar(fraction, ImVec2(fit::itemWidth(-1.0f, 120.0f * s_), 0.0f));
        ImGui::PopStyleColor();
        if (!st.note.empty())
            ImGui::TextColored(col(palette_.iconOrange), "%s", st.note.c_str());
        if (lab().waitingForWifi()) {
            if (ImGui::Button(ICON_FA_SIGNAL "  Use mobile data"))
                lab().useMobileData();
            ImGui::SetItemTooltip("Start again on mobile data (asks first)");
            registerLastItem(std::string("engine-lab:") + id + ":mobile-data", "button", "Use mobile data", [this]() { lab().useMobileData(); });
            fit::sameLineIfFitsButton("Stop download");
        }
    }

    const std::string& failed = lab().downloadError(which);
    if (!failed.empty() && st.state != FileState::Downloading && st.state != FileState::Verifying)
        ImGui::TextColored(col(palette_.iconRed), ICON_FA_TRIANGLE_EXCLAMATION "  Last try: %s", failed.c_str());
    const bool busy = lab().downloading() || st.state == FileState::Verifying;
    if (st.state == FileState::Downloading) {
        if (ImGui::Button(ICON_FA_STOP "  Stop download"))
            lab().cancelDownload();
        registerLastItem(std::string("engine-lab:") + id + ":stop", "button", "Stop download", [this]() { lab().cancelDownload(); });
    } else if (st.state != FileState::Ready && st.state != FileState::Custom && st.state != FileState::Verifying) {
        const char* label = !failed.empty() ? ICON_FA_ROTATE_RIGHT "  Try again"
                            : st.state == FileState::Partial ? ICON_FA_DOWNLOAD "  Resume" : ICON_FA_DOWNLOAD "  Download";
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(label))
            lab().download(which);
        ImGui::EndDisabled();
        registerLastItem(std::string("engine-lab:") + id + ":download", "button", "Download",
                         [this, which]() { lab().download(which); }, !busy);
        if (lab().canImport()) {
            // A file downloaded elsewhere (the browser): apps cannot read Downloads directly.
            fit::sameLineIfFitsButton("    Import file...");
            ImGui::BeginDisabled(busy);
            if (ImGui::Button(ICON_FA_FILE_IMPORT "  Import file..."))
                lab().importFile(which);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Pick the file (e.g. downloaded in the browser); it is copied into the app");
            registerLastItem(std::string("engine-lab:") + id + ":import", "button", "Import file",
                             [this, which]() { lab().importFile(which); }, !busy);
        }
        if (st.state == FileState::Unverified) {
            fit::sameLineIfFitsButton("Check");
            if (ImGui::Button("Check"))
                lab().verify(which);
        }
    }
    ImGui::TextDisabled("From %s", file.origin);
    if (!st.path.empty())
        ImGui::TextDisabled("%s", st.path.c_str());
    ImGui::PopID();
}

void EngineLabView::renderRun()
{
    viewmodel::EngineLabViewModel& l = lab();
    const interfaces::EngineInfo& info = l.info();
    if (!section(ICON_FA_GAUGE_HIGH, "Run"))
        return;

    if (ImGui::TreeNode("Use files already on this device")) {
        ImGui::TextDisabled("A full path; empty uses the downloaded file.");
        std::string model = l.customModelPath.get(), drafter = l.customDrafterPath.get();
        ImGui::SetNextItemWidth(fit::itemWidth(-1.0f, 160.0f * s_));
        if (ImGui::InputTextWithHint("##own-model", "Model .gguf", &model))
            l.customModelPath.set(model);
        ImGui::SetNextItemWidth(fit::itemWidth(-1.0f, 160.0f * s_));
        if (ImGui::InputTextWithHint("##own-drafter", "Drafter .gguf", &drafter))
            l.customDrafterPath.set(drafter);
        bool metered = l.allowMobileData.get();
        if (ImGui::Checkbox("Downloads may use mobile data", &metered))
            l.allowMobileData.set(metered);
        ImGui::TreePop();
    }

    int threads = l.threads.get();
    ImGui::SetNextItemWidth(fit::itemWidth(220.0f * s_, 120.0f * s_));
    if (ImGui::SliderInt("CPU threads", &threads, 1, std::max(1, info.cpuThreads)))
        l.threads.set(threads);
    const bool hasGpu = std::any_of(info.devices.begin(), info.devices.end(), [](const auto& d) { return d.kind != "CPU"; });
    bool gpu = l.useGpu.get() && hasGpu;
    ImGui::BeginDisabled(!hasGpu);
    if (ImGui::Checkbox("Use the GPU", &gpu))
        l.useGpu.set(gpu);
    ImGui::EndDisabled();
    if (!hasGpu && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("No GPU backend in this build yet.");
    int tokens = l.generateTokens.get();
    ImGui::SetNextItemWidth(fit::itemWidth(220.0f * s_, 120.0f * s_));
    if (ImGui::SliderInt("Tokens to generate", &tokens, 32, 512))
        l.generateTokens.set(tokens);

    const bool modelReady = l.fileUsable(Which::Model);
    const bool drafterReady = l.fileUsable(Which::Drafter);
    if (l.running()) {
        ImGui::TextColored(col(palette_.accent), "%s...", l.stage().c_str());
        if (ImGui::Button(ICON_FA_STOP "  Cancel"))
            l.cancelRun();
        registerLastItem("engine-lab:cancel", "button", "Cancel run", [&l]() { l.cancelRun(); });
        return;
    }
    if (!modelReady)
        ImGui::TextDisabled("Download the model to run.");
    ImGui::BeginDisabled(!modelReady);
    if (ImGui::Button(ICON_FA_PLAY "  Run"))
        l.run(false);
    registerLastItem("engine-lab:run", "button", "Run", [&l]() { l.run(false); }, modelReady);
    ImGui::BeginDisabled(!drafterReady);
    fit::sameLineIfFitsButton("    Run with drafter");
    if (ImGui::Button(ICON_FA_BOLT "  Run with drafter"))
        l.run(true);
    registerLastItem("engine-lab:run-drafter", "button", "Run with drafter", [&l]() { l.run(true); }, modelReady && drafterReady);
    fit::sameLineIfFitsButton("    Compare both");
    if (ImGui::Button(ICON_FA_GAUGE "  Compare both"))
        l.runComparison();
    registerLastItem("engine-lab:compare", "button", "Compare both", [&l]() { l.runComparison(); }, modelReady && drafterReady);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (modelReady && !drafterReady)
        ImGui::TextDisabled("Download the drafter to compare with speculative decoding.");
}

void EngineLabView::renderLatest()
{
    const std::vector<Record>& history = lab().history();
    if (history.empty())
        return;
    const Record& r = history.front();
    if (!section(ICON_FA_BOLT, "Latest run"))
        return;
    ImGui::TextDisabled("%s, %s%s", r.when.c_str(), r.result.backend.c_str(), r.settings.drafterPath.empty() ? "" : ", with drafter");
    if (!r.result.ok) {
        ImGui::TextColored(col(palette_.iconRed), "Failed: %s", r.result.error.c_str());
        return;
    }
    // Headline figures; four across on a wide window, two on a phone.
    const float avail = ImGui::GetContentRegionAvail().x;
    const float rowRight = ImGui::GetCursorPosX() + avail;
    const float cell = std::max(120.0f * s_, avail / (avail < 480.0f * s_ ? 2.0f : 4.0f));
    int shown = 0;
    auto place = [&](const char* label, const std::string& value, bool big) {
        if (shown > 0 && ImGui::GetCursorPosX() + cell > rowRight + 1.0f)
            ImGui::NewLine();
        stat(label, value, cell, big);
        ++shown;
    };
    place("Generating", format("%.1f tok/s", r.result.generateTokensPerSecond), true);
    place("Reading the prompt", format("%.0f tok/s", r.result.promptTokensPerSecond), true);
    place("First token", format("%.2f s", r.result.firstTokenSeconds), false);
    place("Model load", format("%.1f s", r.result.loadSeconds), false);
    if (r.result.peakMemory)
        place("Peak memory", gigabytes(r.result.peakMemory), false);
    if (r.result.drafted > 0)
        place("Drafts accepted", format("%.0f%%", 100.0 * r.result.accepted / r.result.drafted), false);
    ImGui::NewLine();
    // Against the newest run of the other kind on the same hardware: what the drafter buys.
    for (size_t i = 1; i < history.size(); ++i) {
        const Record& o = history[i];
        if (!o.result.ok || o.result.backend != r.result.backend || o.settings.drafterPath.empty() == r.settings.drafterPath.empty())
            continue;
        const bool withDrafter = !r.settings.drafterPath.empty();
        const double a = withDrafter ? r.result.generateTokensPerSecond : o.result.generateTokensPerSecond;
        const double b = withDrafter ? o.result.generateTokensPerSecond : r.result.generateTokensPerSecond;
        if (b > 0)
            ImGui::TextColored(col(a >= b ? palette_.iconGreen : palette_.iconOrange), "Drafter: %+.0f%% generating speed (%.1f vs %.1f tok/s)",
                               (a / b - 1.0) * 100.0, a, b);
        break;
    }
    if (!r.result.sample.empty()) {
        ImGui::TextDisabled("It wrote:");
        ImGui::PushFont(fonts_.mono, 0.0f);
        ImGui::TextWrapped("%s", r.result.sample.substr(0, 240).c_str());
        ImGui::PopFont();
    }
}

void EngineLabView::renderHistory()
{
    viewmodel::EngineLabViewModel& l = lab();
    const std::vector<Record>& history = l.history();
    if (history.empty())
        return;
    if (!section(ICON_FA_CLOCK_ROTATE_LEFT, "History"))
        return;

    // Bars of generating speed, newest first: the comparison at a glance.
    double best = 0;
    for (const Record& r : history)
        best = std::max(best, r.result.generateTokensPerSecond);
    const float labelWidth = std::min(220.0f * s_, ImGui::GetContentRegionAvail().x * 0.45f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const size_t shown = std::min<size_t>(history.size(), 12);
    for (size_t i = 0; i < shown; ++i) {
        const Record& r = history[i];
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float rowH = ImGui::GetTextLineHeightWithSpacing();
        std::string label = (r.result.backend.rfind("CPU", 0) == 0 ? "CPU" : "GPU") + std::string(r.settings.drafterPath.empty() ? "" : " + drafter");
        fit::text(dl, p, p.x + labelWidth - 6.0f * s_, palette_.textDim, label);
        const float barX = p.x + labelWidth, barMax = ImGui::GetContentRegionAvail().x - labelWidth - 70.0f * s_;
        if (r.result.ok && best > 0 && barMax > 10.0f) {
            const float w = std::max(2.0f, float(r.result.generateTokensPerSecond / best) * barMax);
            const ImU32 colour = r.settings.drafterPath.empty() ? palette_.iconBlue : palette_.iconGreen;
            dl->AddRectFilled(ImVec2(barX, p.y + rowH * 0.18f), ImVec2(barX + w, p.y + rowH * 0.82f), colour, 3.0f * s_);
            dl->AddText(ImVec2(barX + w + 6.0f * s_, p.y), palette_.text, format("%.1f", r.result.generateTokensPerSecond).c_str());
        } else {
            dl->AddText(ImVec2(barX, p.y), palette_.iconRed, r.result.ok ? "-" : "failed");
        }
        ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, rowH));
    }
    ImGui::TextDisabled("Generating speed, tokens/s, newest first.");

    if (ImGui::Button(ICON_FA_COPY "  Copy report"))
        ImGui::SetClipboardText(l.report().c_str());
    registerLastItem("engine-lab:copy", "button", "Copy report", [&l]() { ImGui::SetClipboardText(l.report().c_str()); });
    fit::sameLineIfFitsButton("Clear history");
    if (ImGui::Button("Clear history"))
        l.clearHistory();
}

} // namespace enp::view
