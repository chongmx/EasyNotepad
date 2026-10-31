#include "enp/model/Document.h"

#include <cctype>

namespace enp::model {

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

} // namespace enp::model
