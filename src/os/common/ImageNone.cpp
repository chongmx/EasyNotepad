// Platforms without a decoder of their own here (Linux, the web): the caller decodes pictures.
#include "enp/os/Image.h"

namespace enp::os::image {

bool decodeJpeg(std::string_view, int, std::vector<unsigned char>&, int&, int&)
{
    return false;
}

} // namespace enp::os::image
