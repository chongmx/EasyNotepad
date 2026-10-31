// The languages the editor understands, as one data table compiled into the binary: how to
// recognise a file, its comment and string syntax, its keywords and how it folds. Highlighting,
// folding, comment toggling and the language menus are all driven by this table; adding a
// language is adding an entry.
#pragma once

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace enp::model {

enum class Language {
    PlainText, Cpp, Python, JavaScript, Markdown, Json, CMake,
    TypeScript, CSharp, Java, Rust, Go, Shell, PowerShell, Html, Xml, Css, Yaml, Toml, Sql, Lua, Csv,
    Count
};

// Colour classes the highlighter assigns; each can be recoloured globally or per language.
enum class Token : unsigned char {
    Text, Keyword, Type, Function, Variable, String, Number, Comment, Preprocessor,
    Key, Heading, Emphasis, Code, Link, Tag, Attribute,
    Column1, Column2, Column3, Column4, Column5, Column6, // CSV columns, cycling
    Count
};
const char* tokenKey(Token token);  // "keyword"
const char* tokenName(Token token); // "Keyword"
Token tokenFromKey(std::string_view key); // Token::Count when unknown

// How a language's regions are found for folding.
enum class FoldStyle {
    None,
    Braces,   // { } blocks (plus [ ] in JSON)
    Python,   // def / class by indentation
    Indent,   // any line whose following lines are indented deeper (YAML)
    Keywords, // opener / closer words: if ... endif, function ... end, if ... fi
    Headings, // Markdown sections and fenced code blocks
    Tags,     // <element> ... </element>
    Sections, // [section] headers (TOML, INI)
};

// Lexical shape of a language beyond keywords.
enum class Lexer {
    Code,     // identifiers, strings, numbers, comments
    Markdown,
    Markup,   // HTML / XML tags
    Delimited, // CSV / TSV: every column its own colour
};

struct LanguageSpec {
    Language id = Language::PlainText;
    const char* key = "";          // stable id: "cpp", "cmake"
    const char* name = "";         // menu name: "C++"
    const char* description = "";  // status bar: "C++ source file"
    std::vector<std::string> extensions; // lower case, with the dot
    std::vector<std::string> fileNames;  // lower case, whole names: "cmakelists.txt", "makefile"

    Lexer lexer = Lexer::Code;
    std::string lineComment;              // "//", "#", "--"
    std::string blockCommentStart;        // "/*", "<!--", "<#", "--[["
    std::string blockCommentEnd;
    std::string quotes = "\"'";           // string delimiters
    bool tripleQuotes = false;            // Python """ / '''
    bool multilineBacktick = false;       // JS template literals, Go raw strings
    bool preprocessor = false;            // # directives / #[attributes]
    bool caseInsensitive = false;         // keywords (CMake commands, SQL)
    bool dollarVariables = false;         // $name, ${name}
    bool callsAreKeywords = false;        // CMake: every "command(" is a command
    bool capitalizedTypes = false;        // Foo, std::string -> type colour
    bool upperCaseConstants = false;      // CMAKE_CXX_STANDARD -> type colour
    bool dashInIdentifiers = false;       // CSS properties, shell options
    char keyValue = 0;                    // YAML ':', TOML '=': the key before it is coloured
    bool atKeywords = false;              // CSS @media, decorators, annotations
    char delimiter = 0;                   // Delimited: comma or tab (0: detect from the line)

    std::unordered_set<std::string> keywords; // lower case when caseInsensitive
    std::unordered_set<std::string> types;    // built-in types and literals

    FoldStyle fold = FoldStyle::None;
    std::vector<std::string> foldOpen;   // Keywords style: words that open a region
    std::vector<std::string> foldClose;  // ... and close the innermost one
    bool foldOpenNeedsParen = false;     // CMake: if( / endif(
    bool foldBraces = false;             // Keywords style: { } too (shell functions)
    bool foldParens = false;             // multi-line ( ) calls (CMake lists, SQL)

    bool lfByDefault = false;   // new documents use LF line endings
    bool program = false;       // has functions: logical folding and the logical map
};

const std::vector<LanguageSpec>& languages(); // table order: Plain text first, then by name
const LanguageSpec& languageSpec(Language language);
const char* languageName(Language language);  // the description, for the status bar
Language languageFromPath(std::string_view path);
Language languageFromKey(std::string_view key); // PlainText when unknown
// The colour classes a language's highlighter produces (for the colour settings).
std::vector<Token> languageTokens(Language language);

} // namespace enp::model
