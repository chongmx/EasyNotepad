// What a file is before it is loaded: text to edit, an image to show, or binary data that must
// not be poured into a text buffer. Decided from the content (magic numbers, NUL bytes in the
// first kilobytes) with the extension as a hint.
#pragma once

#include <string>
#include <string_view>

namespace enp::model {

enum class FileKind { Text, Image, Binary };

struct FileType {
    FileKind kind = FileKind::Text;
    std::string format; // "PNG", "JPEG", "WebP", "ELF executable", ... ; empty for plain text
};

// `bytes` may be the whole file or its first kilobytes.
FileType detectFileType(std::string_view path, std::string_view bytes);
// The extension (lower case, without the dot) names an image format the viewer knows about.
bool isImageExtension(std::string_view extension);

} // namespace enp::model
