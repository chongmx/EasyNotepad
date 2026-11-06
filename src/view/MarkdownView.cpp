#include "enp/view/MarkdownView.h"

#include "enp/model/Emoji.h"
#include "enp/model/Highlighter.h"
#include "enp/model/Language.h"
#include "enp/view/CodeGrid.h"
#include "enp/view/MathView.h"

#include "IconsFontAwesome6.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <numeric>

namespace enp::view {

namespace md = model::markdown;

struct MarkdownView::Style {
    ImFont* ui = nullptr;
    ImFont* mono = nullptr;
    float size = 15.0f; // body text, in pixels
    float s = 1.0f;     // UI scale
    const Palette* palette = nullptr;
    ImU32 text = 0, dim = 0, link = 0, rule = 0, codeBg = 0, quoteBar = 0, tableHeaderBg = 0;
    const std::function<const MarkdownImage*(const std::string&)>* images = nullptr;
    EmojiCache* emoji = nullptr;
    std::map<std::string, model::math::Node>* math = nullptr;
    float lineH() const { return std::floor(size * 1.45f); }
    const model::math::Node& formula(const std::string& source) const
    {
        auto it = math->find(source);
        if (it == math->end())
            it = math->emplace(source, model::math::parse(source)).first;
        return it->second;
    }
};

namespace {

struct LinkHit {
    ImVec2 min, max;
    std::string url;
};

size_t utf8Length(unsigned char c)
{
    return c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
}

// Lays out styled text in lines within [left, right], drawing it when `draw` is set.
class Flow {
public:
    Flow(const MarkdownView::Style& style, ImDrawList* dl, bool draw, float left, float right, float y, float size, float lineH,
         ImU32 color, bool bold, std::vector<LinkHit>* links)
        : st_(style), dl_(dl), draw_(draw), left_(left), right_(right), x_(left), y_(y), size_(size), lineH_(lineH),
          color_(color), bold_(bold), links_(links)
    {
    }

    void spans(const md::Spans& spans)
    {
        for (const md::Span& span : spans)
            text(span);
    }

    float bottom() const { return lineEmpty_ ? y_ : y_ + lineH_; }
    float x() const { return x_; }

private:
    const MarkdownView::Style& st_;
    ImDrawList* dl_;
    bool draw_;
    float left_, right_, x_, y_, size_, lineH_;
    ImU32 color_;
    bool bold_;
    std::vector<LinkHit>* links_;
    // Code span background: one box per span and line, rounded only at its two ends.
    bool spanStart_ = false, spanEnd_ = false;
    float lineStartY_ = -1.0f; // y of the line the last code piece was on
    bool lineEmpty_ = false;   // after an image: the next line has nothing on it yet

    void newline()
    {
        x_ = left_;
        y_ += lineH_;
    }

    // An image on lines of its own, at its pixel size (scaled with the UI) within the column.
    void image(const md::Span& span)
    {
        const MarkdownImage* img = st_.images && *st_.images ? (*st_.images)(span.url) : nullptr;
        if (!img || img->failed || !img->texture) {
            placeholder(span, img ? img->note : std::string("images are not shown here"));
            return;
        }
        const float maxW = right_ - left_;
        float w = float(img->width) * st_.s, h = float(img->height) * st_.s;
        if (w > maxW) {
            h *= maxW / w;
            w = maxW;
        }
        if (x_ > left_ || lineEmpty_)
            newline(), lineEmpty_ = false;
        const float top = y_;
        if (draw_)
            dl_->AddImage(img->texture->GetTexRef(), ImVec2(x_, top), ImVec2(x_ + w, top + h));
        // Text goes on below it; the block ends there when nothing follows.
        x_ = left_;
        y_ = top + h + 4.0f * st_.s;
        lineEmpty_ = true;
    }

    // An image that cannot be shown: a framed box with the picture icon, the alt text and the
    // reason, the source on hover. It takes the room a small image would, so the layout reads
    // the same with and without it.
    void placeholder(const md::Span& span, const std::string& note)
    {
        const std::string title = span.text.empty() ? span.url : span.text;
        const std::string detail = note.empty() ? span.url : note + (span.url.empty() ? std::string() : "  ·  " + span.url);
        const float pad = 8.0f * st_.s;
        const float small = size_ * 0.85f;
        const float iconW = measure(st_.ui, size_, ICON_FA_IMAGE, ICON_FA_IMAGE + sizeof(ICON_FA_IMAGE) - 1) + pad;
        const float maxW = right_ - left_;
        const float wantW = std::max({160.0f * st_.s, iconW + measure(st_.ui, size_, title.data(), title.data() + title.size()) + pad * 2.0f,
                                      measure(st_.ui, small, detail.data(), detail.data() + detail.size()) + pad * 2.0f});
        const float w = std::min(maxW, wantW);
        const float h = lineH_ + small * 1.3f + pad * 2.0f;
        if (x_ > left_ || lineEmpty_)
            newline(), lineEmpty_ = false;
        const float top = y_;
        if (draw_) {
            const ImVec2 a(x_, top), b(x_ + w, top + h);
            dl_->AddRectFilled(a, b, st_.codeBg, 4.0f * st_.s);
            dl_->AddRect(a, b, st_.rule, 4.0f * st_.s, 1.0f * st_.s);
            dl_->AddText(st_.ui, size_, ImVec2(x_ + pad, top + pad), st_.dim, ICON_FA_IMAGE);
            // The texts are clipped to the box.
            dl_->PushClipRect(a, b, true);
            dl_->AddText(st_.ui, size_, ImVec2(x_ + pad + iconW, top + pad), st_.dim, title.c_str());
            dl_->AddText(st_.ui, small, ImVec2(x_ + pad, top + pad + lineH_), st_.palette->textFaint, detail.c_str());
            dl_->PopClipRect();
            if (!span.url.empty() && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, b))
                ImGui::SetTooltip("%s", span.url.c_str());
        }
        x_ = left_;
        y_ = top + h + 4.0f * st_.s;
        lineEmpty_ = true;
    }

    // Inline math, on the text's baseline; a formula taller than the line (a fraction) shrinks to it.
    void math(const md::Span& span)
    {
        if (!st_.math)
            return;
        const model::math::Node& node = st_.formula(span.text);
        MathStyle ms;
        ms.font = st_.ui;
        ms.size = size_;
        ms.scale = st_.s;
        ms.color = color_;
        ms.display = false;
        MathBox box = measureMath(node, ms);
        if (box.height() > lineH_ && box.height() > 0.0f) {
            ms.size *= lineH_ / box.height();
            box = measureMath(node, ms);
        }
        const float w = box.width + 0.1f * size_;
        if (x_ > left_ && x_ + w > right_ + 0.5f)
            newline();
        lineEmpty_ = false;
        if (draw_) {
            ImFontBaked* baked = st_.ui->GetFontBaked(size_);
            const float ascent = baked ? baked->Ascent : size_ * 0.8f;
            const float baseline = y_ + (lineH_ - size_) * 0.5f + ascent;
            drawMath(dl_, node, ImVec2(x_ + 0.05f * size_, baseline), ms);
        }
        x_ += w;
    }

    // An emoji: its picture at text height on the line, or (a sequence the pack has no picture for)
    // its parts side by side, or the base without the skin tone; as a last resort its characters.
    // Measuring and drawing take the same branch: has() answers the same in both.
    void emoji(const md::Span& span)
    {
        EmojiCache* cache = st_.emoji;
        if (!cache) {
            md::Span plain = span;
            plain.style &= ~unsigned(md::Emoji);
            text(plain);
            return;
        }
        if (cache->has(span.emoji)) {
            picture(span, span.emoji);
            return;
        }
        if (span.emoji.find("_200d_") != std::string::npos) {
            bool any = false;
            for (const std::string& part : model::emoji::components(span.emoji)) {
                if (cache->has(part)) {
                    picture(span, part);
                    any = true;
                } else if (const std::string base = model::emoji::withoutModifiers(part); base != part && cache->has(base)) {
                    picture(span, base);
                    any = true;
                }
            }
            if (any)
                return;
        }
        if (const std::string base = model::emoji::withoutModifiers(span.emoji); base != span.emoji && cache->has(base)) {
            picture(span, base);
            return;
        }
        md::Span plain = span;
        plain.style &= ~unsigned(md::Emoji);
        text(plain);
    }

    void picture(const md::Span& span, const std::string& key)
    {
        const float box = std::floor(size_ * 1.2f);
        const float advance = box + std::floor(size_ * 0.08f);
        if (x_ > left_ && x_ + advance > right_ + 0.5f)
            newline();
        lineEmpty_ = false;
        if (draw_) {
            const float top = y_ + (lineH_ - box) * 0.5f;
            if (ImTextureData* tex = st_.emoji->texture(key))
                dl_->AddImage(tex->GetTexRef(), ImVec2(x_, top), ImVec2(x_ + box, top + box));
            if ((span.style & md::Link) && links_)
                links_->push_back({ImVec2(x_, y_), ImVec2(x_ + advance, y_ + lineH_), span.url});
        }
        x_ += advance;
    }

    void text(const md::Span& span)
    {
        if (span.style & md::Emoji) {
            emoji(span);
            return;
        }
        if (span.style & md::Image) {
            image(span);
            return;
        }
        if (span.style & md::Math) {
            math(span);
            return;
        }
        if (lineEmpty_ && !span.text.empty())
            lineEmpty_ = false;
        const bool code = (span.style & md::Code) != 0;
        ImFont* font = code ? st_.mono : st_.ui;
        const float size = code ? size_ * 0.92f : size_;
        const std::string& t = span.text;
        size_t i = 0;
        while (i < t.size()) {
            if (t[i] == '\n') {
                newline();
                ++i;
                continue;
            }
            // A word and the spaces after it.
            size_t end = i;
            while (end < t.size() && t[end] != ' ' && t[end] != '\n')
                ++end;
            size_t spaces = end;
            while (spaces < t.size() && t[spaces] == ' ')
                ++spaces;
            if (code && spaces < t.size() && t[spaces] != '\n')
                end = spaces; // inside code the spaces keep the background (one box, not one per word)
            spanStart_ = i == 0 || t[i - 1] == '\n';
            spanEnd_ = end >= t.size() || t[end] == '\n' || spaces >= t.size();
            word(t.data() + i, t.data() + end, spaces - end, font, size, span);
            i = spaces;
        }
    }

    float measure(ImFont* font, float size, const char* b, const char* e) const
    {
        return font->CalcTextSizeA(size, FLT_MAX, 0.0f, b, e).x;
    }

    void word(const char* b, const char* e, size_t spaces, ImFont* font, float size, const md::Span& span)
    {
        const bool bold = bold_ || (span.style & md::Bold) != 0;
        // Bold is the text drawn twice, the second copy whole pixels to the right: ImGui starts text
        // on a whole pixel, so a fractional offset fell on the same pixel for some words (normal
        // weight) and on the next for others (heavy).
        const float extra = bold ? std::max(1.0f, std::round(size / 22.0f)) : 0.0f;
        const float spaceW = measure(font, size, " ", " " + 1);
        float w = measure(font, size, b, e) + extra;
        const float avail = right_ - left_;
        if (x_ > left_ && x_ + w > right_ + 0.5f) // (the half pixel: a width measured to fit exactly)
            newline();
        // A word longer than the line: break it between characters. The same half pixel: a right-
        // aligned table cell is exactly its text's width, give or take rounding.
        while (w > avail + 0.5f && b < e) {
            const char* cut = b;
            float used = 0.0f;
            while (cut < e) {
                const size_t n = std::min<size_t>(utf8Length(static_cast<unsigned char>(*cut)), size_t(e - cut));
                const float cw = measure(font, size, cut, cut + n);
                if (used + cw > right_ - x_ + 0.5f && cut > b)
                    break;
                used += cw;
                cut += n;
            }
            piece(b, cut, used + extra, font, size, span, bold, extra);
            newline();
            b = cut;
            w = measure(font, size, b, e) + extra;
        }
        if (b < e)
            piece(b, e, w, font, size, span, bold, extra);
        // Spaces: dropped at the end of a line.
        const float trailing = spaceW * float(spaces);
        if (spaces > 0 && x_ + trailing <= right_) {
            if (draw_ && (span.style & (md::Link | md::Strike)) != 0 && b < e)
                decorate(x_, x_ + trailing, span); // underline continues under the gap
            x_ += trailing;
        }
    }

    void piece(const char* b, const char* e, float w, ImFont* font, float size, const md::Span& span, bool bold, float extra)
    {
        const float top = y_ + (lineH_ - size) * 0.5f;
        if (draw_) {
            const bool code = (span.style & md::Code) != 0;
            const bool link = (span.style & md::Link) != 0;
            const bool italic = (span.style & md::Italic) != 0;
            ImU32 color = link ? st_.link : code ? st_.palette->synString : italic && !bold ? st_.dim : color_;
            if (code) {
                const float pad = 2.0f * st_.s;
                const bool first = spanStart_ || x_ <= left_ || lineStartY_ != y_; // starts the box on this line
                const bool last = spanEnd_;
                ImDrawFlags corners = ImDrawFlags_RoundNone;
                if (first && last)
                    corners = ImDrawFlags_RoundAll;
                else if (first)
                    corners = ImDrawFlags_RoundLeft;
                else if (last)
                    corners = ImDrawFlags_RoundRight;
                dl_->AddRectFilled(ImVec2(x_ - (first ? pad : 0.0f), top - 1.0f * st_.s),
                                   ImVec2(x_ + w + (last ? pad : 0.0f), top + size + 2.0f * st_.s), st_.codeBg, 3.0f * st_.s, corners);
                lineStartY_ = y_;
                spanStart_ = false;
            }
            const float textX = std::floor(x_); // where ImGui starts it; the bold copy keeps its offset
            dl_->AddText(font, size, ImVec2(textX, top), color, b, e);
            if (bold)
                dl_->AddText(font, size, ImVec2(textX + extra, top), color, b, e);
            decorate(x_, x_ + w, span);
            if (link && links_)
                links_->push_back({ImVec2(x_, y_), ImVec2(x_ + w, y_ + lineH_), span.url});
        }
        x_ += w;
    }

    void decorate(float x0, float x1, const md::Span& span)
    {
        const float mid = y_ + lineH_ * 0.5f;
        if (span.style & md::Strike)
            dl_->AddLine(ImVec2(x0, mid), ImVec2(x1, mid), color_, 1.0f * st_.s);
        if (span.style & md::Link) {
            const float base = y_ + (lineH_ + size_) * 0.5f;
            dl_->AddLine(ImVec2(x0, base), ImVec2(x1, base), (st_.link & 0x00FFFFFFu) | 0x80000000u, 1.0f * st_.s);
        }
    }
};

float headingScale(int level)
{
    static const float scales[] = {1.0f, 1.6f, 1.35f, 1.18f, 1.06f, 1.0f, 0.95f};
    return scales[std::clamp(level, 0, 6)];
}

// Space above a block, given the one before it.
float gapBefore(const md::Block* previous, const md::Block& block, float lineH)
{
    if (!previous)
        return 0.0f;
    auto listy = [](const md::Block& b) {
        return b.kind == md::BlockKind::Bullet || b.kind == md::BlockKind::Numbered || b.kind == md::BlockKind::Task;
    };
    if (listy(*previous) && listy(block))
        return lineH * 0.12f;
    if (block.kind == md::BlockKind::Heading)
        return lineH * (block.level <= 2 ? 0.9f : 0.7f);
    return lineH * 0.55f;
}

model::Language codeLanguage(const std::string& name)
{
    if (name.empty())
        return model::Language::PlainText;
    model::Language language = model::languageFromKey(name);
    if (language == model::Language::PlainText)
        language = model::languageFromPath("x." + name);
    return language;
}

} // namespace

float MarkdownView::offsetOfLine(int line) const
{
    float y = 0.0f;
    for (size_t i = 0; i < blocks_.size() && i < tops_.size(); ++i) {
        if (blocks_[i].line > line)
            break;
        y = tops_[i];
    }
    return y;
}

float MarkdownView::drawBlock(const md::Block& block, ImVec2 at, float width, const Style& st, bool draw)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::vector<LinkHit> links;
    const float lineH = st.lineH();
    const float quoteIndent = float(block.quote) * 14.0f * st.s;
    const float left = at.x + quoteIndent;
    const float right = at.x + width;
    const ImU32 textColor = block.quote > 0 ? st.dim : st.text;
    float height = 0.0f;

    switch (block.kind) {
    case md::BlockKind::Heading: {
        const float size = st.size * headingScale(block.level);
        const float h = std::floor(size * 1.35f);
        Flow flow(st, dl, draw, left, right, at.y, size, h, block.level >= 6 ? st.dim : st.text, true, &links);
        flow.spans(block.spans);
        height = flow.bottom() - at.y;
        if (block.level <= 2) {
            height += 4.0f * st.s;
            if (draw)
                dl->AddLine(ImVec2(left, at.y + height - 1.0f), ImVec2(right, at.y + height - 1.0f), st.rule, 1.0f);
            height += 2.0f * st.s;
        }
        break;
    }
    case md::BlockKind::Paragraph: {
        Flow flow(st, dl, draw, left, right, at.y, st.size, lineH, textColor, false, &links);
        flow.spans(block.spans);
        height = flow.bottom() - at.y;
        break;
    }
    case md::BlockKind::Bullet:
    case md::BlockKind::Numbered:
    case md::BlockKind::Task: {
        const float indentW = st.size * 1.4f;
        const float markerX = left + float(block.level) * indentW;
        float markerW = indentW;
        if (block.kind == md::BlockKind::Numbered)
            markerW = std::max(indentW, st.ui->CalcTextSizeA(st.size, FLT_MAX, 0.0f, block.marker.c_str()).x + st.size * 0.5f);
        const float textX = markerX + markerW;
        if (draw) {
            const float mid = at.y + lineH * 0.5f;
            if (block.kind == md::BlockKind::Bullet) {
                const float r = std::max(2.0f, st.size * 0.16f);
                const ImVec2 c(markerX + indentW * 0.4f, mid);
                if (block.level == 0)
                    dl->AddCircleFilled(c, r, textColor);
                else if (block.level == 1)
                    dl->AddCircle(c, r, textColor, 0, 1.2f * st.s);
                else
                    dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), textColor);
            } else if (block.kind == md::BlockKind::Numbered) {
                const float w = st.ui->CalcTextSizeA(st.size, FLT_MAX, 0.0f, block.marker.c_str()).x;
                dl->AddText(st.ui, st.size, ImVec2(textX - w - st.size * 0.35f, at.y + (lineH - st.size) * 0.5f), st.dim,
                            block.marker.c_str());
            } else {
                const char* icon = block.checked ? ICON_FA_SQUARE_CHECK : ICON_FA_SQUARE;
                dl->AddText(st.ui, st.size, ImVec2(markerX + 1.0f * st.s, at.y + (lineH - st.size) * 0.5f),
                            block.checked ? st.palette->iconGreen : st.dim, icon);
            }
        }
        Flow flow(st, dl, draw, textX, right, at.y, st.size, lineH, block.checked ? st.dim : textColor, false, &links);
        flow.spans(block.spans);
        height = flow.bottom() - at.y;
        break;
    }
    case md::BlockKind::Math: {
        // Display math: centred, a little larger, limits above and below the big operators.
        const model::math::Node& node = st.formula(block.code);
        MathStyle ms;
        ms.font = st.ui;
        ms.size = st.size * 1.1f;
        ms.scale = st.s;
        ms.color = textColor;
        ms.display = true;
        MathBox box = measureMath(node, ms);
        if (box.width > right - left && box.width > 0.0f) {
            ms.size *= (right - left) / box.width;
            box = measureMath(node, ms);
        }
        const float pad = 6.0f * st.s;
        height = box.height() + pad * 2.0f;
        if (draw)
            drawMath(dl, node, ImVec2(left + (right - left - box.width) * 0.5f, at.y + pad + box.ascent), ms);
        break;
    }
    case md::BlockKind::Rule:
        height = lineH * 0.6f;
        if (draw)
            dl->AddLine(ImVec2(left, at.y + height * 0.5f), ImVec2(right, at.y + height * 0.5f), st.rule, 1.0f * st.s);
        break;
    case md::BlockKind::Code: {
        const float pad = 8.0f * st.s;
        const float size = st.size * 0.92f;
        const float h = std::floor(size * 1.4f);
        // A grid of cells one character wide (layoutCodeLine): ASCII art and box drawing line up.
        const float charW = st.mono->CalcTextSizeA(size, FLT_MAX, 0.0f, "M").x;
        const int columns = std::max(8, int((right - left - pad * 2.0f) / std::max(1.0f, charW)));
        // Lines wrap at the panel's width (code is narrow in the chat).
        std::vector<std::pair<size_t, size_t>> lines; // [begin, end) in block.code
        for (size_t b = 0; b <= block.code.size();) {
            size_t e = block.code.find('\n', b);
            if (e == std::string::npos)
                e = block.code.size();
            lines.emplace_back(b, e);
            b = e + 1;
            if (e == block.code.size())
                break;
        }
        int rows = 0;
        std::string line, emojiKey;
        for (const auto& [b, e] : lines) {
            line.assign(block.code, b, e - b);
            rows += layoutCodeLine(line, columns, emojiKey, [](const CodeCell&) {});
        }
        height = pad * 2.0f + float(rows) * h;
        if (!draw)
            break;
        const ImVec2 boxMin(left, at.y), boxMax(right, at.y + height);
        dl->AddRectFilled(boxMin, boxMax, st.codeBg, 6.0f * st.s);
        const model::LanguageSpec& spec = model::languageSpec(codeLanguage(block.language));
        int state = 0;
        std::vector<model::Token> tokens;
        float y = at.y + pad;
        const ImVec2 clipMin = dl->GetClipRectMin(), clipMax = dl->GetClipRectMax();
        ImFontBaked* baked = st.mono->GetFontBaked(size);
        const float lineWidth = size / 14.0f; // box drawing: 1 px at the default size, thicker zoomed in
        for (const auto& [b, e] : lines) {
            line.assign(block.code, b, e - b);
            model::highlightLine(line, spec, state, tokens);
            const float top = y;
            auto colorAt = [&](size_t byte) {
                return st.palette->syntax[size_t(byte < tokens.size() ? tokens[byte] : model::Token::Text)];
            };
            // ASCII is drawn in runs of one colour on one row (each character is exactly one cell);
            // anything else on its own, in its cells.
            size_t runBegin = 0, runEnd = 0;
            int runColumn = 0, runRow = 0;
            auto flush = [&]() {
                const float ry = top + float(runRow) * h;
                if (runEnd > runBegin && ry + h >= clipMin.y && ry <= clipMax.y)
                    dl->AddText(st.mono, size, ImVec2(left + pad + float(runColumn) * charW, ry + (h - size) * 0.5f),
                                colorAt(runBegin), line.c_str() + runBegin, line.c_str() + runEnd);
                runBegin = runEnd = 0;
            };
            const int lineRows = layoutCodeLine(line, columns, emojiKey, [&](const CodeCell& cell) {
                if (cell.c < 0x80 && cell.c != '\t' && cell.c != '\r' && !cell.emoji) {
                    if (runEnd == cell.byte && runEnd > runBegin && cell.row == runRow && colorAt(cell.byte) == colorAt(runBegin)) {
                        runEnd += 1;
                        return;
                    }
                    flush();
                    runBegin = cell.byte, runEnd = cell.byte + 1, runColumn = cell.column, runRow = cell.row;
                    return;
                }
                flush();
                const float cy = top + float(cell.row) * h;
                if (cell.cells == 0 || cell.c == '\t' || cy + h < clipMin.y || cy > clipMax.y)
                    return;
                const ImVec2 cellMin(left + pad + float(cell.column) * charW, cy), cellMax(cellMin.x + float(cell.cells) * charW, cy + h);
                const ImU32 color = colorAt(cell.byte);
                if (drawBoxCharacter(dl, cell.c, cellMin, cellMax, color, lineWidth))
                    return;
                if (cell.emoji && st.emoji && st.emoji->has(*cell.emoji)) { // its picture, centred in its two cells
                    const float box = std::min(std::floor(size * 1.2f), std::min(cellMax.x - cellMin.x, h));
                    const ImVec2 p(std::floor((cellMin.x + cellMax.x - box) * 0.5f), std::floor(cy + (h - box) * 0.5f));
                    if (ImTextureData* tex = st.emoji->texture(*cell.emoji))
                        dl->AddImage(tex->GetTexRef(), p, ImVec2(p.x + box, p.y + box));
                    return;
                }
                // From a fallback font a glyph may be wider than its cells: shrunk to fit, so the
                // grid holds. Narrower ones are centred.
                const float room = cellMax.x - cellMin.x;
                float advance = cell.c <= 0xFFFF ? baked->GetCharAdvance(ImWchar(cell.c)) : room;
                float drawSize = size;
                if (advance > room + 0.5f) {
                    drawSize = size * room / advance;
                    advance = room;
                }
                const char* s = line.c_str() + cell.byte;
                dl->AddText(st.mono, drawSize, ImVec2(cellMin.x + (room - advance) * 0.5f, cy + (h - drawSize) * 0.5f), color, s,
                            s + cell.length);
            });
            flush();
            y += float(lineRows) * h;
        }
        // Copy button, on hover.
        if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(boxMin, boxMax)) {
            const float bs = st.size * 1.5f;
            const ImVec2 bMin(boxMax.x - bs - 4.0f * st.s, boxMin.y + 4.0f * st.s), bMax(bMin.x + bs, bMin.y + bs);
            const bool over = ImGui::IsMouseHoveringRect(bMin, bMax);
            dl->AddRectFilled(bMin, bMax, over ? st.palette->hover : st.codeBg, 4.0f * st.s);
            const ImVec2 iconSize = st.ui->CalcTextSizeA(st.size, FLT_MAX, 0.0f, ICON_FA_COPY);
            dl->AddText(st.ui, st.size, ImVec2(bMin.x + (bs - iconSize.x) * 0.5f, bMin.y + (bs - iconSize.y) * 0.5f), st.dim,
                        ICON_FA_COPY);
            if (over) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("Copy");
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    ImGui::SetClipboardText(block.code.c_str());
            }
        }
        break;
    }
    case md::BlockKind::Table: {
        const size_t columns = block.align.size();
        if (columns == 0 || block.rows.empty())
            break;
        const float pad = 6.0f * st.s;
        // A cell's width on one line, as drawn (code font, bold header).
        auto oneLine = [&](size_t r, size_t c) {
            if (c >= block.rows[r].size())
                return 0.0f;
            Flow line(st, dl, false, 0.0f, FLT_MAX * 0.25f, 0.0f, st.size, lineH, st.text, r == 0, nullptr);
            line.spans(block.rows[r][c]);
            return line.x();
        };
        std::vector<float> natural(columns, 0.0f);
        for (size_t r = 0; r < block.rows.size(); ++r)
            for (size_t c = 0; c < columns; ++c)
                natural[c] = std::max(natural[c], oneLine(r, c) + pad * 2.0f + 2.0f * st.s);
        float total = 0.0f;
        for (float w : natural)
            total += w;
        const float avail = right - left;
        std::vector<float> widths = natural;
        if (total > avail) { // shrink, wrapping the cells
            const float minW = st.size * 3.0f;
            const float scale = avail / total;
            for (float& w : widths)
                w = std::max(minW, w * scale);
        }
        float y = at.y;
        std::vector<float> rowTops;
        for (size_t r = 0; r < block.rows.size(); ++r) {
            // Measure the row, then draw it.
            float rowH = lineH;
            float x = left;
            for (size_t c = 0; c < columns; ++c) {
                Flow measure(st, dl, false, x + pad, x + widths[c] - pad, y + pad * 0.5f, st.size, lineH, st.text, r == 0, nullptr);
                if (c < block.rows[r].size())
                    measure.spans(block.rows[r][c]);
                rowH = std::max(rowH, measure.bottom() - y + pad * 0.5f);
                x += widths[c];
            }
            if (draw) {
                if (r == 0)
                    dl->AddRectFilled(ImVec2(left, y), ImVec2(x, y + rowH), st.tableHeaderBg);
                x = left;
                for (size_t c = 0; c < columns; ++c) {
                    float cellLeft = x + pad;
                    if (block.align[c] != -1) { // centre / right: from the single-line width
                        const float room = widths[c] - pad * 2.0f - oneLine(r, c);
                        if (room > 0.0f)
                            cellLeft += block.align[c] == 0 ? room * 0.5f : room;
                    }
                    Flow cell(st, dl, true, cellLeft, x + widths[c] - pad, y + pad * 0.5f, st.size, lineH, st.text, r == 0, &links);
                    if (c < block.rows[r].size())
                        cell.spans(block.rows[r][c]);
                    x += widths[c];
                }
                dl->AddLine(ImVec2(left, y + rowH), ImVec2(x, y + rowH), st.rule, 1.0f);
            }
            rowTops.push_back(y);
            y += rowH;
        }
        if (draw) {
            float x = left;
            dl->AddRect(ImVec2(left, at.y), ImVec2(left + std::accumulate(widths.begin(), widths.end(), 0.0f), y), st.rule, 0.0f, 1.0f);
            for (size_t c = 0; c + 1 < columns; ++c) {
                x += widths[c];
                dl->AddLine(ImVec2(x, at.y), ImVec2(x, y), st.rule, 1.0f);
            }
        }
        height = y - at.y;
        break;
    }
    }

    if (draw) {
        // Quote bars, one per level.
        for (int q = 0; q < block.quote; ++q) {
            const float x = at.x + float(q) * 14.0f * st.s + 3.0f * st.s;
            dl->AddRectFilled(ImVec2(x, at.y - 2.0f * st.s), ImVec2(x + 3.0f * st.s, at.y + height + 2.0f * st.s), st.quoteBar);
        }
        // Links: hand cursor, target as tooltip, click.
        if (ImGui::IsWindowHovered())
            for (const LinkHit& hit : links)
                if (ImGui::IsMouseHoveringRect(hit.min, hit.max)) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("%s", hit.url.c_str());
                    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 16.0f && onLink)
                        onLink(hit.url);
                    break;
                }
    }
    return height;
}

void MarkdownView::render(const std::string& text, float width, const Fonts& fonts, const Palette& palette, float fontSize,
                          float uiScale)
{
    Style st;
    st.ui = fonts.ui;
    st.mono = fonts.mono ? fonts.mono : fonts.ui;
    ImGui::PushFont(fonts.ui, fontSize);
    st.size = ImGui::GetFontSize(); // with the global scale applied
    ImGui::PopFont();
    st.s = uiScale;
    st.palette = &palette;
    st.text = palette.text;
    st.dim = palette.textDim;
    st.link = palette.iconBlue;
    st.rule = palette.border;
    st.codeBg = IM_COL32(128, 128, 128, 34);
    st.quoteBar = (palette.accent & 0x00FFFFFFu) | 0x90000000u;
    st.tableHeaderBg = IM_COL32(128, 128, 128, 26);
    st.images = &onImage;
    st.emoji = emoji;
    st.math = &math_;
    width = std::max(width, 40.0f * uiScale);

    if (text != text_) {
        text_ = text;
        blocks_ = md::parse(text_);
        math_.clear();
        width_ = -1.0f;
    }
    // Measure every block when the layout changed (text, width or size).
    if (width != width_ || st.size != fontSize_ || tops_.size() != blocks_.size()) {
        width_ = width;
        fontSize_ = st.size;
        tops_.assign(blocks_.size(), 0.0f);
        heights_.assign(blocks_.size(), 0.0f);
        float y = 0.0f;
        for (size_t i = 0; i < blocks_.size(); ++i) {
            y += gapBefore(i > 0 ? &blocks_[i - 1] : nullptr, blocks_[i], st.lineH());
            tops_[i] = y;
            heights_[i] = drawBlock(blocks_[i], ImVec2(0.0f, 0.0f), width, st, false);
            y += heights_[i];
        }
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float clipTop = dl->GetClipRectMin().y, clipBottom = dl->GetClipRectMax().y;
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool hovered = ImGui::IsWindowHovered();
    hoveredLine_ = -1;
    for (size_t i = 0; i < blocks_.size(); ++i) {
        const float top = origin.y + tops_[i];
        const float bottom = top + heights_[i];
        if (hovered && mouse.y >= top - 4.0f && mouse.y < bottom + 4.0f && mouse.x >= origin.x && mouse.x <= origin.x + width)
            hoveredLine_ = blocks_[i].line;
        if (bottom < clipTop || top > clipBottom)
            continue; // off screen
        heights_[i] = drawBlock(blocks_[i], ImVec2(origin.x, top), width, st, true);
    }
    const float total = blocks_.empty() ? 0.0f : tops_.back() + heights_.back();
    ImGui::Dummy(ImVec2(width, std::max(total, 1.0f)));
}

} // namespace enp::view
