#include "FrameCapture.h"

#include "imgui_impl_opengl3.h"

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
#   include <GLES3/gl3.h>
#else
#   include <glad/glad.h>
#endif

#include <algorithm>
#include <cstring>

namespace enp::app {

bool captureFrame(ImDrawData* drawData, ImVec4 clear, int width, int height, std::vector<unsigned char>& rgba, std::string* failure)
{
    if (!drawData || width <= 0 || height <= 0) {
        if (failure)
            *failure = "the window has no picture (minimized?)";
        return false;
    }
    GLint previousFramebuffer = 0, previousTexture = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    GLuint texture = 0, framebuffer = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
        glViewport(0, 0, width, height);
        glClearColor(clear.x, clear.y, clear.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(drawData);
        rgba.resize(size_t(width) * size_t(height) * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        // GL's rows start at the bottom; a picture's at the top. Blending leaves alpha below 255.
        const size_t row = size_t(width) * 4;
        for (int y = 0; y < height / 2; ++y)
            std::swap_ranges(rgba.begin() + std::ptrdiff_t(size_t(y) * row), rgba.begin() + std::ptrdiff_t(size_t(y + 1) * row),
                             rgba.begin() + std::ptrdiff_t(size_t(height - 1 - y) * row));
        for (size_t i = 3; i < rgba.size(); i += 4)
            rgba[i] = 255;
    } else if (failure) {
        *failure = "the offscreen framebuffer is not supported here";
    }
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previousFramebuffer));
    glBindTexture(GL_TEXTURE_2D, GLuint(previousTexture));
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    return complete;
}

} // namespace enp::app
