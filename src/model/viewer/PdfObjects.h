// PDF's object layer (internal to the PDF reader): the lexer, objects, the cross-reference table
// (tables and streams, incremental updates, object streams), stream filters, a rebuilt table
// for files whose own is broken, and encrypted files (unlock: strings and streams come out
// decrypted, PdfCrypt.h).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enp::model::viewer::pdf {

struct DictEntry;

struct Object {
    enum class Type : std::uint8_t { Null, Bool, Number, String, Name, Array, Dict, Ref, Stream, Operator };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;         // Number; Ref: the object number
    int generation = 0;          // Ref
    std::string text;            // String (bytes), Name (without '/'), Operator
    std::vector<Object> array;
    std::vector<DictEntry> dict; // Dict and Stream
    std::size_t streamStart = 0, streamLength = 0; // Stream: where its raw data lies in the file
    // Stream: the indirect object it is (its key in an encrypted file); 0 when not known (a
    // cross-reference stream read before the table, which is never encrypted).
    int objectNumber = 0, objectGeneration = 0;

    bool isNull() const { return type == Type::Null; }
    bool isNumber() const { return type == Type::Number; }
    bool isName(std::string_view n) const { return type == Type::Name && text == n; }
    bool isDict() const { return type == Type::Dict || type == Type::Stream; }
    const Object* get(std::string_view key) const; // in a Dict / Stream, or null
    double num(double otherwise = 0.0) const { return type == Type::Number ? number : otherwise; }
};

class Security;

struct DictEntry {
    std::string key;
    Object value;
};

// Tokens and objects over a byte range.
class Lexer {
public:
    Lexer(std::string_view data, std::size_t at = 0) : data_(data), at_(at) {}
    // The next object; operators (in content streams) come back as Type::Operator. False at the end.
    bool next(Object& out, int depth = 0);
    std::size_t position() const { return at_; }
    void seek(std::size_t at) { at_ = at; }
    void skipSpace();
    std::string_view data() const { return data_; }
    bool atEnd() { skipSpace(); return at_ >= data_.size(); }

private:
    bool readString(std::string& out);
    bool readHexString(std::string& out);
    std::string readName();
    std::string_view data_;
    std::size_t at_ = 0;
};

class File {
public:
    static constexpr std::size_t kMaxStreamBytes = std::size_t(128) << 20;

    File();
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    // False (and `error`) when it is not a PDF this reads.
    bool open(std::string bytes, std::string* error);
    // An encrypted file (encrypted()) is read once unlocked with its user's or owner's password
    // ("" first: most encrypted files only restrict printing or copying, and open without one).
    enum class Unlock { Opened, NeedsPassword, Unsupported };
    Unlock unlock(std::string_view password, std::string* error);

    // The object an indirect reference names (cached); `o` itself when it is not a reference.
    const Object& resolve(const Object& o);
    const Object* resolvedGet(const Object& dict, std::string_view key); // dict[key], resolved; null if absent
    // A stream's data with its filters undone (images keep DCT / JPX: `rawImage` says so).
    std::optional<std::string> streamData(const Object& stream, std::string* imageFilter = nullptr);
    const Object& trailer() const { return trailer_; }
    bool encrypted() const { return encrypted_; }
    std::string_view bytes() const { return bytes_; }

private:
    struct XrefEntry {
        std::uint8_t type = 0; // 1: at an offset, 2: in an object stream
        std::size_t offset = 0;  // type 1: offset; type 2: the object stream's number
        int index = 0;           // type 2: index in the stream
    };
    bool readXref(std::size_t offset, int depth);
    bool readXrefTable(Lexer& lex);
    bool readXrefStream(const Object& stream);
    void rebuildXref();
    std::optional<Object> loadObject(int number);
    std::optional<Object> parseAt(std::size_t offset, int expected);
    std::optional<Object> fromObjectStream(int stream, int index, int number);
    std::size_t streamLength(const Object& dict, std::size_t start);
    void decryptStrings(Object& o, int number, int generation, int depth) const;

    std::string bytes_;
    std::map<int, XrefEntry> xref_;
    std::map<int, std::unique_ptr<Object>> cache_;
    std::map<int, std::vector<std::pair<int, std::size_t>>> objStmIndex_; // per object stream: (number, offset)
    std::map<int, std::string> objStmData_;
    Object trailer_;
    Object null_;
    bool encrypted_ = false;
    bool rebuilt_ = false;               // the table was rebuilt (read again once unlocked)
    std::unique_ptr<Security> security_; // unlocked: decrypts strings and streams
    int encryptNumber_ = -1;             // the /Encrypt dictionary's object: never encrypted
    int resolving_ = 0; // depth guard
};

// Filters (exposed for tests).
std::optional<std::string> decodeFilter(std::string_view name, std::string data, const Object* parms, std::size_t limit);

} // namespace enp::model::viewer::pdf
