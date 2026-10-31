// The user's syntax colours: overrides of the theme's colours as 0xRRGGBB, per theme (dark / light),
// for one language (its table key) or for all of them, kept in the settings store.
#pragma once

#include "enp/model/Language.h"
#include "enp/viewmodel/Settings.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace enp::viewmodel {

class SyntaxColorSettings {
public:
    static constexpr const char* kAllLanguages = "*";

    explicit SyntaxColorSettings(Settings& settings); // registers the "syntaxColors" section
    ~SyntaxColorSettings();                           // hands its data to the store, unregisters

    std::optional<uint32_t> color(bool dark, const std::string& languageKey, model::Token token) const;
    void setColor(bool dark, const std::string& languageKey, model::Token token, std::optional<uint32_t> rgb);
    void reset(bool dark, const std::string& languageKey); // empty key: every language of that theme
    bool hasColors(bool dark, const std::string& languageKey) const;
    int revision() const { return revision_; }

private:
    static std::string key(bool dark, const std::string& languageKey, model::Token token);

    Settings& settings_;
    std::map<std::string, uint32_t> colors_; // "dark|cpp|keyword" -> 0xRRGGBB
    int revision_ = 0;
};

} // namespace enp::viewmodel
