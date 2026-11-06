#include "enp/model/Math.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace enp::model::math {

namespace {

struct Symbol {
    const char* command;
    const char* glyph;
};

// Commands that stand for one glyph. Binary operators and relations are listed separately below.
constexpr Symbol kSymbols[] = {
    {"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"}, {"delta", "δ"}, {"epsilon", "ε"}, {"varepsilon", "ε"}, {"zeta", "ζ"},
    {"eta", "η"}, {"theta", "θ"}, {"vartheta", "ϑ"}, {"iota", "ι"}, {"kappa", "κ"}, {"lambda", "λ"}, {"mu", "μ"}, {"nu", "ν"},
    {"xi", "ξ"}, {"pi", "π"}, {"varpi", "ϖ"}, {"rho", "ρ"}, {"varrho", "ϱ"}, {"sigma", "σ"}, {"varsigma", "ς"}, {"tau", "τ"},
    {"upsilon", "υ"}, {"phi", "φ"}, {"varphi", "ϕ"}, {"chi", "χ"}, {"psi", "ψ"}, {"omega", "ω"},
    {"Gamma", "Γ"}, {"Delta", "Δ"}, {"Theta", "Θ"}, {"Lambda", "Λ"}, {"Xi", "Ξ"}, {"Pi", "Π"}, {"Sigma", "Σ"}, {"Upsilon", "Υ"},
    {"Phi", "Φ"}, {"Psi", "Ψ"}, {"Omega", "Ω"},
    {"infty", "∞"}, {"partial", "∂"}, {"nabla", "∇"}, {"forall", "∀"}, {"exists", "∃"}, {"nexists", "∄"}, {"emptyset", "∅"},
    {"varnothing", "∅"}, {"neg", "¬"}, {"lnot", "¬"}, {"ldots", "…"}, {"dots", "…"}, {"cdots", "⋯"}, {"vdots", "⋮"}, {"ddots", "⋱"},
    {"angle", "∠"}, {"triangle", "△"}, {"square", "□"}, {"degree", "°"}, {"circ", "∘"}, {"bullet", "•"}, {"prime", "′"},
    {"hbar", "ℏ"}, {"ell", "ℓ"}, {"Re", "ℜ"}, {"Im", "ℑ"}, {"aleph", "ℵ"}, {"wp", "℘"}, {"top", "⊤"}, {"bot", "⊥"},
    {"langle", "⟨"}, {"rangle", "⟩"}, {"lceil", "⌈"}, {"rceil", "⌉"}, {"lfloor", "⌊"}, {"rfloor", "⌋"}, {"|", "‖"}, {"vert", "|"},
    {"Vert", "‖"}, {"backslash", "\\"}, {"{", "{"}, {"}", "}"}, {"$", "$"}, {"%", "%"}, {"&", "&"}, {"#", "#"}, {"_", "_"},
    {"checkmark", "✓"}, {"star", "⋆"}, {"ast", "∗"}, {"dagger", "†"}, {"ddagger", "‡"}, {"S", "§"}, {"P", "¶"}, {"copyright", "©"},
    {"pounds", "£"}, {"euro", "€"}, {"yen", "¥"},
};

constexpr Symbol kBinary[] = {
    {"times", "×"}, {"cdot", "·"}, {"div", "÷"}, {"pm", "±"}, {"mp", "∓"}, {"leq", "≤"}, {"le", "≤"}, {"geq", "≥"}, {"ge", "≥"},
    {"neq", "≠"}, {"ne", "≠"}, {"approx", "≈"}, {"equiv", "≡"}, {"sim", "∼"}, {"simeq", "≃"}, {"cong", "≅"}, {"propto", "∝"},
    {"ll", "≪"}, {"gg", "≫"}, {"to", "→"}, {"rightarrow", "→"}, {"leftarrow", "←"}, {"leftrightarrow", "↔"}, {"Rightarrow", "⇒"},
    {"Leftarrow", "⇐"}, {"Leftrightarrow", "⇔"}, {"implies", "⇒"}, {"iff", "⇔"}, {"mapsto", "↦"}, {"uparrow", "↑"},
    {"downarrow", "↓"}, {"in", "∈"}, {"notin", "∉"}, {"ni", "∋"}, {"subset", "⊂"}, {"subseteq", "⊆"}, {"supset", "⊃"},
    {"supseteq", "⊇"}, {"cup", "∪"}, {"cap", "∩"}, {"setminus", "∖"}, {"land", "∧"}, {"wedge", "∧"}, {"lor", "∨"}, {"vee", "∨"},
    {"oplus", "⊕"}, {"otimes", "⊗"}, {"ominus", "⊖"}, {"perp", "⊥"}, {"parallel", "∥"}, {"mid", "|"}, {"vdash", "⊢"},
    {"models", "⊨"}, {"prec", "≺"}, {"succ", "≻"}, {"ast", "∗"}, {"star", "⋆"}, {"circ", "∘"},
};

// The binary symbols above that are operators; the rest are relations.
constexpr const char* kOperatorGlyphs[] = {"×", "·", "÷", "±", "∓", "∪", "∩", "∖", "∧", "∨", "⊕", "⊗", "⊖", "∗", "⋆", "∘"};

bool isRelation(std::string_view glyph)
{
    for (const char* op : kOperatorGlyphs)
        if (glyph == op)
            return false;
    return true;
}

struct Operator {
    const char* command;
    const char* glyph;
    bool limitsAbove;
};
constexpr Operator kOperators[] = {
    {"sum", "∑", true}, {"prod", "∏", true}, {"coprod", "∐", true}, {"bigcup", "⋃", true}, {"bigcap", "⋂", true},
    {"bigoplus", "⨁", true}, {"bigotimes", "⨂", true}, {"int", "∫", false}, {"iint", "∬", false}, {"iiint", "∭", false},
    {"oint", "∮", false}, {"lim", "lim", true}, {"limsup", "lim sup", true}, {"liminf", "lim inf", true}, {"max", "max", true},
    {"min", "min", true}, {"sup", "sup", true}, {"inf", "inf", true}, {"argmax", "arg max", true}, {"argmin", "arg min", true},
    {"det", "det", true}, {"gcd", "gcd", true},
};

constexpr const char* kFunctions[] = {"sin", "cos", "tan", "cot", "sec", "csc", "arcsin", "arccos", "arctan", "sinh", "cosh",
                                      "tanh", "coth", "log", "ln", "lg", "exp", "deg", "dim", "ker", "hom", "arg", "Pr", "sgn"};

struct Accent {
    const char* command;
    const char* glyph;
};
constexpr Accent kAccents[] = {{"hat", "^"},    {"widehat", "^"},  {"bar", "¯"},      {"overline", "‾"}, {"vec", "→"},
                               {"tilde", "~"},  {"widetilde", "~"}, {"dot", "˙"},      {"ddot", "¨"},     {"check", "ˇ"},
                               {"breve", "˘"},  {"acute", "´"},    {"grave", "`"},    {"mathring", "˚"}};

struct Spacing {
    const char* command;
    float em;
};
constexpr Spacing kSpacing[] = {{",", 0.17f}, {":", 0.22f}, {";", 0.28f}, {"!", -0.17f}, {" ", 0.33f}, {"quad", 1.0f},
                                {"qquad", 2.0f}, {"enspace", 0.5f}, {"thinspace", 0.17f}};

bool isLetter(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

Node text(std::string t, bool upright = false)
{
    Node n;
    n.kind = Node::Kind::Text;
    n.text = std::move(t);
    n.upright = upright;
    return n;
}

Node row(std::vector<Node> items = {})
{
    Node n;
    n.children = std::move(items);
    return n;
}

// Rows of one item collapse to the item.
Node simplify(Node n)
{
    if (n.kind == Node::Kind::Row && n.children.size() == 1)
        return std::move(n.children[0]);
    return n;
}

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    // A formula with \\ or & at the top level is a table (an align without the environment).
    Node run() { return parseTable(true); }

private:
    std::string_view s_;
    size_t i_ = 0;

    bool at(char c) const { return i_ < s_.size() && s_[i_] == c; }
    void skipSpaces()
    {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
            ++i_;
    }

    bool atRowBreak() const { return s_.substr(i_, 2) == "\\\\"; }

    // Rows split by \\ and cells by &, until \end{...} (consumed) or, at the top level, the end
    // (a stray \end there is dropped). One cell alone is that cell.
    Node parseTable(bool top)
    {
        Node table;
        table.kind = Node::Kind::Table;
        Node cells = row(), cell = row();
        auto endRow = [&] {
            cells.children.push_back(simplify(std::move(cell)));
            cell = row();
            table.children.push_back(std::move(cells));
            cells = row();
        };
        for (;;) {
            Node part = parseRow(nullptr, true);
            for (Node& n : part.children)
                cell.children.push_back(std::move(n));
            if (i_ >= s_.size()) {
                endRow();
                break;
            }
            if (at('&')) {
                ++i_;
                cells.children.push_back(simplify(std::move(cell)));
                cell = row();
            } else if (atRowBreak()) {
                i_ += 2;
                skipSpaces();
                if (at('[')) { // \\[2pt]: extra space, not drawn
                    const size_t end = s_.find(']', i_);
                    i_ = end == std::string_view::npos ? s_.size() : end + 1;
                }
                endRow();
            } else { // \end
                i_ += 4;
                argumentText();
                if (!top) {
                    endRow();
                    break;
                }
            }
        }
        // A last \\ makes no row of its own.
        if (table.children.size() > 1) {
            const Node& last = table.children.back();
            if (last.children.size() == 1 && last.children[0].kind == Node::Kind::Row && last.children[0].children.empty())
                table.children.pop_back();
        }
        if (table.children.size() == 1 && table.children[0].children.size() == 1)
            return std::move(table.children[0].children[0]);
        return table;
    }

    // \begin{env}: its rows and columns, fenced as the environment is (pmatrix in parentheses,
    // cases with a brace on the left).
    Node parseEnvironment(const std::string& env)
    {
        std::string align = "c", left, right;
        bool pairs = false, display = false;
        if (env == "array" || env == "subarray") {
            align.clear();
            for (char ch : argumentText())
                if (ch == 'l' || ch == 'c' || ch == 'r')
                    align += ch;
            if (align.empty())
                align = "c";
        } else if (env == "aligned" || env == "align" || env == "align*" || env == "split" || env == "flalign" ||
                   env == "flalign*" || env == "alignat" || env == "alignat*" || env == "alignedat") {
            if (env.compare(0, 7, "alignat") == 0 || env == "alignedat")
                argumentText(); // the number of column pairs
            align = "rl";
            pairs = display = true;
        } else if (env == "eqnarray" || env == "eqnarray*") {
            align = "rcl";
            display = true;
        } else if (env == "cases" || env == "dcases") {
            align = "l";
            left = "{";
            display = env == "dcases";
        } else if (env == "rcases") {
            align = "l";
            right = "}";
        } else if (env == "pmatrix") {
            left = "(", right = ")";
        } else if (env == "bmatrix") {
            left = "[", right = "]";
        } else if (env == "Bmatrix") {
            left = "{", right = "}";
        } else if (env == "vmatrix") {
            left = "|", right = "|";
        } else if (env == "Vmatrix") {
            left = "‖", right = "‖";
        } else if (env != "matrix" && env != "smallmatrix") { // gather, equation, multline and the unknown
            display = true;
        }
        Node body = parseTable(false);
        if (body.kind != Node::Kind::Table) { // one cell: a table of it, so a matrix stays a matrix
            Node t;
            t.kind = Node::Kind::Table;
            t.children.push_back(row({std::move(body)}));
            body = std::move(t);
        }
        body.text = align;
        body.pairs = pairs;
        body.display = display;
        if (left.empty() && right.empty())
            return body;
        Node f;
        f.kind = Node::Kind::Fenced;
        f.text = left;
        f.text2 = right;
        f.children.push_back(std::move(body));
        return f;
    }

    // Items until `}` (consumed when `closer` is "}"), a \right, \end, or the end; in a `cell` of a
    // table also until & or \\.
    Node parseRow(const char* closer, bool cell = false)
    {
        Node r = row();
        while (i_ < s_.size()) {
            skipSpaces();
            if (i_ >= s_.size())
                break;
            if (closer && std::strcmp(closer, "}") == 0 && at('}')) {
                ++i_;
                return r;
            }
            if (at('}')) { // stray: skip it
                ++i_;
                continue;
            }
            if (closer && std::strcmp(closer, "right") == 0 && s_.substr(i_, 6) == "\\right")
                return r;
            if (s_.substr(i_, 4) == "\\end")
                return r;
            if (cell && (at('&') || atRowBreak()))
                return r;
            if (at('^') || at('_')) {
                // Scripts attach to the item before (an empty base when there is none).
                Node base = r.children.empty() ? row() : std::move(r.children.back());
                if (!r.children.empty())
                    r.children.pop_back();
                r.children.push_back(parseScripts(std::move(base)));
                continue;
            }
            if (at('\'')) {
                Node base = r.children.empty() ? row() : std::move(r.children.back());
                if (!r.children.empty())
                    r.children.pop_back();
                std::string primes;
                while (at('\'')) {
                    primes += "′";
                    ++i_;
                }
                Node sc;
                sc.kind = Node::Kind::Scripts;
                sc.children = {std::move(base), row({text(primes)}), row()};
                r.children.push_back(std::move(sc));
                continue;
            }
            r.children.push_back(parseItem());
        }
        return r;
    }

    // One item: a group, a command, or a run of characters.
    Node parseItem()
    {
        const char c = s_[i_];
        if (c == '{') {
            ++i_;
            return simplify(parseRow("}"));
        }
        if (c == '\\')
            return parseCommand();
        if (isLetter(c)) { // one variable per letter, as LaTeX sets them
            ++i_;
            return text(std::string(1, c));
        }
        if (isDigit(c) || c == '.') {
            size_t j = i_;
            while (j < s_.size() && (isDigit(s_[j]) || s_[j] == '.'))
                ++j;
            Node n = text(std::string(s_.substr(i_, j - i_)), true);
            i_ = j;
            return n;
        }
        if (c == '~') {
            ++i_;
            return space(0.33f);
        }
        // Punctuation and operators: one glyph (UTF-8 sequences kept whole).
        size_t n = 1;
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0xF0)
            n = 4;
        else if (u >= 0xE0)
            n = 3;
        else if (u >= 0xC0)
            n = 2;
        n = std::min(n, s_.size() - i_);
        Node t = text(std::string(s_.substr(i_, n)), true);
        t.binary = std::string_view("+-=<>*/").find(c) != std::string_view::npos && n == 1;
        t.relation = std::string_view("=<>").find(c) != std::string_view::npos && n == 1;
        i_ += n;
        return t;
    }

    static Node space(float em)
    {
        Node n;
        n.kind = Node::Kind::Space;
        n.space = em;
        return n;
    }

    Node parseScripts(Node base)
    {
        Node sc;
        sc.kind = Node::Kind::Scripts;
        sc.children = {std::move(base), row(), row()};
        while (at('^') || at('_')) {
            const bool sup = s_[i_] == '^';
            ++i_;
            skipSpaces();
            Node arg = i_ < s_.size() ? parseItem() : row();
            sc.children[sup ? 1 : 2] = std::move(arg);
        }
        return sc;
    }

    std::string argumentText()
    {
        // The raw text of a {...} group (for \text, \mathrm).
        skipSpaces();
        if (!at('{'))
            return i_ < s_.size() ? std::string(1, s_[i_++]) : std::string();
        int depth = 0;
        const size_t start = i_ + 1;
        for (; i_ < s_.size(); ++i_) {
            if (s_[i_] == '{')
                ++depth;
            else if (s_[i_] == '}' && --depth == 0) {
                std::string out(s_.substr(start, i_ - start));
                ++i_;
                return out;
            }
        }
        return std::string(s_.substr(start));
    }

    Node argument()
    {
        skipSpaces();
        if (i_ >= s_.size())
            return row();
        return parseItem();
    }

    Node parseCommand()
    {
        ++i_; // the backslash
        if (i_ >= s_.size())
            return text("\\", true);
        std::string name;
        if (isLetter(s_[i_])) {
            while (i_ < s_.size() && isLetter(s_[i_]))
                name += s_[i_++];
        } else {
            name = std::string(1, s_[i_++]);
        }
        for (const Spacing& sp : kSpacing)
            if (name == sp.command)
                return space(sp.em);
        if (name == "\\")
            return space(0.5f); // a line break inside a group (\substack{a \\ b}): shown as a gap
        if (name == "frac" || name == "dfrac" || name == "tfrac") {
            Node f;
            f.kind = Node::Kind::Frac;
            f.children.push_back(argument());
            f.children.push_back(argument());
            return f;
        }
        if (name == "binom" || name == "dbinom" || name == "tbinom") { // n over k in parentheses
            Node t;
            t.kind = Node::Kind::Table;
            t.text = "c";
            t.display = name == "dbinom";
            t.children.push_back(row({argument()}));
            t.children.push_back(row({argument()}));
            Node f;
            f.kind = Node::Kind::Fenced;
            f.text = "(";
            f.text2 = ")";
            f.children.push_back(std::move(t));
            return f;
        }
        if (name == "sqrt") {
            Node q;
            q.kind = Node::Kind::Sqrt;
            Node index = row();
            skipSpaces();
            if (at('[')) {
                ++i_;
                const size_t end = s_.find(']', i_);
                index = Parser(s_.substr(i_, end == std::string_view::npos ? std::string_view::npos : end - i_)).run();
                i_ = end == std::string_view::npos ? s_.size() : end + 1;
            }
            q.children.push_back(argument());
            q.children.push_back(std::move(index));
            return q;
        }
        if (name == "text" || name == "mathrm" || name == "textrm" || name == "operatorname" || name == "mathit" || name == "mathsf" ||
            name == "mathtt" || name == "textit" || name == "textbf" || name == "mathbf" || name == "boldsymbol" || name == "mathcal" ||
            name == "mathbb") {
            const bool bold = name == "mathbf" || name == "boldsymbol" || name == "textbf";
            if (name == "text" || name == "textrm" || name == "textit" || name == "textbf" || name == "operatorname" || name == "mathrm") {
                Node t = text(argumentText(), true);
                t.bold = bold;
                return t;
            }
            Node inner = argument();
            auto mark = [&](auto&& self, Node& n) -> void {
                if (n.kind == Node::Kind::Text) {
                    n.bold = n.bold || bold;
                    n.upright = n.upright || name == "mathsf" || name == "mathtt";
                }
                for (Node& c : n.children)
                    self(self, c);
            };
            mark(mark, inner);
            return inner;
        }
        if (name == "left") {
            Node f;
            f.kind = Node::Kind::Fenced;
            f.text = delimiter();
            f.children.push_back(simplify(parseRow("right")));
            if (s_.substr(i_, 6) == "\\right") {
                i_ += 6;
                f.text2 = delimiter();
            }
            if (f.text == ".")
                f.text.clear();
            if (f.text2 == ".")
                f.text2.clear();
            return f;
        }
        if (name == "right") // without a \left: the delimiter itself
            return text(delimiter(), true);
        if (name == "begin")
            return parseEnvironment(argumentText());
        if (name == "end") {
            argumentText();
            return row();
        }
        for (const Accent& a : kAccents)
            if (name == a.command) {
                Node n;
                n.kind = Node::Kind::Accent;
                n.text = a.glyph;
                n.children.push_back(argument());
                return n;
            }
        for (const Operator& op : kOperators)
            if (name == op.command) {
                Node n;
                n.kind = Node::Kind::Op;
                n.text = op.glyph;
                n.limitsAbove = op.limitsAbove;
                n.upright = true;
                return n;
            }
        for (const char* fn : kFunctions)
            if (name == fn) {
                Node n = text(name, true);
                return row({std::move(n), space(0.17f)});
            }
        for (const Symbol& sym : kBinary)
            if (name == sym.command) {
                Node n = text(sym.glyph, true);
                n.binary = true;
                n.relation = isRelation(sym.glyph);
                return n;
            }
        for (const Symbol& sym : kSymbols)
            if (name == sym.command)
                return text(sym.glyph, true);
        if (name == "displaystyle" || name == "textstyle" || name == "scriptstyle" || name == "nonumber" || name == "limits" ||
            name == "nolimits")
            return row();
        return text(name, true); // unknown: its name
    }

    std::string delimiter()
    {
        skipSpaces();
        if (i_ >= s_.size())
            return {};
        if (s_[i_] == '\\') {
            Node n = parseCommand();
            return n.kind == Node::Kind::Text ? n.text : std::string();
        }
        return std::string(1, s_[i_++]);
    }
};

void plain(const Node& n, std::string& out)
{
    switch (n.kind) {
    case Node::Kind::Row:
        for (const Node& c : n.children)
            plain(c, out);
        break;
    case Node::Kind::Text:
        out += n.text;
        break;
    case Node::Kind::Frac:
        out += '(';
        plain(n.children[0], out);
        out += ")/(";
        plain(n.children[1], out);
        out += ')';
        break;
    case Node::Kind::Scripts:
        plain(n.children[0], out);
        if (!n.children[2].children.empty() || n.children[2].kind != Node::Kind::Row) {
            out += "_{";
            plain(n.children[2], out);
            out += '}';
        }
        if (!n.children[1].children.empty() || n.children[1].kind != Node::Kind::Row) {
            out += "^{";
            plain(n.children[1], out);
            out += '}';
        }
        break;
    case Node::Kind::Sqrt:
        out += "√(";
        plain(n.children[0], out);
        out += ')';
        break;
    case Node::Kind::Op:
        out += n.text;
        break;
    case Node::Kind::Accent:
        plain(n.children[0], out);
        out += n.text;
        break;
    case Node::Kind::Space:
        out += ' ';
        break;
    case Node::Kind::Fenced:
        out += n.text;
        plain(n.children[0], out);
        out += n.text2;
        break;
    case Node::Kind::Table:
        for (size_t r = 0; r < n.children.size(); ++r) {
            if (r > 0)
                out += " \\\\ ";
            const Node& cells = n.children[r];
            for (size_t j = 0; j < cells.children.size(); ++j) {
                if (j > 0)
                    out += " & ";
                plain(cells.children[j], out);
            }
        }
        break;
    }
}

} // namespace

Node parse(std::string_view latex)
{
    return Parser(latex).run();
}

std::string plainText(const Node& node)
{
    std::string out;
    plain(node, out);
    return out;
}

} // namespace enp::model::math
