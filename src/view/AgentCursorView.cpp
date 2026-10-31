#include "enp/view/AgentCursorView.h"

#include "enp/view/UiAutomation.h"

#include "IconsFontAwesome6.h"

#include <algorithm>
#include <cmath>

namespace enp::view {

namespace {

constexpr double kMoveSeconds = 0.7;
constexpr double kClickSeconds = 0.6;

ImU32 withAlpha(ImU32 c, float alpha)
{
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(v);
}

float easeOutCubic(float t)
{
    t = 1.0f - t;
    return 1.0f - t * t * t;
}

} // namespace

AgentCursorView::AgentCursorView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale)
    : vm_(vm), fonts_(fonts), palette_(palette), s_(uiScale)
{
}

void AgentCursorView::render()
{
    const viewmodel::AgentPointer& p = vm_.agentPointer.get();
    const float dt = ImGui::GetIO().DeltaTime;
    fade_ = std::clamp(fade_ + (p.visible ? dt : -dt) * 4.0f, 0.0f, 1.0f);
    if (fade_ <= 0.0f)
        return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const UiElement* target = UiAutomation::instance().find(p.target);
    const ImVec2 targetMin = target ? target->min : ImVec2(viewport->GetCenter().x - 1.0f, viewport->GetCenter().y - 1.0f);
    const ImVec2 targetMax = target ? target->max : ImVec2(targetMin.x + 2.0f, targetMin.y + 2.0f);
    // Aim slightly inside the element, the way a person points at a button. An element that went
    // away (the dialog its click closed) leaves the cursor where it was.
    const ImVec2 aim = !target && pos_.x >= 0.0f && p.serial == serial_
                           ? pos_
                           : ImVec2(targetMin.x + (targetMax.x - targetMin.x) * 0.55f, targetMin.y + (targetMax.y - targetMin.y) * 0.6f);
    const double now = ImGui::GetTime();
    if (p.serial != serial_) {
        serial_ = p.serial;
        moveStart_ = now;
        from_ = pos_.x < 0.0f ? ImVec2(viewport->Pos.x + viewport->Size.x * 0.75f, viewport->Pos.y + viewport->Size.y * 0.35f) : pos_;
    }
    const float t = float(std::clamp((now - moveStart_) / kMoveSeconds, 0.0, 1.0));
    const float e = easeOutCubic(t);
    pos_ = ImVec2(from_.x + (aim.x - from_.x) * e, from_.y + (aim.y - from_.y) * e);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImU32 glow = palette_.modeAgent;
    const float pulse = 0.5f + 0.5f * float(std::sin(now * 4.0));

    // Target highlight once the cursor has arrived.
    if (target && t >= 1.0f) {
        const float pad = 3.0f * s_;
        for (int i = 0; i < 3; ++i)
            dl->AddRect(ImVec2(targetMin.x - pad - float(i) * 2.0f * s_, targetMin.y - pad - float(i) * 2.0f * s_),
                        ImVec2(targetMax.x + pad + float(i) * 2.0f * s_, targetMax.y + pad + float(i) * 2.0f * s_),
                        withAlpha(glow, fade_ * (0.55f - 0.15f * float(i)) * (0.6f + 0.4f * pulse)), 7.0f * s_, 2.0f * s_);
    }

    // Click ripple.
    if (p.click && t >= 1.0f) {
        const float c = float((now - moveStart_ - kMoveSeconds) / kClickSeconds);
        if (c >= 0.0f && c <= 1.0f)
            dl->AddCircle(pos_, (8.0f + 30.0f * c) * s_, withAlpha(glow, fade_ * (1.0f - c)), 32, 3.0f * s_);
    }

    // Glow.
    for (int i = 6; i >= 1; --i)
        dl->AddCircleFilled(pos_, float(i) * 4.0f * s_ * (0.9f + 0.1f * pulse), withAlpha(glow, fade_ * 0.06f * float(7 - i) / 6.0f));

    // Arrow (classic pointer shape), white outline for contrast on any background.
    const float k = 1.15f * s_;
    const ImVec2 pts[] = {
        {pos_.x, pos_.y},
        {pos_.x, pos_.y + 17.0f * k},
        {pos_.x + 4.5f * k, pos_.y + 13.0f * k},
        {pos_.x + 7.5f * k, pos_.y + 19.5f * k},
        {pos_.x + 10.0f * k, pos_.y + 18.3f * k},
        {pos_.x + 7.2f * k, pos_.y + 12.0f * k},
        {pos_.x + 12.5f * k, pos_.y + 12.0f * k},
    };
    dl->AddConcavePolyFilled(pts, 7, withAlpha(glow, fade_));
    dl->AddPolyline(pts, 7, withAlpha(IM_COL32(255, 255, 255, 255), fade_), 1.5f * s_, ImDrawFlags_Closed);

    // Message bubble.
    if (!p.message.empty()) {
        ImGui::PushFont(fonts_.ui, Fonts::kUiSize * 0.95f);
        const float wrap = 300.0f * s_;
        const char* title = ICON_FA_ROBOT "  Agent";
        const ImVec2 titleSize = ImGui::CalcTextSize(title);
        const ImVec2 textSize = ImGui::CalcTextSize(p.message.c_str(), nullptr, false, wrap);
        const float padX = 10.0f * s_, padY = 8.0f * s_;
        const float w = std::max(textSize.x, titleSize.x) + padX * 2.0f;
        const float h = titleSize.y + textSize.y + padY * 2.0f + 3.0f * s_;
        ImVec2 b0(pos_.x + 16.0f * s_, pos_.y + 22.0f * s_);
        // Keep the bubble on screen.
        b0.x = std::min(b0.x, viewport->Pos.x + viewport->Size.x - w - 8.0f * s_);
        if (b0.y + h > viewport->Pos.y + viewport->Size.y - 8.0f * s_)
            b0.y = pos_.y - h - 10.0f * s_;
        const ImVec2 b1(b0.x + w, b0.y + h);
        dl->AddRectFilled(ImVec2(b0.x + 2.0f * s_, b0.y + 3.0f * s_), ImVec2(b1.x + 2.0f * s_, b1.y + 3.0f * s_),
                          withAlpha(IM_COL32(0, 0, 0, 255), fade_ * 0.25f), 9.0f * s_);
        dl->AddRectFilled(b0, b1, withAlpha(palette_.panelHeaderBg, fade_ * 0.97f), 9.0f * s_);
        dl->AddRect(b0, b1, withAlpha(glow, fade_ * 0.9f), 9.0f * s_, 1.5f * s_);
        dl->AddText(ImVec2(b0.x + padX, b0.y + padY), withAlpha(glow, fade_), title);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(b0.x + padX, b0.y + padY + titleSize.y + 3.0f * s_),
                    withAlpha(palette_.text, fade_), p.message.c_str(), nullptr, wrap);
        ImGui::PopFont();
    }
}

} // namespace enp::view
