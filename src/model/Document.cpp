#include "enp/model/Document.h"

#include <cctype>

namespace enp::model {

const char* languageName(Language language)
{
    switch (language) {
    case Language::PlainText: return "Normal text file";
    case Language::Cpp: return "C++ source file";
    case Language::Python: return "Python file";
    case Language::JavaScript: return "JavaScript file";
    case Language::Markdown: return "Markdown file";
    case Language::Json: return "JSON file";
    case Language::CMake: return "CMake file";
    }
    return "";
}

const char* eolName(EolMode eol)
{
    switch (eol) {
    case EolMode::CrLf: return "Windows (CR LF)";
    case EolMode::Lf: return "Unix (LF)";
    case EolMode::Cr: return "Macintosh (CR)";
    }
    return "";
}

const char* eolShortName(EolMode eol)
{
    switch (eol) {
    case EolMode::CrLf: return "CRLF";
    case EolMode::Lf: return "LF";
    case EolMode::Cr: return "CR";
    }
    return "";
}

const char* encodingName(Encoding encoding)
{
    switch (encoding) {
    case Encoding::Utf8: return "UTF-8";
    case Encoding::Utf8Bom: return "UTF-8-BOM";
    case Encoding::Utf16Le: return "UTF-16 LE BOM";
    case Encoding::Utf16Be: return "UTF-16 BE BOM";
    case Encoding::Ansi: return "ANSI";
    }
    return "";
}

Language languageFromPath(std::string_view path)
{
    std::string lower(path);
    for (char& c : lower)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    auto endsWith = [&lower](std::string_view suffix) {
        return lower.size() >= suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (endsWith("cmakelists.txt") || endsWith(".cmake"))
        return Language::CMake;
    for (const char* ext : {".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh"})
        if (endsWith(ext))
            return Language::Cpp;
    if (endsWith(".py"))
        return Language::Python;
    if (endsWith(".js") || endsWith(".ts") || endsWith(".mjs"))
        return Language::JavaScript;
    if (endsWith(".md") || endsWith(".markdown"))
        return Language::Markdown;
    if (endsWith(".json"))
        return Language::Json;
    return Language::PlainText;
}

} // namespace enp::model
