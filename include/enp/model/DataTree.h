// JSON and YAML read into one tree for the structured views (tree with values, accordion). Nodes
// sit in one array and every key and value in one string, so a large file is a few allocations.
// Each node keeps its source line, so a view can go back to the text there.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace enp::model::data {

enum class Kind : std::uint8_t { Object, Array, String, Number, Bool, Null };
constexpr std::uint32_t kNone = 0xFFFFFFFFu;

struct Node {
    Kind kind = Kind::Null;
    std::uint32_t parent = kNone;
    std::uint32_t firstChild = kNone, lastChild = kNone, nextSibling = kNone;
    std::uint32_t childCount = 0;
    std::uint32_t key = 0, keyLength = 0;     // in Tree::strings (empty for array items and the root)
    std::uint32_t value = 0, valueLength = 0; // scalars: the value as text (strings unescaped)
    int line = 0;                             // 0-based source line
    bool container() const { return kind == Kind::Object || kind == Kind::Array; }
};

struct Tree {
    std::vector<Node> nodes; // nodes[0] is the root
    std::string strings;
    std::string error; // why the text could not be read (the tree is empty then)
    int errorLine = -1;

    bool ok() const { return error.empty() && !nodes.empty(); }
    std::string_view key(const Node& n) const { return std::string_view(strings).substr(n.key, n.keyLength); }
    std::string_view value(const Node& n) const { return std::string_view(strings).substr(n.value, n.valueLength); }
    // "$.servers[2].name"
    std::string path(std::uint32_t index) const;
    // The position of `index` among its parent's children.
    std::uint32_t indexInParent(std::uint32_t index) const;
};

const char* kindName(Kind kind);
Tree parseJson(std::string_view text);
// The YAML that configuration files use: block mappings and sequences, flow [..] {..}, plain,
// quoted and block (| >) scalars, comments, several documents (the root is then their list).
// Anchors and aliases are shown as written; tags are dropped.
Tree parseYaml(std::string_view text);

} // namespace enp::model::data
