#pragma once

#include <cstdint>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>

#include "color_transform.hpp"

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

namespace kmshot
{

// Imports a DRM DMA-BUF into an EGL/GLES context and reads it back, either as
// interleaved RGBA float or as full-range YUV444 16-bit with the colour
// transform executed on the GPU.
class DmabufGlReader
{
public:
    ~DmabufGlReader();

    bool init(int card_fd);

    bool context_lost() const { return context_lost_; }

    // True when the GPU colour pipeline (colour shader + FP32 render target) is
    // usable. Checked once after init(); if a frame still fails the caller is
    // expected to fall back to the CPU transform.
    bool supports_gpu_color() const { return color_program_.valid() && high_precision_path_; }

    // True when the colour shader can write the three YUV planes into separate
    // 16-bit targets (MRT), which removes the CPU de-interleave entirely. Only
    // set when the driver actually passed the runtime readback probe in init().
    bool supports_planar_yuv() const { return planar_program_.valid() && planar_readback_ok_; }

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

    // Runs the whole colour transform (source decode -> display->target matrix
    // -> target transfer -> RGB->YUV) in the fragment shader and reads the
    // result back as full-range 16-bit YUV444 planes.
    //
    // Returns false when the GPU colour path is unavailable or fails; the
    // caller must then fall back to read_dmabuf_to_rgba32f() plus the CPU
    // transform.
    bool read_dmabuf_to_yuv444p16(
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
        const ColorTransformConfig &color,
        std::vector<uint16_t> &y,
        std::vector<uint16_t> &u,
        std::vector<uint16_t> &v);

    // Same as above but stops after the target transfer function, returning
    // interleaved 16-bit RGB. This is what libavif wants: it does the RGB->YUV
    // conversion (and the chroma downsampling) itself.
    //
    // Requires the 16-bit readback target; returns false otherwise so the caller
    // can fall back to the CPU path.
    bool read_dmabuf_to_rgb16(
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
        const ColorTransformConfig &color,
        std::vector<uint16_t> &rgb);

private:
    // Pixel layout of the offscreen render target the full-screen pass draws
    // into, which also determines how it is read back.
    enum class ReadbackFormat
    {
        Rgba8,
        Rgba16Unorm, // 16-bit unorm: glReadPixels(GL_UNSIGNED_SHORT)
        Rgba32f,
    };

    // Uniform/attribute locations shared by every full-screen pass.
    struct Program
    {
        GLuint id{0};
        GLint pos{-1};
        GLint uv{-1};
        GLint tex{-1};
        GLint uv_off{-1};
        GLint uv_scale{-1};

        bool valid() const
        {
            return id != 0 && pos >= 0 && uv >= 0 && tex >= 0 && uv_off >= 0 && uv_scale >= 0;
        }
    };

    // The colour pass adds the transform uniforms on top of the common ones.
    struct ColorProgram : Program
    {
        GLint display_to_target{-1};
        GLint rgb_to_yuv{-1};
        GLint decode_gamma{-1};
        GLint pq_scale{-1};
        GLint mode{-1};
        GLint output{-1};

        bool valid() const
        {
            return Program::valid() && display_to_target >= 0 && rgb_to_yuv >= 0 &&
                   decode_gamma >= 0 && pq_scale >= 0 && mode >= 0 && output >= 0;
        }
    };

    void set_color_uniforms(const ColorTransformConfig &color, bool output_rgb);

    // ES3 variant of the colour pass that writes Y, U and V into three separate
    // R16 targets instead of one packed RGBA target.
    struct PlanarProgram : Program
    {
        GLint display_to_target{-1};
        GLint rgb_to_yuv{-1};
        GLint decode_gamma{-1};
        GLint pq_scale{-1};
        GLint mode{-1};

        bool valid() const
        {
            return Program::valid() && display_to_target >= 0 && rgb_to_yuv >= 0 &&
                   decode_gamma >= 0 && pq_scale >= 0 && mode >= 0;
        }
    };

    // Shared DMA-BUF import / full-screen draw / teardown.
    bool begin_image(int dmabuf_fd,
                     uint32_t fb_width,
                     uint32_t fb_height,
                     uint32_t fourcc,
                     uint32_t pitch0,
                     uint32_t offset0,
                     uint64_t modifier,
                     bool dmabuf_sync);
    void end_image(int dmabuf_fd, bool dmabuf_sync);
    void draw_quad(const Program &program,
                   float uv_off_x,
                   float uv_off_y,
                   float uv_scale_x,
                   float uv_scale_y);

    bool build_planar_program();
    bool ensure_plane_targets(uint32_t w, uint32_t h);
    bool verify_planar_readback();
    bool render_planar(int dmabuf_fd,
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
                       std::vector<uint16_t> &y,
                       std::vector<uint16_t> &u,
                       std::vector<uint16_t> &v);

    void attach_readback_target();
    bool framebuffer_complete();
    bool verify_fp32_readback();
    bool ensure_readback_target(uint32_t w, uint32_t h, bool prefer_u16);
    bool readback_to_float(const std::vector<uint8_t> &raw, std::vector<float> &out) const;
    GLuint compile_shader(GLenum type, const char *src);
    bool link_program(Program &program, const char *vertex_shader, const char *fragment_shader);
    bool build_blit_program();
    bool build_color_program();
    bool gl_has_error(const char *stage);
    void shutdown();

    // DMA-BUF import, full-screen draw with `program` and readback into
    // `out_bytes`, laid out according to readback_format_. The program's common
    // uniforms are set from the arguments.
    bool render_and_readback(
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
        const Program &program,
        bool prefer_u16,
        std::vector<uint8_t> &out_bytes);

    gbm_device *gbm_{nullptr};
    EGLDisplay egl_dpy_{EGL_NO_DISPLAY};
    EGLContext egl_ctx_{EGL_NO_CONTEXT};
    EGLSurface egl_surf_{EGL_NO_SURFACE};
    bool surfaceless_{false};
    int gles_major_{2};
    bool high_precision_path_{false};
    ReadbackFormat readback_format_{ReadbackFormat::Rgba8};

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_{nullptr};
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_{nullptr};
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_{nullptr};
    EGLImageKHR image_{EGL_NO_IMAGE_KHR};

    GLuint import_tex_{0};
    GLuint readback_tex_{0};
    GLuint fbo_{0};
    Program blit_program_;
    ColorProgram color_program_;
    PlanarProgram planar_program_;
    GLuint plane_tex_[3]{0, 0, 0};
    uint32_t plane_w_{0};
    uint32_t plane_h_{0};
    bool planar_readback_ok_{false};
    GLuint vbo_{0};

    bool sync_warned_{false};
    bool context_lost_{false};
    uint32_t rb_w_{0};
    uint32_t rb_h_{0};

    // Reused between frames: allocating (and zero-filling) a fresh 25-50 MB
    // buffer every frame showed up as ~25% of the capture CPU time.
    std::vector<uint8_t> readback_bytes_;
};

} // namespace kmshot
