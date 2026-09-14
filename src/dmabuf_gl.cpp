#include "dmabuf_gl.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

#include <errno.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>

#include <drm_fourcc.h>

#include "drm_util.hpp"

// ES3 entry points used by the planar colour path. The header only pulls in the
// GLES2 headers, so declare them here; they resolve from libGLESv2, and are only
// ever called when the context really is ES3.
extern "C"
{
    void glDrawBuffers(GLsizei n, const GLenum *bufs);
    void glReadBuffer(GLenum mode);
    void glClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat *value);
}

namespace kmshot
{

namespace
{

const char *egl_error_string(EGLint e)
{
    switch (e)
    {
    case EGL_SUCCESS: return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
    case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
    case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
    case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
    case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
    case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
    case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
    case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
    default: return "EGL_UNKNOWN_ERROR";
    }
}

bool has_extension(const char *ext_list, const char *ext)
{
    if (!ext_list || !ext)
        return false;
    const std::string all(ext_list);
    const std::string needle = std::string(" ") + ext + " ";
    return (std::string(" ") + all + " ").find(needle) != std::string::npos;
}

// GLSL mat3 is column-major while Mat3 is row-major.
void mat3_to_column_major(const Mat3 &m, GLfloat out[9])
{
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            out[col * 3 + row] = static_cast<GLfloat>(m.m[row][col]);
}

} // namespace

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif
#ifndef GL_CONTEXT_LOST
#define GL_CONTEXT_LOST 0x0507
#endif
#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
#endif
#ifndef GL_RGBA16
#define GL_RGBA16 0x805B
#endif
#ifndef GL_R16
#define GL_R16 0x822A
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_COLOR
#define GL_COLOR 0x1800
#endif
#ifndef GL_DRAW_BUFFER
#define GL_DRAW_BUFFER 0x0C01
#endif
#ifndef GL_COLOR_ATTACHMENT1
#define GL_COLOR_ATTACHMENT1 0x8CE1
#endif
#ifndef GL_COLOR_ATTACHMENT2
#define GL_COLOR_ATTACHMENT2 0x8CE2
#endif

DmabufGlReader::~DmabufGlReader()
{
    shutdown();
}

bool DmabufGlReader::init(int card_fd)
{
    gbm_ = gbm_create_device(card_fd);
    if (!gbm_)
    {
        std::cerr << "gbm_create_device failed\n";
        return false;
    }

    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!get_platform_display)
    {
        std::cerr << "eglGetPlatformDisplayEXT not available\n";
        return false;
    }

    egl_dpy_ = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm_, nullptr);
    if (egl_dpy_ == EGL_NO_DISPLAY)
    {
        std::cerr << "eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR) failed\n";
        return false;
    }

    if (!eglInitialize(egl_dpy_, nullptr, nullptr))
    {
        const EGLint err = eglGetError();
        std::cerr << "eglInitialize failed: " << egl_error_string(err)
                  << " (0x" << std::hex << err << std::dec << ")\n";
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API))
    {
        const EGLint err = eglGetError();
        std::cerr << "eglBindAPI(EGL_OPENGL_ES_API) failed: " << egl_error_string(err)
                  << " (0x" << std::hex << err << std::dec << ")\n";
        return false;
    }

    const char *exts = eglQueryString(egl_dpy_, EGL_EXTENSIONS);
    const bool surfaceless_ok = has_extension(exts, "EGL_KHR_surfaceless_context");

    const EGLint cfg_attrs_strict[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE};
    const EGLint cfg_attrs_relaxed[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE};
    const EGLint cfg_attrs_surfaceless[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE};

    EGLConfig cfg{};
    EGLint num{};

    bool have_cfg = eglChooseConfig(egl_dpy_, cfg_attrs_strict, &cfg, 1, &num) && num > 0;
    if (!have_cfg)
    {
        have_cfg = eglChooseConfig(egl_dpy_, cfg_attrs_relaxed, &cfg, 1, &num) && num > 0;
    }
    if (!have_cfg && surfaceless_ok)
    {
        have_cfg = eglChooseConfig(egl_dpy_, cfg_attrs_surfaceless, &cfg, 1, &num) && num > 0;
        surfaceless_ = have_cfg;
    }

    if (!have_cfg)
    {
        const EGLint err = eglGetError();
        std::cerr << "eglChooseConfig failed: " << egl_error_string(err)
                  << " (0x" << std::hex << err << std::dec << ")\n";
        return false;
    }

    if (!surfaceless_)
    {
        const EGLint pb_attrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        egl_surf_ = eglCreatePbufferSurface(egl_dpy_, cfg, pb_attrs);
        if (egl_surf_ == EGL_NO_SURFACE)
        {
            if (surfaceless_ok)
            {
                surfaceless_ = true;
            }
            else
            {
                const EGLint err = eglGetError();
                std::cerr << "eglCreatePbufferSurface failed: " << egl_error_string(err)
                          << " (0x" << std::hex << err << std::dec << ")\n";
                return false;
            }
        }
    }

    // Prefer GLES3 for the high-bit-depth readback path, fall back to GLES2.
    const EGLint ctx_attrs3[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLint ctx_attrs2[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    egl_ctx_ = eglCreateContext(egl_dpy_, cfg, EGL_NO_CONTEXT, ctx_attrs3);
    if (egl_ctx_ != EGL_NO_CONTEXT)
    {
        gles_major_ = 3;
    }
    else
    {
        egl_ctx_ = eglCreateContext(egl_dpy_, cfg, EGL_NO_CONTEXT, ctx_attrs2);
        gles_major_ = 2;
    }

    if (egl_ctx_ == EGL_NO_CONTEXT)
    {
        const EGLint err = eglGetError();
        std::cerr << "eglCreateContext failed: " << egl_error_string(err)
                  << " (0x" << std::hex << err << std::dec << ")\n";
        return false;
    }

    EGLSurface s = surfaceless_ ? EGL_NO_SURFACE : egl_surf_;
    if (!eglMakeCurrent(egl_dpy_, s, s, egl_ctx_))
    {
        const EGLint err = eglGetError();
        std::cerr << "eglMakeCurrent failed: " << egl_error_string(err)
                  << " (0x" << std::hex << err << std::dec << ")\n";
        return false;
    }

    const char *gl_exts = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
    if (!has_extension(gl_exts, "GL_OES_EGL_image_external"))
    {
        std::cerr << "Missing GL_OES_EGL_image_external\n";
        return false;
    }

    const bool has_float_color = has_extension(gl_exts, "GL_EXT_color_buffer_float");

    high_precision_path_ = (gles_major_ >= 3) && has_float_color;
    std::cerr << "GLES " << gles_major_ << ": FP32 colour buffers "
              << (high_precision_path_ ? "available" : "unavailable") << "\n";

    eglCreateImageKHR_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    eglDestroyImageKHR_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    glEGLImageTargetTexture2DOES_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));

    if (!eglCreateImageKHR_ || !eglDestroyImageKHR_ || !glEGLImageTargetTexture2DOES_)
    {
        std::cerr << "Required EGL/GL DMA-BUF import symbols not available\n";
        return false;
    }

    glGenTextures(1, &import_tex_);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, import_tex_);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &readback_tex_);
    glBindTexture(GL_TEXTURE_2D, readback_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &fbo_);

    if (!build_blit_program())
    {
        std::cerr << "Failed to build blit shader\n";
        return false;
    }

    // Best effort: without it the capture falls back to the CPU transform.
    if (high_precision_path_ && build_color_program())
        std::cerr << "GPU colour pipeline: available\n";
    else
        std::cerr << "GPU colour pipeline: unavailable\n";

    // The planar writer is an optimisation on top of the colour pipeline. It is
    // probed at runtime because multi-render-target R16 support varies between
    // drivers; on failure the interleaved readback above stays in use.
    planar_readback_ok_ = false;
    if (getenv("KMSHOT_NO_PLANAR"))
        std::cerr << "Planar YUV writeback: disabled by KMSHOT_NO_PLANAR\n";
    else if (color_program_.valid() && gles_major_ >= 3 && build_planar_program())
    {
        planar_readback_ok_ = verify_planar_readback();
        std::cerr << "Planar YUV writeback: " << (planar_readback_ok_ ? "available" : "unavailable")
                  << "\n";
    }
    else
    {
        std::cerr << "Planar YUV writeback: unavailable\n";
    }

    const GLfloat quad[] = {
        -1.f, -1.f, 0.f, 0.f,
        1.f, -1.f, 1.f, 0.f,
        -1.f, 1.f, 0.f, 1.f,
        1.f, 1.f, 1.f, 1.f,
    };
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    return true;
}

bool DmabufGlReader::begin_image(int dmabuf_fd,
                                 uint32_t fb_width,
                                 uint32_t fb_height,
                                 uint32_t fourcc,
                                 uint32_t pitch0,
                                 uint32_t offset0,
                                 uint64_t modifier,
                                 bool dmabuf_sync)
{
    if (dmabuf_sync)
    {
        dma_buf_sync s{};
        s.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
        if (::ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &s) != 0 && !sync_warned_ &&
            errno != ENOTTY && errno != EINVAL && errno != ENOSYS)
        {
            sync_warned_ = true;
            std::cerr << "DMA_BUF_IOCTL_SYNC failed: " << std::strerror(errno) << "\n";
        }
    }

    std::vector<EGLint> attrs = {
        EGL_WIDTH,
        static_cast<EGLint>(fb_width),
        EGL_HEIGHT,
        static_cast<EGLint>(fb_height),
        EGL_LINUX_DRM_FOURCC_EXT,
        static_cast<EGLint>(fourcc),
        EGL_DMA_BUF_PLANE0_FD_EXT,
        dmabuf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        static_cast<EGLint>(offset0),
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        static_cast<EGLint>(pitch0),
    };
    if (modifier != DRM_FORMAT_MOD_INVALID)
    {
        attrs.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT);
        attrs.push_back(static_cast<EGLint>(modifier & 0xFFFFFFFFu));
        attrs.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT);
        attrs.push_back(static_cast<EGLint>((modifier >> 32) & 0xFFFFFFFFu));
    }
    attrs.push_back(EGL_NONE);

    image_ = eglCreateImageKHR_(
        egl_dpy_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs.data());
    if (image_ == EGL_NO_IMAGE_KHR)
    {
        std::cerr << "eglCreateImageKHR failed for fourcc=" << fourcc_to_string(fourcc) << "\n";
        end_image(dmabuf_fd, dmabuf_sync);
        return false;
    }

    glBindTexture(GL_TEXTURE_EXTERNAL_OES, import_tex_);
    glEGLImageTargetTexture2DOES_(GL_TEXTURE_EXTERNAL_OES, image_);
    if (gl_has_error("glEGLImageTargetTexture2DOES"))
    {
        end_image(dmabuf_fd, dmabuf_sync);
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    return true;
}

void DmabufGlReader::end_image(int dmabuf_fd, bool dmabuf_sync)
{
    if (image_ != EGL_NO_IMAGE_KHR)
    {
        eglDestroyImageKHR_(egl_dpy_, image_);
        image_ = EGL_NO_IMAGE_KHR;
    }

    if (!dmabuf_sync)
        return;

    dma_buf_sync s{};
    s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    if (::ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &s) != 0 && !sync_warned_ &&
        errno != ENOTTY && errno != EINVAL && errno != ENOSYS)
    {
        sync_warned_ = true;
        std::cerr << "DMA_BUF_IOCTL_SYNC failed: " << std::strerror(errno) << "\n";
    }
}

void DmabufGlReader::draw_quad(const Program &program,
                               float uv_off_x,
                               float uv_off_y,
                               float uv_scale_x,
                               float uv_scale_y)
{
    glUseProgram(program.id);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(static_cast<GLuint>(program.pos));
    glEnableVertexAttribArray(static_cast<GLuint>(program.uv));
    glVertexAttribPointer(static_cast<GLuint>(program.pos), 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), reinterpret_cast<void *>(0));
    glVertexAttribPointer(static_cast<GLuint>(program.uv), 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), reinterpret_cast<void *>(2 * sizeof(GLfloat)));
    glUniform2f(program.uv_off, uv_off_x, uv_off_y);
    glUniform2f(program.uv_scale, uv_scale_x, uv_scale_y);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, import_tex_);
    glUniform1i(program.tex, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(static_cast<GLuint>(program.pos));
    glDisableVertexAttribArray(static_cast<GLuint>(program.uv));
}

bool DmabufGlReader::render_and_readback(
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
    std::vector<uint8_t> &out_bytes)
{
    if (!begin_image(dmabuf_fd, fb_width, fb_height, fourcc, pitch0, offset0, modifier, dmabuf_sync))
        return false;

    if (!ensure_readback_target(out_width, out_height, prefer_u16))
    {
        end_image(dmabuf_fd, dmabuf_sync);
        return false;
    }

    // Single target: also resets the draw buffer state after a planar pass set
    // up three attachments. glDrawBuffers is ES3-only, so skip it otherwise.
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, readback_tex_, 0);
    if (gles_major_ >= 3)
    {
        const GLenum one = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &one);
    }
    if (!framebuffer_complete())
    {
        std::cerr << "Framebuffer incomplete\n";
        end_image(dmabuf_fd, dmabuf_sync);
        return false;
    }

    glViewport(0, 0, static_cast<GLsizei>(out_width), static_cast<GLsizei>(out_height));
    draw_quad(program, uv_off_x, uv_off_y, uv_scale_x, uv_scale_y);

    if (gl_has_error("glDrawArrays"))
    {
        end_image(dmabuf_fd, dmabuf_sync);
        return false;
    }

    const size_t px_count = static_cast<size_t>(out_width) * out_height;
    GLenum read_type = GL_UNSIGNED_SHORT;
    size_t bytes_per_pixel = 4u * sizeof(uint16_t);

    if (readback_format_ == ReadbackFormat::Rgba32f)
    {
        read_type = GL_FLOAT;
        bytes_per_pixel = 4u * sizeof(float);
    }
    else if (readback_format_ == ReadbackFormat::Rgba8)
    {
        read_type = GL_UNSIGNED_BYTE;
        bytes_per_pixel = 4u;
    }

    out_bytes.resize(px_count * bytes_per_pixel);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(
        0, 0,
        static_cast<GLsizei>(out_width),
        static_cast<GLsizei>(out_height),
        GL_RGBA,
        read_type,
        out_bytes.data());
    glFinish();

    if (gl_has_error("glReadPixels"))
    {
        end_image(dmabuf_fd, dmabuf_sync);
        return false;
    }

    end_image(dmabuf_fd, dmabuf_sync);
    return true;
}

bool DmabufGlReader::read_dmabuf_to_rgba32f(
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
    std::vector<float> &out_rgba32f)
{
    if (!render_and_readback(
            dmabuf_fd, fb_width, fb_height, out_width, out_height,
            uv_off_x, uv_off_y, uv_scale_x, uv_scale_y,
            fourcc, pitch0, offset0, modifier, dmabuf_sync,
            blit_program_, false, 
            readback_bytes_))
    {
        return false;
    }

    return readback_to_float(readback_bytes_, out_rgba32f);
}

namespace
{

// Uploads the colour transform uniforms. Works for both the packed colour
// program and the planar one; `output_loc` is -1 when the program has no output
// selector.
template <typename ProgramT>
void apply_color_uniforms(const ProgramT &program,
                          const ColorTransformConfig &color,
                          GLint output_loc,
                          int output_value)
{
    // 0: SDR native primaries, 1: HDR already PQ, 2: HDR gamma 2.2 -> PQ,
    // 3: assumed target (pass through).
    GLint mode = 3;
    switch (color.source)
    {
    case SourceEncoding::SdrDisplayNative:
        mode = 0;
        break;
    case SourceEncoding::HdrPqBt2020:
        mode = color.pq_input_is_gamma22 ? 2 : 1;
        break;
    case SourceEncoding::AssumedTarget:
        mode = 3;
        break;
    }

    GLfloat display_to_target[9];
    GLfloat rgb_to_yuv[9];
    mat3_to_column_major(color.display_to_target, display_to_target);
    mat3_to_column_major(color.target_rgb_to_yuv, rgb_to_yuv);

    glUseProgram(program.id);
    glUniformMatrix3fv(program.display_to_target, 1, GL_FALSE, display_to_target);
    glUniformMatrix3fv(program.rgb_to_yuv, 1, GL_FALSE, rgb_to_yuv);
    glUniform1f(program.decode_gamma, static_cast<GLfloat>(color.display_decode_gamma));
    glUniform1f(program.pq_scale, static_cast<GLfloat>(color.pq_scale));
    glUniform1i(program.mode, mode);
    if (output_loc >= 0)
        glUniform1i(output_loc, output_value);
}

} // namespace

void DmabufGlReader::set_color_uniforms(const ColorTransformConfig &color, bool output_rgb)
{
    apply_color_uniforms(color_program_, color, color_program_.output, output_rgb ? 1 : 0);
}

bool DmabufGlReader::ensure_plane_targets(uint32_t w, uint32_t h)
{
    if (plane_tex_[0] != 0 && plane_w_ == w && plane_h_ == h)
        return true;

    if (plane_tex_[0] == 0)
        glGenTextures(3, plane_tex_);

    for (int i = 0; i < 3; ++i)
    {
        glBindTexture(GL_TEXTURE_2D, plane_tex_[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16,
                     static_cast<GLsizei>(w), static_cast<GLsizei>(h),
                     0, GL_RED, GL_UNSIGNED_SHORT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    if (gl_has_error("glTexImage2D(plane R16)"))
        return false;

    plane_w_ = w;
    plane_h_ = h;
    return true;
}

bool DmabufGlReader::verify_planar_readback()
{
    // Renders known constants into three R16 targets and reads them back. This
    // checks the whole chain (MRT draw buffers, R16 renderability, GL_RED
    // readback) on the actual driver instead of trusting an extension string,
    // because support is not guaranteed everywhere.
    const GLsizei w = 4;
    const GLsizei h = 1;
    GLuint probe[3] = {0, 0, 0};
    glGenTextures(3, probe);
    for (int i = 0; i < 3; ++i)
    {
        glBindTexture(GL_TEXTURE_2D, probe[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16, w, h, 0, GL_RED, GL_UNSIGNED_SHORT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    if (gl_has_error("glTexImage2D(planar probe)"))
    {
        glDeleteTextures(3, probe);
        return false;
    }

    GLint prev_fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    for (int i = 0; i < 3; ++i)
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, probe[i], 0);

    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok)
    {
        const GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
        glDrawBuffers(3, bufs);

        const GLfloat values[3][4] = {
            {0.25f, 0.0f, 0.0f, 1.0f},
            {0.50f, 0.0f, 0.0f, 1.0f},
            {0.75f, 0.0f, 0.0f, 1.0f},
        };
        for (int i = 0; i < 3; ++i)
            glClearBufferfv(GL_COLOR, i, values[i]);
        glFinish();

        for (int i = 0; i < 3 && ok; ++i)
        {
            uint16_t got[4] = {0, 0, 0, 0};
            glReadBuffer(GL_COLOR_ATTACHMENT0 + i);
            glReadPixels(0, 0, w, h, GL_RED, GL_UNSIGNED_SHORT, got);
            const uint16_t expected = static_cast<uint16_t>(std::lround(values[i][0] * 65535.0));
            if (gl_has_error("planar probe readback") || got[0] != expected)
            {
                std::cerr << "Planar readback probe failed on plane " << i
                          << " (got " << got[0] << ", expected " << expected << ")\n";
                ok = false;
            }
        }

        const GLenum one = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &one);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    }

    // Detach before deleting: the attachments must not outlive the textures,
    // and the next single-target pass expects a clean attachment set.
    for (int i = 0; i < 3; ++i)
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, 0, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fb));
    glDeleteTextures(3, probe);
    return ok;
}

bool DmabufGlReader::render_planar(
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
    std::vector<uint16_t> &y,
    std::vector<uint16_t> &u,
    std::vector<uint16_t> &v)
{
    if (!begin_image(dmabuf_fd, fb_width, fb_height, fourcc, pitch0, offset0, modifier, dmabuf_sync))
        return false;

    if (!ensure_plane_targets(out_width, out_height))
    {
        end_image(dmabuf_fd, dmabuf_sync);
        return false;
    }

    const GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    for (int i = 0; i < 3; ++i)
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, plane_tex_[i], 0);
    glDrawBuffers(3, bufs);

    if (!framebuffer_complete())
    {
        std::cerr << "Planar framebuffer incomplete\n";
        end_image(dmabuf_fd, dmabuf_sync);
        return false;
    }

    glViewport(0, 0, static_cast<GLsizei>(out_width), static_cast<GLsizei>(out_height));
    draw_quad(planar_program_, uv_off_x, uv_off_y, uv_scale_x, uv_scale_y);

    if (gl_has_error("glDrawArrays(planar)"))
    {
        end_image(dmabuf_fd, dmabuf_sync);
        return false;
    }

    const size_t px = static_cast<size_t>(out_width) * out_height;
    std::vector<uint16_t> *planes[3] = {&y, &u, &v};
    bool ok = true;

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    for (int i = 0; i < 3 && ok; ++i)
    {
        if (planes[i]->size() != px)
            planes[i]->resize(px);
        glReadBuffer(GL_COLOR_ATTACHMENT0 + i);
        glReadPixels(0, 0, static_cast<GLsizei>(out_width), static_cast<GLsizei>(out_height),
                     GL_RED, GL_UNSIGNED_SHORT, planes[i]->data());
        if (gl_has_error("glReadPixels(planar)"))
            ok = false;
    }
    glFinish();

    // Leave single-target drawing behind for any later interleaved pass.
    const GLenum one = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &one);
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    end_image(dmabuf_fd, dmabuf_sync);
    return ok;
}

bool DmabufGlReader::read_dmabuf_to_yuv444p16(
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
    std::vector<uint16_t> &v)
{
    if (!supports_gpu_color())
        return false;

    // Preferred path: render each plane into its own R16 target with MRT and
    // read the planes back directly. That skips the CPU side de-interleave of
    // a 4-plane RGBA16 buffer, which used to be the largest remaining cost.
    if (supports_planar_yuv())
    {
        apply_color_uniforms(planar_program_, color, -1, 0);
        if (render_planar(
                dmabuf_fd, fb_width, fb_height, out_width, out_height,
                uv_off_x, uv_off_y, uv_scale_x, uv_scale_y,
                fourcc, pitch0, offset0, modifier, dmabuf_sync, y, u, v))
        {
            return true;
        }
        // Fall through to the interleaved path; a runtime failure here means
        // the planar probe was too optimistic, so stop trying.
        planar_readback_ok_ = false;
        std::cerr << "Planar YUV readback failed; falling back to interleaved\n";
    }

    set_color_uniforms(color, /*output_rgb=*/false);

    // The colour uniforms are per-program state, so render_and_readback() can
    // re-bind the same program without clearing them.
    if (!render_and_readback(
            dmabuf_fd, fb_width, fb_height, out_width, out_height,
            uv_off_x, uv_off_y, uv_scale_x, uv_scale_y,
            fourcc, pitch0, offset0, modifier, dmabuf_sync,
            color_program_, true, 
            readback_bytes_))
    {
        return false;
    }

    // With a 16-bit unorm target the samples are already final; only the
    // interleaved layout has to be split into planes.
    if (readback_format_ == ReadbackFormat::Rgba16Unorm)
    {
        const size_t px = static_cast<size_t>(out_width) * out_height;
        if (readback_bytes_.size() < px * 4u * sizeof(uint16_t))
            return false;

        return split_yuv444p16(
            reinterpret_cast<const uint16_t *>(readback_bytes_.data()), out_width, out_height, 4, y, u, v);
    }

    std::vector<float> interleaved;
    if (!readback_to_float(readback_bytes_, interleaved))
        return false;

    return quantize_yuv444p16(interleaved.data(), out_width, out_height, 4, y, u, v);
}

bool DmabufGlReader::read_dmabuf_to_rgb16(
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
    std::vector<uint16_t> &rgb)
{
    if (!supports_gpu_color())
        return false;

    set_color_uniforms(color, /*output_rgb=*/true);

    if (!render_and_readback(
            dmabuf_fd, fb_width, fb_height, out_width, out_height,
            uv_off_x, uv_off_y, uv_scale_x, uv_scale_y,
            fourcc, pitch0, offset0, modifier, dmabuf_sync,
            color_program_, true, 
            readback_bytes_))
    {
        return false;
    }

    // libavif consumes 16-bit RGB directly, so this path needs the 16-bit
    // target; anything else falls back to the CPU path in the caller.
    if (readback_format_ != ReadbackFormat::Rgba16Unorm)
        return false;

    const size_t px = static_cast<size_t>(out_width) * out_height;
    if (readback_bytes_.size() < px * 4u * sizeof(uint16_t))
        return false;

    const auto *src = reinterpret_cast<const uint16_t *>(readback_bytes_.data());
    rgb.resize(px * 3u);
    for (size_t i = 0; i < px; ++i)
    {
        rgb[i * 3u + 0] = src[i * 4u + 0];
        rgb[i * 3u + 1] = src[i * 4u + 1];
        rgb[i * 3u + 2] = src[i * 4u + 2];
    }

    return true;
}

bool DmabufGlReader::framebuffer_complete()
{
    // Pure status query: which textures are attached is the caller's business
    // (the planar path attaches three, the interleaved path one).
    GLint prev_fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fb));
    return complete;
}

bool DmabufGlReader::verify_fp32_readback()
{
    if (!framebuffer_complete())
        return false;

    GLint prev_fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);

    glViewport(0, 0, 1, 1);
    // < 1/255 so an 8-bit target quantizes to 0 while FP32 keeps it.
    // This only verifies if we have at least ~9 bits of precision
    // [FIXME] check for at least F16 precision instead of anything above 8 
    const float testv = 0.001f;
    glClearColor(testv, testv, testv, testv);
    glClear(GL_COLOR_BUFFER_BIT);

    float tmp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, tmp);
    glFinish();

    const bool ok = !gl_has_error("sanity-readback(glReadPixels)") && tmp[0] > 5e-4f;
    if (ok)
        std::cerr << "FP32 readback target verification passed (got " << tmp[0] << ")\n";

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fb));
    return ok;
}

void DmabufGlReader::attach_readback_target()
{
    GLint prev_fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, readback_tex_, 0);
    // A planar pass (or the planar probe) may have left attachments 1 and 2
    // set up. Leaving them behind keeps such an FBO "complete" while making
    // later single-target draws silently produce nothing, so clear them.
    if (gles_major_ >= 3)
    {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, 0, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fb));
}

bool DmabufGlReader::ensure_readback_target(uint32_t w, uint32_t h, bool prefer_u16)
{
    const ReadbackFormat want =
        prefer_u16 ? ReadbackFormat::Rgba16Unorm
                   : (high_precision_path_ ? ReadbackFormat::Rgba32f : ReadbackFormat::Rgba8);

    if (rb_w_ == w && rb_h_ == h && readback_format_ == want)
        return true;

    glBindTexture(GL_TEXTURE_2D, readback_tex_);

    // The colour pass only needs 16 bits per sample. Reading a 16-bit unorm
    // target back with glReadPixels(GL_UNSIGNED_SHORT) halves the transfer
    // compared to RGBA32F and already yields the final quantized samples, so the
    // CPU only has to de-interleave them. RGBA16 is not a required
    // colour-renderable format in GLES3, so it is probed first.
    if (want == ReadbackFormat::Rgba16Unorm)
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16, static_cast<GLsizei>(w), static_cast<GLsizei>(h),
                     0, GL_RGBA, GL_UNSIGNED_SHORT, nullptr);
        attach_readback_target();

        if (!gl_has_error("glTexImage2D(readback RGBA16)") && framebuffer_complete())
        {
            rb_w_ = w;
            rb_h_ = h;
            readback_format_ = ReadbackFormat::Rgba16Unorm;
            std::cerr << "Readback: RGBA16 unorm -> uint16\n";
            return true;
        }
        std::cerr << "Readback: RGBA16 target unusable; falling back to the float path\n";
    }

    if (high_precision_path_)
    {
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA32F,
            static_cast<GLsizei>(w),
            static_cast<GLsizei>(h),
            0,
            GL_RGBA,
            GL_FLOAT,
            nullptr);

        attach_readback_target();

        if (gl_has_error("glTexImage2D(readback RGBA32F)"))
        {
            std::cerr << "FP32 readback target unsupported at runtime; "
                         "falling back to RGBA8 path\n";
            high_precision_path_ = false;
        }
        else if (verify_fp32_readback())
        {
            rb_w_ = w;
            rb_h_ = h;
            readback_format_ = ReadbackFormat::Rgba32f;
            std::cerr << "Readback: RGBA32F -> float\n";
            return true;
        }
        else
        {
            std::cerr << "Readback: RGBA32F appears not usable at runtime -> falling back\n";
            high_precision_path_ = false;
        }
    }

    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA,
        static_cast<GLsizei>(w),
        static_cast<GLsizei>(h),
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        nullptr);

    if (gl_has_error("glTexImage2D(readback RGBA8)"))
        return false;

    rb_w_ = w;
    rb_h_ = h;
    readback_format_ = ReadbackFormat::Rgba8;
    std::cerr << "Readback: RGBA8 -> float\n";
    return true;
}

bool DmabufGlReader::readback_to_float(const std::vector<uint8_t> &raw, std::vector<float> &out) const
{
    const size_t px = static_cast<size_t>(rb_w_) * rb_h_;
    out.resize(px * 4u);

    switch (readback_format_)
    {
    case ReadbackFormat::Rgba8:
        if (raw.size() < out.size())
            return false;
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = static_cast<float>(raw[i]) * (1.0f / 255.0f);
        return true;

    case ReadbackFormat::Rgba16Unorm:
    {
        if (raw.size() < out.size() * sizeof(uint16_t))
            return false;
        const auto *p = reinterpret_cast<const uint16_t *>(raw.data());
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = static_cast<float>(p[i]) * (1.0f / 65535.0f);
        return true;
    }

    case ReadbackFormat::Rgba32f:
        if (raw.size() < out.size() * sizeof(float))
            return false;
        std::memcpy(out.data(), raw.data(), out.size() * sizeof(float));
        return true;
    }

    return false;
}

GLuint DmabufGlReader::compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    if (!s)
        return 0;

    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);

    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::cerr << "Shader compile failed: " << log << "\n";
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool DmabufGlReader::gl_has_error(const char *stage)
{
    bool failed = false;
    for (;;)
    {
        const GLenum e = glGetError();
        if (e == GL_NO_ERROR)
            break;
        failed = true;
        if (e == GL_CONTEXT_LOST)
        {
            context_lost_ = true;
            std::cerr << stage << ": GL_CONTEXT_LOST\n";
        }
        else
        {
            std::cerr << stage << ": gl error 0x" << std::hex << e << std::dec << "\n";
        }
    }
    return failed;
}

bool DmabufGlReader::link_program(Program &program, const char *vertex_shader, const char *fragment_shader)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader);
    if (!vs)
        return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader);
    if (!fs)
    {
        glDeleteShader(vs);
        return false;
    }

    program.id = glCreateProgram();
    glAttachShader(program.id, vs);
    glAttachShader(program.id, fs);
    glBindAttribLocation(program.id, 0, "a_pos");
    glBindAttribLocation(program.id, 1, "a_uv");
    glLinkProgram(program.id);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(program.id, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = {};
        glGetProgramInfoLog(program.id, sizeof(log), nullptr, log);
        std::cerr << "Program link failed: " << log << "\n";
        glDeleteProgram(program.id);
        program.id = 0;
        return false;
    }

    program.pos = glGetAttribLocation(program.id, "a_pos");
    program.uv = glGetAttribLocation(program.id, "a_uv");
    program.tex = glGetUniformLocation(program.id, "u_tex");
    program.uv_off = glGetUniformLocation(program.id, "u_uv_off");
    program.uv_scale = glGetUniformLocation(program.id, "u_uv_scale");
    return program.valid();
}

static const char *kVertexShaderEs2 = R"(
  attribute vec2 a_pos;
  attribute vec2 a_uv;
  varying vec2 v_uv;
  void main() {
    v_uv = a_uv;
    gl_Position = vec4(a_pos, 0.0, 1.0);
  }
)";

static const char *kVertexShaderEs3 = R"(
  #version 300 es
  in vec2 a_pos;
  in vec2 a_uv;
  out vec2 v_uv;
  void main() {
    v_uv = a_uv;
    gl_Position = vec4(a_pos, 0.0, 1.0);
  }
)";

// Colour transform shared by the packed ES2 colour program and the planar ES3
// one. Mirrors decode_to_target_rgb() in color_transform.cpp, so a frame never
// has to touch the CPU.
static const char *kColorMathGlsl = R"(
  uniform mat3 u_display_to_target;
  uniform mat3 u_rgb_to_yuv;
  uniform float u_decode_gamma;
  uniform float u_pq_scale;
  uniform int u_mode;

  const float PQ_M1 = 0.1593017578125;
  const float PQ_M2 = 78.84375;
  const float PQ_C1 = 0.8359375;
  const float PQ_C2 = 18.8515625;
  const float PQ_C3 = 18.6875;

  vec3 srgb_oetf(vec3 x) {
    vec3 lo = 12.92 * x;
    vec3 hi = 1.055 * pow(max(x, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3(0.0031308), x));
  }

  vec3 pq_oetf(vec3 l) {
    vec3 lp = pow(clamp(l, 0.0, 1.0), vec3(PQ_M1));
    return pow((PQ_C1 + PQ_C2 * lp) / (1.0 + PQ_C3 * lp), vec3(PQ_M2));
  }

  // u_mode 0: SDR native gamma decode -> display->target matrix -> sRGB encode.
  // u_mode 2: HDR gamma 2.2 -> linear -> PQ.
  // u_mode 1 (already PQ) and 3 (assumed target) pass the values through.
  vec3 target_rgb(vec3 c) {
    if (u_mode == 0) {
      vec3 lin = pow(clamp(c, 0.0, 1.0), vec3(u_decode_gamma));
      return clamp(srgb_oetf(u_display_to_target * lin), 0.0, 1.0);
    }
    if (u_mode == 2) {
      return pq_oetf(pow(clamp(c, 0.0, 1.0), vec3(2.2)) * u_pq_scale);
    }
    return c;
  }
)";

bool DmabufGlReader::build_blit_program()
{
    static const char *kFs = R"(
      #extension GL_OES_EGL_image_external : require
      precision highp float;
      varying vec2 v_uv;
      uniform samplerExternalOES u_tex;
      uniform vec2 u_uv_off;
      uniform vec2 u_uv_scale;
      void main() {
        vec2 uv = u_uv_off + (v_uv * u_uv_scale);
        gl_FragColor = texture2D(u_tex, uv);
      }
    )";

    return link_program(blit_program_, kVertexShaderEs2, kFs);
}

bool DmabufGlReader::build_color_program()
{
    const std::string fs = std::string(R"(
      #extension GL_OES_EGL_image_external : require
      precision highp float;
      varying vec2 v_uv;
      uniform samplerExternalOES u_tex;
      uniform vec2 u_uv_off;
      uniform vec2 u_uv_scale;
      uniform int u_output;
    )") + kColorMathGlsl + R"(
      void main() {
        vec2 uv = u_uv_off + (v_uv * u_uv_scale);
        vec3 c = target_rgb(texture2D(u_tex, uv).rgb);

        // u_output 0 writes YUV (Y4M); 1 writes the target RGB and leaves the
        // RGB -> YUV conversion (and chroma downsampling) to libavif.
        if (u_output == 0) {
          gl_FragColor = vec4(u_rgb_to_yuv * c + vec3(0.0, 0.5, 0.5), 1.0);
        } else {
          gl_FragColor = vec4(c, 1.0);
        }
      }
    )";

    if (!link_program(color_program_, kVertexShaderEs2, fs.c_str()))
        return false;

    color_program_.display_to_target = glGetUniformLocation(color_program_.id, "u_display_to_target");
    color_program_.rgb_to_yuv = glGetUniformLocation(color_program_.id, "u_rgb_to_yuv");
    color_program_.decode_gamma = glGetUniformLocation(color_program_.id, "u_decode_gamma");
    color_program_.pq_scale = glGetUniformLocation(color_program_.id, "u_pq_scale");
    color_program_.mode = glGetUniformLocation(color_program_.id, "u_mode");
    color_program_.output = glGetUniformLocation(color_program_.id, "u_output");
    return color_program_.valid();
}

bool DmabufGlReader::build_planar_program()
{
    // ES3 so the fragment shader can write three separate outputs, which removes
    // the CPU de-interleave from the Y4M path entirely.
    const std::string fs = std::string(R"(
      #version 300 es
      #extension GL_OES_EGL_image_external_essl3 : require
      precision highp float;
      in vec2 v_uv;
      uniform samplerExternalOES u_tex;
      uniform vec2 u_uv_off;
      uniform vec2 u_uv_scale;
      layout(location = 0) out vec4 out_y;
      layout(location = 1) out vec4 out_u;
      layout(location = 2) out vec4 out_v;
    )") + kColorMathGlsl + R"(
      void main() {
        vec2 uv = u_uv_off + (v_uv * u_uv_scale);
        vec3 c = target_rgb(texture(u_tex, uv).rgb);
        vec3 yuv = u_rgb_to_yuv * c + vec3(0.0, 0.5, 0.5);
        out_y = vec4(yuv.x, 0.0, 0.0, 1.0);
        out_u = vec4(yuv.y, 0.0, 0.0, 1.0);
        out_v = vec4(yuv.z, 0.0, 0.0, 1.0);
      }
    )";

    if (!link_program(planar_program_, kVertexShaderEs3, fs.c_str()))
        return false;

    planar_program_.display_to_target = glGetUniformLocation(planar_program_.id, "u_display_to_target");
    planar_program_.rgb_to_yuv = glGetUniformLocation(planar_program_.id, "u_rgb_to_yuv");
    planar_program_.decode_gamma = glGetUniformLocation(planar_program_.id, "u_decode_gamma");
    planar_program_.pq_scale = glGetUniformLocation(planar_program_.id, "u_pq_scale");
    planar_program_.mode = glGetUniformLocation(planar_program_.id, "u_mode");
    return planar_program_.valid();
}

void DmabufGlReader::shutdown()
{
    if (egl_dpy_ != EGL_NO_DISPLAY)
    {
        EGLSurface s = surfaceless_ ? EGL_NO_SURFACE : egl_surf_;
        if (egl_ctx_ != EGL_NO_CONTEXT)
        {
            eglMakeCurrent(egl_dpy_, s, s, egl_ctx_);
        }

        if (vbo_)
            glDeleteBuffers(1, &vbo_);
        if (color_program_.id)
            glDeleteProgram(color_program_.id);
        if (planar_program_.id)
            glDeleteProgram(planar_program_.id);
        if (blit_program_.id)
            glDeleteProgram(blit_program_.id);
        glDeleteTextures(3, plane_tex_);
        if (fbo_)
            glDeleteFramebuffers(1, &fbo_);
        if (readback_tex_)
            glDeleteTextures(1, &readback_tex_);
        if (import_tex_)
            glDeleteTextures(1, &import_tex_);

        if (egl_ctx_ != EGL_NO_CONTEXT)
        {
            eglMakeCurrent(egl_dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(egl_dpy_, egl_ctx_);
        }
        if (egl_surf_ != EGL_NO_SURFACE)
            eglDestroySurface(egl_dpy_, egl_surf_);
        eglTerminate(egl_dpy_);
    }

    if (gbm_)
        gbm_device_destroy(gbm_);

    gbm_ = nullptr;
    egl_dpy_ = EGL_NO_DISPLAY;
    egl_ctx_ = EGL_NO_CONTEXT;
    egl_surf_ = EGL_NO_SURFACE;
    import_tex_ = 0;
    readback_tex_ = 0;
    fbo_ = 0;
    blit_program_ = Program{};
    color_program_ = ColorProgram{};
    planar_program_ = PlanarProgram{};
    plane_tex_[0] = plane_tex_[1] = plane_tex_[2] = 0;
    plane_w_ = plane_h_ = 0;
    planar_readback_ok_ = false;
    vbo_ = 0;
    rb_w_ = 0;
    rb_h_ = 0;
    readback_format_ = ReadbackFormat::Rgba8;
}

} // namespace kmshot
