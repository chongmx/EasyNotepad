// PDF's standard security handler (ISO 32000-2, 7.6.4; internal to the PDF reader): the file's
// key from a password, then each string and stream of the file decrypted with it. RC4 with 40 to
// 128-bit keys (revisions 2 to 4), AES-128 (revision 4) and AES-256 (revisions 5 and 6), with
// crypt filters (a method for streams, one for strings, a stream naming its own). A password is
// the user's (opens the file) or the owner's (opens it too). Files encrypted for certain people
// (a public-key handler, certificates) are not read.
#pragma once

#include "PdfObjects.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace enp::model::viewer::pdf {

class Security {
public:
    enum class Result { Opened, WrongPassword, Unsupported };

    // From the file's /Encrypt dictionary (`encrypt`, its members resolved through `file`) and the
    // first string of the trailer's /ID. Unsupported: `error` says why.
    Result open(File& file, const Object& encrypt, std::string_view firstId, std::string_view password, std::string* error);

    // A string of indirect object `number` `generation`, decrypted.
    std::string string(int number, int generation, std::string_view data) const;
    // A stream's raw data, decrypted: with its own crypt filter when it names one (`filter`, the
    // /Crypt filter's /Name: "Identity" leaves it as it is), else the file's method for streams.
    std::string stream(int number, int generation, std::string_view data, std::string_view filter = {}) const;
    // Whether the document's metadata streams are encrypted (/EncryptMetadata).
    bool encryptsMetadata() const { return encryptMetadata_; }

private:
    enum class Method : std::uint8_t { None, Rc4, Aes128, Aes256 };
    std::string decrypt(Method method, int number, int generation, std::string_view data) const;
    bool tryLegacy(std::string_view password, int revision, std::string_view o, std::string_view u, std::uint32_t p,
                   std::string_view id);
    bool tryAes256(std::string_view password, int revision, std::string_view o, std::string_view u, std::string_view oe,
                   std::string_view ue);

    Method strings_ = Method::None, streams_ = Method::None;
    std::map<std::string, Method, std::less<>> filters_; // the crypt filters by name
    std::string key_;
    std::size_t keyBytes_ = 5;
    bool encryptMetadata_ = true;
};

} // namespace enp::model::viewer::pdf
