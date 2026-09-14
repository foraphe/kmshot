#pragma once

#include <cstdint>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

namespace kmshot
{

// Imports a DRM DMA-BUF into an EGL/GLES context and reads it back as
// interleaved RGBA float, optionally using a GPU FP32 (RGBA32F) colour buffer
// and falling back to RGBA8 when the driver cannot provide one.
class DmabufGlReader
{
public:
    ~DmabufGlReader();

    bool init(int card_fd);

    bool context_lost() const { return context_lost_; }

    bool read_dmabuf_to_rgba32f(
        int dmabuf_fd,
        uint32_t fb_width,
        uint32_t fb_height,
        uint32_t out_width,
        uint32_t out_height,
        float uv_off_x,
        float uv_off_y,
        float uv_scale_x,
        float uv_scale_y,
        uint32_t fourcc,
        uint32_t pitch0,
        uint32_t offset0,
        uint64_t modifier,
        bool dmabuf_sync,
        std::vector<float> &out_rgba32f);

private:
    bool ensure_readback_target(uint32_t w, uint32_t h);
    GLuint compile_shader(GLenum type, const char *src);
    bool build_blit_program();
    bool gl_has_error(const char *stage);
    void shutdown();

    gbm_device *gbm_{nullptr};
    EGLDisplay egl_dpy_{EGL_NO_DISPLAY};
    EGLContext egl_ctx_{EGL_NO_CONTEXT};
    EGLSurface egl_surf_{EGL_NO_SURFACE};
    bool surfaceless_{false};
    int gles_major_{2};
    bool high_precision_path_{false};

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_{nullptr};
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_{nullptr};
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_{nullptr};

    GLuint import_tex_{0};
    GLuint readback_tex_{0};
    GLuint fbo_{0};
    GLuint prog_{0};
    GLuint vbo_{0};

    GLint loc_pos_{-1};
    GLint loc_uv_{-1};
    GLint loc_tex_{-1};
    GLint loc_uv_off_{-1};
    GLint loc_uv_scale_{-1};

    bool sync_warned_{false};
    bool context_lost_{false};
    uint32_t rb_w_{0};
    uint32_t rb_h_{0};
};

} // namespace kmshot
