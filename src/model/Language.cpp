#include "enp/model/Language.h"

#include <algorithm>
#include <cctype>

namespace enp::model {

namespace {

std::vector<LanguageSpec> buildTable()
{
    std::vector<LanguageSpec> t;
    auto add = [&t](Language id, const char* key, const char* name, const char* description) -> LanguageSpec& {
        LanguageSpec& s = t.emplace_back();
        s.id = id;
        s.key = key;
        s.name = name;
        s.description = description;
        return s;
    };
    // C-family defaults: // and /* */ comments, braces.
    auto cFamily = [](LanguageSpec& s) {
        s.lineComment = "//";
        s.blockCommentStart = "/*";
        s.blockCommentEnd = "*/";
        s.fold = FoldStyle::Braces;
        s.program = true;
    };

    add(Language::PlainText, "text", "Plain Text", "Normal text file").extensions = {".txt", ".log"};

    {
        LanguageSpec& s = add(Language::Cpp, "cpp", "C / C++", "C++ source file");
        s.extensions = {".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh", ".hxx", ".inl", ".ino"};
        cFamily(s);
        s.preprocessor = true;
        s.capitalizedTypes = true;
        s.keywords = {"alignas", "alignof", "auto", "break", "case", "catch", "class", "co_await", "co_return",
                      "co_yield", "concept", "const", "consteval", "constexpr", "constinit", "const_cast", "continue",
                      "decltype", "default", "delete", "do", "dynamic_cast", "else", "enum", "explicit", "export",
                      "extern", "false", "final", "for", "friend", "goto", "if", "inline", "mutable", "namespace",
                      "new", "noexcept", "nullptr", "operator", "override", "private", "protected", "public",
                      "register", "reinterpret_cast", "requires", "return", "sizeof", "static", "static_assert",
                      "static_cast", "struct", "switch", "template", "this", "thread_local", "throw", "true", "try",
                      "typedef", "typeid", "typename", "union", "using", "virtual", "volatile", "while"};
        s.types = {"bool", "char", "char8_t", "char16_t", "char32_t", "double", "float", "int", "long", "short",
                   "signed", "unsigned", "void", "wchar_t", "size_t", "ptrdiff_t", "int8_t", "int16_t", "int32_t",
                   "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t"};
    }
    {
        LanguageSpec& s = add(Language::CSharp, "csharp", "C#", "C# source file");
        s.extensions = {".cs", ".csx"};
        cFamily(s);
        s.preprocessor = true;
        s.capitalizedTypes = true;
        s.keywords = {"abstract", "as", "async", "await", "base", "break", "case", "catch", "checked", "class",
                      "const", "continue", "default", "delegate", "do", "else", "enum", "event", "explicit", "extern",
                      "false", "finally", "fixed", "for", "foreach", "get", "goto", "if", "implicit", "in", "init",
                      "interface", "internal", "is", "lock", "namespace", "new", "null", "operator", "out",
                      "override", "params", "partial", "private", "protected", "public", "readonly", "record", "ref",
                      "return", "sealed", "set", "sizeof", "stackalloc", "static", "struct", "switch", "this",
                      "throw", "true", "try", "typeof", "unchecked", "unsafe", "using", "value", "var", "virtual",
                      "volatile", "when", "where", "while", "yield"};
        s.types = {"bool", "byte", "char", "decimal", "double", "dynamic", "float", "int", "long", "object", "sbyte",
                   "short", "string", "uint", "ulong", "ushort", "void", "nint", "nuint"};
    }
    {
        LanguageSpec& s = add(Language::Java, "java", "Java", "Java source file");
        s.extensions = {".java"};
        cFamily(s);
        s.capitalizedTypes = true;
        s.atKeywords = true; // annotations
        s.keywords = {"abstract", "assert", "break", "case", "catch", "class", "const", "continue", "default", "do",
                      "else", "enum", "extends", "false", "final", "finally", "for", "goto", "if", "implements",
                      "import", "instanceof", "interface", "native", "new", "null", "package", "permits", "private",
                      "protected", "public", "record", "return", "sealed", "static", "strictfp", "super", "switch",
                      "synchronized", "this", "throw", "throws", "transient", "true", "try", "var", "volatile",
                      "while", "yield"};
        s.types = {"boolean", "byte", "char", "double", "float", "int", "long", "short", "void", "String", "Object"};
    }
    {
        LanguageSpec& s = add(Language::JavaScript, "javascript", "JavaScript", "JavaScript file");
        s.extensions = {".js", ".mjs", ".cjs", ".jsx"};
        cFamily(s);
        s.quotes = "\"'`";
        s.multilineBacktick = true;
        s.capitalizedTypes = true;
        s.lfByDefault = true;
        s.keywords = {"async", "await", "break", "case", "catch", "class", "const", "continue", "debugger",
                      "default", "delete", "do", "else", "export", "extends", "false", "finally", "for", "from",
                      "function", "get", "if", "import", "in", "instanceof", "let", "new", "null", "of", "return",
                      "set", "static", "super", "switch", "this", "throw", "true", "try", "typeof", "undefined",
                      "var", "void", "while", "with", "yield"};
    }
    {
        LanguageSpec s = t.back(); // JavaScript, plus the differences
        s.id = Language::TypeScript;
        s.key = "typescript";
        s.name = "TypeScript";
        s.description = "TypeScript file";
        s.extensions = {".ts", ".tsx", ".mts", ".cts"};
        s.atKeywords = true; // decorators
        for (const char* k : {"abstract", "as", "declare", "enum", "implements", "interface", "keyof", "namespace",
                              "private", "protected", "public", "readonly", "satisfies", "type", "infer", "is"})
            s.keywords.insert(k);
        s.types = {"any", "boolean", "never", "number", "object", "string", "symbol", "unknown", "bigint", "void"};
        t.push_back(std::move(s));
    }
    {
        LanguageSpec& s = add(Language::Python, "python", "Python", "Python file");
        s.extensions = {".py", ".pyw", ".pyi"};
        s.lineComment = "#";
        s.tripleQuotes = true;
        s.atKeywords = true; // decorators
        s.fold = FoldStyle::Python;
        s.lfByDefault = true;
        s.program = true;
        s.keywords = {"and", "as", "assert", "async", "await", "break", "case", "class", "continue", "def", "del",
                      "elif", "else", "except", "False", "finally", "for", "from", "global", "if", "import", "in",
                      "is", "lambda", "match", "None", "nonlocal", "not", "or", "pass", "raise", "return", "True",
                      "try", "while", "with", "yield"};
        s.types = {"self", "cls", "int", "float", "str", "bool", "list", "dict", "set", "tuple", "bytes", "object"};
    }
    {
        LanguageSpec& s = add(Language::Rust, "rust", "Rust", "Rust source file");
        s.extensions = {".rs"};
        cFamily(s);
        s.quotes = "\""; // ' starts lifetimes as well as chars
        s.preprocessor = true; // #[attributes]
        s.capitalizedTypes = true;
        s.lfByDefault = true;
        s.keywords = {"as", "async", "await", "break", "const", "continue", "crate", "dyn", "else", "enum", "extern",
                      "false", "fn", "for", "if", "impl", "in", "let", "loop", "match", "mod", "move", "mut", "pub",
                      "ref", "return", "self", "Self", "static", "struct", "super", "trait", "true", "type", "unsafe",
                      "use", "where", "while"};
        s.types = {"bool", "char", "f32", "f64", "i8", "i16", "i32", "i64", "i128", "isize", "str", "u8", "u16",
                   "u32", "u64", "u128", "usize", "String", "Vec", "Option", "Result", "Box"};
    }
    {
        LanguageSpec& s = add(Language::Go, "go", "Go", "Go source file");
        s.extensions = {".go"};
        cFamily(s);
        s.quotes = "\"'`";
        s.multilineBacktick = true;
        s.lfByDefault = true;
        s.keywords = {"break", "case", "chan", "const", "continue", "default", "defer", "else", "fallthrough", "false",
                      "for", "func", "go", "goto", "if", "import", "interface", "iota", "map", "nil", "package",
                      "range", "return", "select", "struct", "switch", "true", "type", "var"};
        s.types = {"bool", "byte", "complex64", "complex128", "error", "float32", "float64", "int", "int8", "int16",
                   "int32", "int64", "rune", "string", "uint", "uint8", "uint16", "uint32", "uint64", "uintptr", "any"};
    }
    {
        LanguageSpec& s = add(Language::Lua, "lua", "Lua", "Lua script");
        s.extensions = {".lua"};
        s.lineComment = "--";
        s.blockCommentStart = "--[[";
        s.blockCommentEnd = "]]";
        s.fold = FoldStyle::Keywords;
        s.foldOpen = {"function", "if", "for", "while", "repeat"};
        s.foldClose = {"end", "until"};
        s.program = true;
        s.keywords = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in",
                      "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
        s.types = {"self", "_G", "_ENV"};
    }
    {
        LanguageSpec& s = add(Language::Shell, "shell", "Shell", "Shell script");
        s.extensions = {".sh", ".bash", ".zsh", ".ksh"};
        s.fileNames = {".bashrc", ".zshrc", ".profile", ".bash_profile"};
        s.lineComment = "#";
        s.dollarVariables = true;
        s.dashInIdentifiers = true;
        s.fold = FoldStyle::Keywords;
        s.foldOpen = {"if", "case", "for", "while", "until", "select"};
        s.foldClose = {"fi", "esac", "done"};
        s.foldBraces = true;
        s.lfByDefault = true;
        s.keywords = {"if", "then", "else", "elif", "fi", "case", "esac", "for", "while", "until", "do", "done", "in",
                      "function", "select", "return", "break", "continue", "local", "export", "readonly", "declare",
                      "set", "unset", "shift", "exit", "source", "trap", "eval", "exec", "true", "false"};
        s.types = {"echo", "printf", "read", "cd", "pwd", "test", "cat", "grep", "sed", "awk", "find", "mkdir", "rm",
                   "cp", "mv", "ls"};
    }
    {
        LanguageSpec& s = add(Language::PowerShell, "powershell", "PowerShell", "PowerShell script");
        s.extensions = {".ps1", ".psm1", ".psd1"};
        s.lineComment = "#";
        s.blockCommentStart = "<#";
        s.blockCommentEnd = "#>";
        s.dollarVariables = true;
        s.dashInIdentifiers = true; // Get-ChildItem, -Path
        s.caseInsensitive = true;
        s.fold = FoldStyle::Braces;
        s.program = true;
        s.keywords = {"begin", "break", "catch", "class", "continue", "data", "do", "dynamicparam", "else", "elseif",
                      "end", "enum", "exit", "filter", "finally", "for", "foreach", "function", "if", "in", "param",
                      "process", "return", "switch", "throw", "trap", "try", "until", "using", "while", "-eq", "-ne",
                      "-gt", "-lt", "-ge", "-le", "-like", "-match", "-and", "-or", "-not"};
        s.types = {"$true", "$false", "$null", "$_", "$psitem", "$this"};
    }
    {
        LanguageSpec& s = add(Language::Html, "html", "HTML", "HTML document");
        s.extensions = {".html", ".htm", ".xhtml", ".vue", ".svelte"};
        s.lexer = Lexer::Markup;
        s.blockCommentStart = "<!--";
        s.blockCommentEnd = "-->";
        s.fold = FoldStyle::Tags;
    }
    {
        LanguageSpec& s = add(Language::Xml, "xml", "XML", "XML document");
        s.extensions = {".xml", ".xsd", ".xsl", ".xslt", ".svg", ".plist", ".csproj", ".vcxproj", ".props",
                        ".targets", ".config", ".xaml", ".resx"};
        s.lexer = Lexer::Markup;
        s.blockCommentStart = "<!--";
        s.blockCommentEnd = "-->";
        s.fold = FoldStyle::Tags;
    }
    {
        LanguageSpec& s = add(Language::Css, "css", "CSS", "Style sheet");
        s.extensions = {".css", ".scss", ".less"};
        s.blockCommentStart = "/*";
        s.blockCommentEnd = "*/";
        s.dashInIdentifiers = true;
        s.atKeywords = true;
        s.keyValue = ':';
        s.fold = FoldStyle::Braces;
        s.keywords = {"important", "inherit", "initial", "unset", "none", "auto", "solid", "block", "inline", "flex",
                      "grid", "absolute", "relative", "fixed", "sticky", "hidden", "bold", "center"};
    }
    {
        LanguageSpec& s = add(Language::Json, "json", "JSON", "JSON file");
        s.extensions = {".json", ".jsonc", ".json5", ".geojson", ".ipynb"};
        s.fileNames = {".babelrc", ".eslintrc", ".prettierrc"};
        s.quotes = "\"";
        s.lineComment = "//"; // JSONC
        s.blockCommentStart = "/*";
        s.blockCommentEnd = "*/";
        s.keyValue = ':';
        s.fold = FoldStyle::Braces;
        s.lfByDefault = true;
        s.keywords = {"true", "false", "null"};
    }
    {
        LanguageSpec& s = add(Language::Yaml, "yaml", "YAML", "YAML file");
        s.extensions = {".yaml", ".yml"};
        s.fileNames = {".clang-format", ".clang-tidy"};
        s.lineComment = "#";
        s.keyValue = ':';
        s.fold = FoldStyle::Indent;
        s.lfByDefault = true;
        s.keywords = {"true", "false", "null", "yes", "no", "on", "off", "~"};
    }
    {
        LanguageSpec& s = add(Language::Toml, "toml", "TOML / INI", "TOML / INI file");
        s.extensions = {".toml", ".ini", ".cfg", ".conf", ".properties", ".editorconfig"};
        s.fileNames = {".gitconfig", ".editorconfig", ".gitmodules"};
        s.lineComment = "#";
        s.keyValue = '=';
        s.fold = FoldStyle::Sections;
        s.lfByDefault = true;
        s.keywords = {"true", "false"};
    }
    {
        LanguageSpec& s = add(Language::Csv, "csv", "CSV / TSV", "Comma-separated values");
        s.extensions = {".csv", ".tsv", ".tab"};
        s.lexer = Lexer::Delimited;
        s.quotes = "\"";
    }
    {
        LanguageSpec& s = add(Language::Sql, "sql", "SQL", "SQL script");
        s.extensions = {".sql", ".ddl", ".dml"};
        s.lineComment = "--";
        s.blockCommentStart = "/*";
        s.blockCommentEnd = "*/";
        s.quotes = "'\"`";
        s.caseInsensitive = true;
        s.fold = FoldStyle::Keywords;
        s.foldOpen = {"begin", "case"};
        s.foldClose = {"end"};
        s.foldParens = true;
        s.keywords = {"add", "all", "alter", "and", "as", "asc", "begin", "between", "by", "case", "check", "column",
                      "commit", "constraint", "create", "database", "default", "delete", "desc", "distinct", "drop",
                      "else", "end", "exists", "foreign", "from", "full", "function", "group", "having", "if", "in",
                      "index", "inner", "insert", "into", "is", "join", "key", "left", "like", "limit", "not", "null",
                      "offset", "on", "or", "order", "outer", "primary", "procedure", "references", "replace",
                      "return", "returns", "right", "rollback", "select", "set", "table", "then", "transaction",
                      "trigger", "truncate", "union", "unique", "update", "values", "view", "when", "where", "with"};
        s.types = {"int", "integer", "bigint", "smallint", "decimal", "numeric", "float", "real", "double", "char",
                   "varchar", "nvarchar", "text", "date", "time", "datetime", "timestamp", "boolean", "blob", "uuid",
                   "json", "serial", "true", "false"};
    }
    {
        LanguageSpec& s = add(Language::CMake, "cmake", "CMake", "CMake file");
        s.extensions = {".cmake"};
        s.fileNames = {"cmakelists.txt"};
        s.lineComment = "#";
        s.blockCommentStart = "#[[";
        s.blockCommentEnd = "]]";
        s.quotes = "\"";
        s.caseInsensitive = true;
        s.dollarVariables = true; // ${VAR}, $ENV{VAR}, $<genex>
        s.callsAreKeywords = true;
        s.upperCaseConstants = true;
        s.fold = FoldStyle::Keywords;
        s.foldOpen = {"if", "foreach", "while", "function", "macro", "block"};
        s.foldClose = {"endif", "endforeach", "endwhile", "endfunction", "endmacro", "endblock"};
        s.foldOpenNeedsParen = true;
        s.foldParens = true;
        s.lfByDefault = true;
        s.keywords = {"if", "elseif", "else", "endif", "foreach", "endforeach", "while", "endwhile", "function",
                      "endfunction", "macro", "endmacro", "block", "endblock", "return", "break", "continue"};
        s.types = {"on", "off", "true", "false", "yes", "no", "and", "or", "not", "public", "private", "interface",
                   "static", "shared", "module", "object", "defined", "strequal", "equal", "less", "greater",
                   "matches", "version_less", "version_greater", "version_equal", "exists", "target", "command",
                   "policy", "required", "components", "cache", "string", "bool", "path", "filepath", "internal",
                   "force", "parent_scope", "append", "prepend", "remove_item", "sources", "properties"};
    }
    {
        LanguageSpec& s = add(Language::Markdown, "markdown", "Markdown", "Markdown file");
        s.extensions = {".md", ".markdown", ".mdx", ".mkd"};
        s.fileNames = {"readme", "changelog"};
        s.lexer = Lexer::Markdown;
        s.fold = FoldStyle::Headings;
        s.lfByDefault = true;
    }

    // Plain text first, the rest by name (for menus).
    std::sort(t.begin() + 1, t.end(), [](const LanguageSpec& a, const LanguageSpec& b) {
        std::string x = a.name, y = b.name;
        for (char& c : x)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : y)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return x < y;
    });
    return t;
}

} // namespace

const std::vector<LanguageSpec>& languages()
{
    static const std::vector<LanguageSpec> table = buildTable();
    return table;
}

const LanguageSpec& languageSpec(Language language)
{
    static const std::vector<const LanguageSpec*> byId = [] {
        std::vector<const LanguageSpec*> index(size_t(Language::Count), nullptr);
        for (const LanguageSpec& s : languages())
            index[size_t(s.id)] = &s;
        for (const LanguageSpec*& p : index)
            if (!p)
                p = &languages().front(); // every language has an entry; plain text as a guard
        return index;
    }();
    const size_t i = size_t(language);
    return i < byId.size() ? *byId[i] : languages().front();
}

const char* languageName(Language language)
{
    return languageSpec(language).description;
}

Language languageFromPath(std::string_view path)
{
    std::string lower(path);
    for (char& c : lower)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    const size_t slash = lower.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? lower : lower.substr(slash + 1);
    // Whole file names first (CMakeLists.txt is not plain text), then extensions.
    for (const LanguageSpec& s : languages())
        for (const std::string& n : s.fileNames)
            if (name == n)
                return s.id;
    Language best = Language::PlainText;
    size_t bestLength = 0;
    for (const LanguageSpec& s : languages())
        for (const std::string& ext : s.extensions)
            if (ext.size() > bestLength && name.size() >= ext.size() &&
                name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
                best = s.id;
                bestLength = ext.size();
            }
    return best;
}

const char* tokenKey(Token token)
{
    switch (token) {
    case Token::Text: return "text";
    case Token::Keyword: return "keyword";
    case Token::Type: return "type";
    case Token::Function: return "function";
    case Token::Variable: return "variable";
    case Token::String: return "string";
    case Token::Number: return "number";
    case Token::Comment: return "comment";
    case Token::Preprocessor: return "preprocessor";
    case Token::Key: return "key";
    case Token::Heading: return "heading";
    case Token::Emphasis: return "emphasis";
    case Token::Code: return "code";
    case Token::Link: return "link";
    case Token::Tag: return "tag";
    case Token::Attribute: return "attribute";
    case Token::Column1: return "column1";
    case Token::Column2: return "column2";
    case Token::Column3: return "column3";
    case Token::Column4: return "column4";
    case Token::Column5: return "column5";
    case Token::Column6: return "column6";
    case Token::Count: break;
    }
    return "";
}

const char* tokenName(Token token)
{
    switch (token) {
    case Token::Text: return "Text";
    case Token::Keyword: return "Keyword";
    case Token::Type: return "Type";
    case Token::Function: return "Function";
    case Token::Variable: return "Variable";
    case Token::String: return "String";
    case Token::Number: return "Number";
    case Token::Comment: return "Comment";
    case Token::Preprocessor: return "Directive / decorator";
    case Token::Key: return "Key / property";
    case Token::Heading: return "Heading";
    case Token::Emphasis: return "Emphasis";
    case Token::Code: return "Code";
    case Token::Link: return "Link";
    case Token::Tag: return "Tag";
    case Token::Attribute: return "Attribute";
    case Token::Column1: return "Column 1";
    case Token::Column2: return "Column 2";
    case Token::Column3: return "Column 3";
    case Token::Column4: return "Column 4";
    case Token::Column5: return "Column 5";
    case Token::Column6: return "Column 6";
    case Token::Count: break;
    }
    return "";
}

Token tokenFromKey(std::string_view key)
{
    for (int i = 0; i < int(Token::Count); ++i)
        if (key == tokenKey(Token(i)))
            return Token(i);
    return Token::Count;
}

std::vector<Token> languageTokens(Language language)
{
    using T = Token;
    const LanguageSpec& s = languageSpec(language);
    switch (s.lexer) {
    case Lexer::Markdown: return {T::Text, T::Heading, T::Emphasis, T::Code, T::Link, T::Keyword, T::Comment};
    case Lexer::Markup: return {T::Text, T::Tag, T::Attribute, T::String, T::Comment, T::Preprocessor, T::Variable};
    case Lexer::Delimited: return {T::Column1, T::Column2, T::Column3, T::Column4, T::Column5, T::Column6, T::String};
    case Lexer::Code: break;
    }
    if (s.id == Language::PlainText)
        return {T::Text};
    std::vector<Token> tokens = {T::Text, T::Keyword};
    if (!s.types.empty() || s.capitalizedTypes || s.upperCaseConstants)
        tokens.push_back(T::Type);
    const bool data = s.id == Language::Json || s.id == Language::Yaml || s.id == Language::Toml;
    if (!data)
        tokens.push_back(T::Function);
    if (s.dollarVariables)
        tokens.push_back(T::Variable);
    tokens.push_back(T::String);
    tokens.push_back(T::Number);
    if (!s.lineComment.empty() || !s.blockCommentStart.empty())
        tokens.push_back(T::Comment);
    if (s.preprocessor || s.atKeywords || s.id == Language::Yaml || s.id == Language::CMake) // #include, @decorator, ---, $<genex>
        tokens.push_back(T::Preprocessor);
    if (s.keyValue != 0)
        tokens.push_back(T::Key);
    if (s.fold == FoldStyle::Sections)
        tokens.push_back(T::Heading);
    return tokens;
}

Language languageFromKey(std::string_view key)
{
    for (const LanguageSpec& s : languages())
        if (key == s.key)
            return s.id;
    return Language::PlainText;
}

} // namespace enp::model
