// The app's own picture, read back from the GPU: the frame's Dear ImGui draw data is drawn once more
// into an offscreen framebuffer and read from there, so the picture is whole whatever covers the
// window on screen (reading the window's own buffer would give the other windows' pixels, or
// nothing, where they overlap it).
#pragma once

#include "imgui.h"

#include <string>
#include <vector>

namespace enp::app {

// `width` x `height`: the framebuffer size the frame was drawn for. On success `rgba` holds
// width * height * 4 bytes, top row first, opaque; on failure `*failure` says why.
bool captureFrame(ImDrawData* drawData, ImVec4 clear, int width, int height, std::vector<unsigned char>& rgba, std::string* failure);

} // namespace enp::app
