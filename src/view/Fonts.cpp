#include "enp/view/Fonts.h"

#include "enp/model/viewer/Viewer.h"
#include "enp/os/Memory.h"
#include "enp/os/Os.h"

#include "IconsFontAwesome6.h"
#include "imgui_internal.h" // a merged source removed again (ImFontAtlasFontDiscardBakes and the like)

#include <cstdint>
#include <cstring>

namespace enp::view {

bool fontReadable(const void* data, std::size_t size)
{
    const auto* p = static_cast<const unsigned char*>(data);
    if (!p || size < 12)
        return false;
    auto u16 = [p](std::size_t at) { return std::uint32_t(p[at]) << 8 | p[at + 1]; };
    auto u32 = [&u16](std::size_t at) { return u16(at) << 16 | u16(at + 2); };
    std::size_t face = 0;
    if (std::memcmp(p, "ttcf", 4) == 0) { // a collection: its first face
        if (size < 16 || (u32(4) != 0x00010000 && u32(4) != 0x00020000) || u32(8) == 0)
            return false;
        face = u32(12);
        if (face > size - 12)
            return false;
    }
    const std::uint32_t version = u32(face);
    if (version != 0x00010000 && std::memcmp(p + face, "OTTO", 4) != 0 && std::memcmp(p + face, "true", 4) != 0 &&
        std::memcmp(p + face, "typ1", 4) != 0 && std::memcmp(p + face, "1\0\0\0", 4) != 0)
        return false;
    const std::size_t tables = u16(face + 4);
    if (tables > (size - face - 12) / 16)
        return false;
    auto has = [&](const char* tag) {
        for (std::size_t i = 0; i < tables; ++i) {
            const std::size_t entry = face + 12 + 16 * i;
            if (std::memcmp(p + entry, tag, 4) == 0)
                return u32(entry + 8) <= size && u32(entry + 12) <= size - u32(entry + 8);
        }
        return false;
    };
    if (!has("cmap") || !has("head") || !has("hhea") || !has("hmtx"))
        return false;
    if (has("glyf"))
        return has("loca");
    return has("CFF "); // CFF2 (variable outlines) is not read
}

namespace {

// ImGui takes ownership of TTF data and frees it with IM_FREE, so copy into IM_ALLOC'd memory.
ImFont* addFont(const char* asset, float size, const ImFontConfig& config, const ImWchar* ranges = nullptr)
{
    std::optional<std::string> data = os::fs::readAsset(asset);
    if (!data || !fontReadable(data->data(), data->size()))
        return nullptr;
    void* buffer = IM_ALLOC(data->size());
    std::memcpy(buffer, data->data(), data->size());
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(buffer, int(data->size()), size, &config, ranges);
}

// Glyphs the bundled fonts lack (terminal programs draw with box drawing, block elements, arrows,
// geometric shapes and braille: htop's sort arrow, btop's graphs) come from a system font when
// one is installed. With dynamic fonts only the glyphs actually used are rasterized.
// `monospace` (the code font): on Windows Consolas is merged before Segoe UI Symbol, so arrows and
// the like come one cell wide (Segoe's are proportional, its arrows 1.4 cells); Segoe still gives
// what Consolas lacks (braille, geometric shapes). Elsewhere the first candidate is monospace.
void mergeSymbolFallback(float size, bool monospace)
{
    static const char* const kCandidates[] = {
#if defined(_WIN32)
        "C:/Windows/Fonts/seguisym.ttf", "C:/Windows/Fonts/consola.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/Apple Symbols.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
#endif
    };
    auto merge = [size](const char* path) {
        std::optional<std::string> data = os::fs::readFile(path);
        if (!data || !fontReadable(data->data(), data->size()))
            return false;
        void* buffer = IM_ALLOC(data->size());
        std::memcpy(buffer, data->data(), data->size());
        ImFontConfig config;
        config.MergeMode = true;
        return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(buffer, int(data->size()), size, &config) != nullptr;
    };
#if defined(_WIN32)
    if (monospace) { // a merged font gives the glyphs that the fonts before it lack
        merge(kCandidates[1]);
        merge(kCandidates[0]);
        return;
    }
#else
    (void)monospace;
#endif
    for (const char* path : kCandidates)
        if (merge(path))
            return;
}

bool mergeIcons(float size)
{
    static const ImWchar iconRanges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
    ImFontConfig iconConfig;
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    iconConfig.GlyphMinAdvanceX = size; // fixed-width icons align nicely in buttons
    return addFont("fonts/" FONT_ICON_FILE_NAME_FAS, size, iconConfig, iconRanges) != nullptr;
}

} // namespace

Fonts Fonts::load()
{
    Fonts fonts;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    // The atlas is used for as long as the app runs: its texture is made 512 x 512 from the start
    // (1 MB of pixels) and kept, instead of growing from 512 x 128 as documents bring new sizes
    // and staying grown after they close. Replacing a texture costs memory in some GL drivers, so
    // it is not shrunk back either (docs/MEMORY.md rule 9).
    atlas->TexMinHeight = 512;

    ImFontConfig textConfig;
    textConfig.OversampleH = 2;
    fonts.ui = addFont("fonts/Roboto-Medium.ttf", kUiSize, textConfig);
    if (!fonts.ui)
        fonts.ui = atlas->AddFontDefaultVector();
    fonts.hasIcons = mergeIcons(kUiSize);
    mergeSymbolFallback(kUiSize, false); // ∑ ∫ √ and other math glyphs Roboto lacks (rendered Markdown)

    fonts.mono = addFont("fonts/Cousine-Regular.ttf", kEditorSize, ImFontConfig());
    if (fonts.mono) {
        mergeIcons(kEditorSize);
        mergeSymbolFallback(kEditorSize, true);
    } else {
        fonts.mono = fonts.ui;
    }

    return fonts;
}

namespace {

// System fonts with Chinese, Japanese or Korean letters, best first; "~/" is the home directory.
struct ScriptFont {
    const char* path;
    std::uint8_t scripts;
};
constexpr std::uint8_t kHan = model::viewer::ScriptHan, kHangul = model::viewer::ScriptHangul, kCjk = kHan | kHangul;
const ScriptFont kScriptFonts[] = {
#if defined(_WIN32)
    {"C:/Windows/Fonts/msyh.ttc", kHan}, {"C:/Windows/Fonts/YuGothM.ttc", kHan}, {"C:/Windows/Fonts/simsun.ttc", kHan},
    {"C:/Windows/Fonts/malgun.ttf", kHangul}, {"C:/Windows/Fonts/gulim.ttc", kHangul},
#elif defined(__ANDROID__)
    // NotoSansCJK is a variable font with CFF2 outlines from Android 15 (not readable: skipped);
    // Samsung's own SECCJK, and NotoSerifCJK, have CFF outlines.
    {"/system/fonts/NotoSansCJK-Regular.ttc", kCjk}, {"/system/fonts/SECCJK-Regular.ttc", kCjk},
    {"/system/fonts/DroidSansFallback.ttf", kCjk}, {"/system/fonts/NotoSerifCJK-Regular.ttc", kCjk},
#elif defined(__APPLE__)
    {"/System/Library/Fonts/Hiragino Sans GB.ttc", kHan}, {"/System/Library/Fonts/STHeiti Light.ttc", kHan},
    {"/System/Library/Fonts/AppleSDGothicNeo.ttc", kHangul},
#else
    {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", kCjk}, {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", kCjk},
    {"/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", kCjk}, {"/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", kCjk},
    {"/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", kCjk}, {"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", kCjk},
    {"/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc", kCjk}, {"/usr/share/fonts/wqy-microhei/wqy-microhei.ttc", kCjk},
    {"~/.local/share/fonts/NotoSansCJK-Regular.ttc", kCjk}, {"~/.local/share/fonts/DroidSansFallbackFull.ttf", kCjk},
    {"~/.local/share/fonts/wqy-microhei.ttc", kCjk}, {"~/.fonts/NotoSansCJK-Regular.ttc", kCjk}, {"~/.fonts/DroidSansFallbackFull.ttf", kCjk},
#endif
};

// Bundled: Noto Sans CJK SC cut to the common letters (GB 2312, Big5 level 1, JIS level 1, KS X
// 1001 Hangul, kana, punctuation), CFF outlines; for a system without a CJK font ImGui can read.
// Made by scripts/fonts/build_cjk_font.py.
constexpr const char* kBundledCjkFont = "fonts/NotoSansCJK-Common.otf";

} // namespace

ScriptFonts::~ScriptFonts()
{
    for (Source& s : sources_)
        if (s.scripts)
            remove(s);
}

std::uint8_t ScriptFonts::merged() const
{
    std::uint8_t scripts = 0;
    for (const Source& s : sources_)
        scripts |= s.scripts;
    return scripts;
}

std::size_t ScriptFonts::mappedBytes() const
{
    std::size_t bytes = 0;
    for (const Source& s : sources_)
        bytes += s.size();
    return bytes;
}

void ScriptFonts::update(ImFont* font, std::uint8_t wanted)
{
    for (Source& s : sources_)
        if (s.scripts && (font != font_ || !(s.scripts & wanted)))
            remove(s);
    font_ = font;
    if (!wanted)
        missing_ = 0; // looked for again next time (a font may have been installed)
    std::uint8_t need = std::uint8_t(wanted & ~merged() & ~missing_);
    if (!font || !need)
        return;
    os::mem::Scope scope("fonts"); // the atlas's record of the source, and the bundled font's bytes
    auto freeSlot = [this]() { return !sources_[0].scripts ? &sources_[0] : !sources_[1].scripts ? &sources_[1] : nullptr; };
    // Merges the slot's font (its mapped file, or its bytes) into `font`; a font ImGui cannot read
    // is let go instead (ImGui would stop the app on it).
    auto merge = [font](Source& slot, std::uint8_t scripts) {
        const void* data = slot.data();
        const std::size_t size = slot.size();
        if (size <= std::size_t(INT32_MAX) && fontReadable(data, size)) {
            ImFontConfig config;
            config.MergeMode = true;
            config.DstFont = font;
            config.FontDataOwnedByAtlas = false;
            // Read only: ImGui's loader parses the font in place and never writes to it.
            if (ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<void*>(data), int(size), font->LegacySize, &config)) {
                slot.scripts = scripts;
                return true;
            }
        }
        slot.file.close();
        os::mem::release(slot.bytes);
        return false;
    };
    for (const ScriptFont& candidate : kScriptFonts) {
        if (!(candidate.scripts & need))
            continue;
        Source* slot = freeSlot();
        if (!slot)
            break;
        const std::string path = std::strncmp(candidate.path, "~/", 2) == 0 ? os::fs::homeDirectory() + (candidate.path + 1) : candidate.path;
        if (!slot->file.open(path) || !merge(*slot, candidate.scripts))
            continue;
        need = std::uint8_t(need & ~candidate.scripts);
        if (!need)
            break;
    }
    // No system font for them (or none ImGui can read): the bundled one, common letters only.
    if (need & kCjk) {
        if (Source* slot = freeSlot()) {
            if (std::optional<std::string> bytes = os::fs::readAsset(kBundledCjkFont)) {
                slot->bytes = std::move(*bytes);
                if (merge(*slot, kCjk))
                    need = std::uint8_t(need & ~kCjk);
            }
        }
    }
    missing_ = std::uint8_t(missing_ | need);
}

void ScriptFonts::remove(Source& source)
{
    ImFontAtlas* atlas = ImGui::GetCurrentContext() ? ImGui::GetIO().Fonts : nullptr;
    if (atlas && font_) {
        // As ImFontAtlas::RemoveFont does for a whole font, for one of its sources: the sizes baked
        // with it go first (each holds loader data per source), then the source itself.
        ImFontAtlasFontDiscardBakes(atlas, font_, 0);
        for (int i = 0; i < atlas->Sources.Size; ++i) {
            ImFontConfig* src = &atlas->Sources[i];
            if (src->FontData != source.data())
                continue;
            const ImFontLoader* loader = src->FontLoader ? src->FontLoader : atlas->FontLoader;
            if (loader && loader->FontSrcDestroy)
                loader->FontSrcDestroy(atlas, src);
            ImFontAtlasFontDestroySourceData(atlas, src);
            atlas->Sources.erase(src);
            ImFontAtlasBuildUpdatePointers(atlas); // the fonts' source lists, rebuilt from the atlas's
            break;
        }
    }
    source.scripts = 0;
    source.file.close();
    os::mem::release(source.bytes);
}

} // namespace enp::view
