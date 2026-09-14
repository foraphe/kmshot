#include "dmabuf_gl.hpp"

#include <cstring>
#include <iostream>
#include <string>

#include <errno.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>

#include <drm_fourcc.h>

#include "drm_util.hpp"

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
    std::cerr << "Readback path: "
              << (high_precision_path_ ? "GPU FP32 (RGBA32F -> float)"
                                       : "fallback (RGBA8 -> software float)")
              << "\n";

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
    auto dmabuf_sync_ioctl = [&](uint64_t flags)
    {
        if (!dmabuf_sync)
            return;
        dma_buf_sync s{};
        s.flags = flags;
        if (::ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &s) != 0)
        {
            if (!sync_warned_ && errno != ENOTTY && errno != EINVAL && errno != ENOSYS)
            {
                sync_warned_ = true;
                std::cerr << "DMA_BUF_IOCTL_SYNC failed: " << std::strerror(errno) << "\n";
            }
        }
    };

    dmabuf_sync_ioctl(DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);

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

    EGLImageKHR image = eglCreateImageKHR_(
        egl_dpy_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs.data());
    if (image == EGL_NO_IMAGE_KHR)
    {
        std::cerr << "eglCreateImageKHR failed for fourcc=" << fourcc_to_string(fourcc) << "\n";
        dmabuf_sync_ioctl(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        return false;
    }

    if (!ensure_readback_target(out_width, out_height))
    {
        eglDestroyImageKHR_(egl_dpy_, image);
        dmabuf_sync_ioctl(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        return false;
    }

    glBindTexture(GL_TEXTURE_EXTERNAL_OES, import_tex_);
    glEGLImageTargetTexture2DOES_(GL_TEXTURE_EXTERNAL_OES, image);
    if (gl_has_error("glEGLImageTargetTexture2DOES"))
    {
        eglDestroyImageKHR_(egl_dpy_, image);
        dmabuf_sync_ioctl(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, readback_tex_, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        std::cerr << "Framebuffer incomplete\n";
        eglDestroyImageKHR_(egl_dpy_, image);
        dmabuf_sync_ioctl(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        return false;
    }

    glViewport(0, 0, static_cast<GLsizei>(out_width), static_cast<GLsizei>(out_height));
    glUseProgram(prog_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(static_cast<GLuint>(loc_pos_));
    glEnableVertexAttribArray(static_cast<GLuint>(loc_uv_));
    glVertexAttribPointer(static_cast<GLuint>(loc_pos_), 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), reinterpret_cast<void *>(0));
    glVertexAttribPointer(static_cast<GLuint>(loc_uv_), 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), reinterpret_cast<void *>(2 * sizeof(GLfloat)));
    glUniform2f(loc_uv_off_, uv_off_x, uv_off_y);
    glUniform2f(loc_uv_scale_, uv_scale_x, uv_scale_y);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, import_tex_);
    glUniform1i(loc_tex_, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(static_cast<GLuint>(loc_pos_));
    glDisableVertexAttribArray(static_cast<GLuint>(loc_uv_));

    if (gl_has_error("glDrawArrays"))
    {
        eglDestroyImageKHR_(egl_dpy_, image);
        dmabuf_sync_ioctl(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        return false;
    }

    const size_t px_count = static_cast<size_t>(out_width) * out_height;
    out_rgba32f.resize(px_count * 4u);

    if (high_precision_path_)
    {
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(
            0, 0,
            static_cast<GLsizei>(out_width),
            static_cast<GLsizei>(out_height),
            GL_RGBA,
            GL_FLOAT,
            out_rgba32f.data());
        glFinish();

        if (gl_has_error("glReadPixels(GL_FLOAT)/glFinish"))
        {
            eglDestroyImageKHR_(egl_dpy_, image);
            dmabuf_sync_ioctl(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            return false;
        }
    }
    else
    {
        std::vector<uint8_t> tmp8(px_count * 4u);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(
            0, 0,
            static_cast<GLsizei>(out_width),
            static_cast<GLsizei>(out_height),
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            tmp8.data());
        glFinish();

        if (gl_has_error("glReadPixels(GL_UNSIGNED_BYTE)/glFinish"))
        {
            eglDestroyImageKHR_(egl_dpy_, image);
            dmabuf_sync_ioctl(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            return false;
        }

        for (size_t i = 0; i < tmp8.size(); ++i)
        {
            out_rgba32f[i] = static_cast<float>(tmp8[i]) * (1.0f / 255.0f);
        }
    }

    eglDestroyImageKHR_(egl_dpy_, image);
    dmabuf_sync_ioctl(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    return true;
}

bool DmabufGlReader::ensure_readback_target(uint32_t w, uint32_t h)
{
    if (rb_w_ == w && rb_h_ == h)
        return true;

    glBindTexture(GL_TEXTURE_2D, readback_tex_);

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

        if (gl_has_error("glTexImage2D(readback RGBA32F)"))
        {
            std::cerr << "FP32 readback target unsupported at runtime; "
                         "falling back to RGBA8 path\n";
            high_precision_path_ = false;
        }
        else
        {
            GLint prev_fb = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fb);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, readback_tex_, 0);

            bool hp_ok = false;
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
            {
                glViewport(0, 0, 1, 1);
                // < 1/255 so an 8-bit target quantizes to 0 while FP32 keeps it.
                const float testv = 0.001f;
                glClearColor(testv, testv, testv, testv);
                glClear(GL_COLOR_BUFFER_BIT);

                float tmp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, tmp);
                glFinish();

                if (!gl_has_error("sanity-readback(glReadPixels)") && tmp[0] > 5e-4f)
                {
                    hp_ok = true;
                    std::cerr << "FP32 readback target verification passed (got " << tmp[0] << ")\n";
                }
            }

            glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fb));

            if (!hp_ok)
            {
                std::cerr << "Readback: RGBA32F appears not usable at runtime -> falling back\n";
                high_precision_path_ = false;
            }
        }
    }

    if (!high_precision_path_)
    {
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
    }

    rb_w_ = w;
    rb_h_ = h;
    return true;
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

bool DmabufGlReader::build_blit_program()
{
    static const char *kVs = R"(
      attribute vec2 a_pos;
      attribute vec2 a_uv;
      varying vec2 v_uv;
      void main() {
        v_uv = a_uv;
        gl_Position = vec4(a_pos, 0.0, 1.0);
      }
    )";

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

    GLuint vs = compile_shader(GL_VERTEX_SHADER, kVs);
    if (!vs)
        return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFs);
    if (!fs)
    {
        glDeleteShader(vs);
        return false;
    }

    prog_ = glCreateProgram();
    glAttachShader(prog_, vs);
    glAttachShader(prog_, fs);
    glBindAttribLocation(prog_, 0, "a_pos");
    glBindAttribLocation(prog_, 1, "a_uv");
    glLinkProgram(prog_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = {};
        glGetProgramInfoLog(prog_, sizeof(log), nullptr, log);
        std::cerr << "Program link failed: " << log << "\n";
        glDeleteProgram(prog_);
        prog_ = 0;
        return false;
    }

    loc_pos_ = glGetAttribLocation(prog_, "a_pos");
    loc_uv_ = glGetAttribLocation(prog_, "a_uv");
    loc_tex_ = glGetUniformLocation(prog_, "u_tex");
    loc_uv_off_ = glGetUniformLocation(prog_, "u_uv_off");
    loc_uv_scale_ = glGetUniformLocation(prog_, "u_uv_scale");
    return loc_pos_ >= 0 && loc_uv_ >= 0 && loc_tex_ >= 0 && loc_uv_off_ >= 0 && loc_uv_scale_ >= 0;
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
        if (prog_)
            glDeleteProgram(prog_);
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
    prog_ = 0;
    vbo_ = 0;
    rb_w_ = 0;
    rb_h_ = 0;
}

} // namespace kmshot
