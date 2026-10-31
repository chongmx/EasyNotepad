#include "enp/viewmodel/SyntaxColorSettings.h"

#include <cstdio>

namespace enp::viewmodel {

std::string SyntaxColorSettings::key(bool dark, const std::string& languageKey, model::Token token)
{
    return std::string(dark ? "dark" : "light") + "|" + languageKey + "|" + model::tokenKey(token);
}

SyntaxColorSettings::SyntaxColorSettings(Settings& settings) : settings_(settings)
{
    // {"syntaxColors": {"dark": {"cpp": {"keyword": "#c678dd"}}}}
    settings_.section(
        "syntaxColors",
        [this](const model::Json& colors) {
            colors_.clear();
            for (const auto& [theme, languages] : colors.members())
                for (const auto& [language, tokens] : languages.members())
                    for (const auto& [token, value] : tokens.members()) {
                        const model::Token t = model::tokenFromKey(token);
                        const std::string& hex = value.asString();
                        if (t == model::Token::Count || hex.size() != 7 || hex[0] != '#')
                            continue;
                        colors_[key(theme == "dark", language, t)] = uint32_t(std::stoul(hex.substr(1), nullptr, 16));
                    }
            ++revision_;
        },
        [this]() {
            std::map<std::string, std::map<std::string, std::map<std::string, std::string>>> tree;
            for (const auto& [k, rgb] : colors_) {
                const size_t a = k.find('|'), b = k.find('|', a + 1);
                char hex[8];
                std::snprintf(hex, sizeof(hex), "#%06x", unsigned(rgb & 0xFFFFFF));
                tree[k.substr(0, a)][k.substr(a + 1, b - a - 1)][k.substr(b + 1)] = hex;
            }
            model::Json colors = model::Json::object();
            for (const auto& [theme, languages] : tree) {
                model::Json l = model::Json::object();
                for (const auto& [language, tokens] : languages) {
                    model::Json t = model::Json::object();
                    for (const auto& [token, hex] : tokens)
                        t.set(token, hex);
                    l.set(language, std::move(t));
                }
                colors.set(theme, std::move(l));
            }
            return colors;
        });
}

SyntaxColorSettings::~SyntaxColorSettings()
{
    settings_.removeSection("syntaxColors");
}

std::optional<uint32_t> SyntaxColorSettings::color(bool dark, const std::string& languageKey, model::Token token) const
{
    const auto it = colors_.find(key(dark, languageKey, token));
    if (it == colors_.end())
        return std::nullopt;
    return it->second;
}

void SyntaxColorSettings::setColor(bool dark, const std::string& languageKey, model::Token token, std::optional<uint32_t> rgb)
{
    const std::string k = key(dark, languageKey, token);
    if (rgb)
        colors_[k] = *rgb & 0xFFFFFF;
    else
        colors_.erase(k);
    ++revision_;
    settings_.markDirty();
}

void SyntaxColorSettings::reset(bool dark, const std::string& languageKey)
{
    const std::string prefix = std::string(dark ? "dark" : "light") + "|" + languageKey + (languageKey.empty() ? "" : "|");
    for (auto it = colors_.begin(); it != colors_.end();)
        it = it->first.rfind(prefix, 0) == 0 ? colors_.erase(it) : std::next(it);
    ++revision_;
    settings_.markDirty();
}

bool SyntaxColorSettings::hasColors(bool dark, const std::string& languageKey) const
{
    const std::string prefix = std::string(dark ? "dark" : "light") + "|" + languageKey + "|";
    const auto it = colors_.lower_bound(prefix);
    return it != colors_.end() && it->first.rfind(prefix, 0) == 0;
}

} // namespace enp::viewmodel
