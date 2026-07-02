/* gles2info — Mesa bring-up oracle. Surfaceless EGL context + GLES2 renderer string, then an
 * off-screen FBO clear + glReadPixels round-trip. This is the milestone that drmtest could not
 * reach: it proves UNMODIFIED Mesa (gallium-virgl + EGL + GLES2, ported to NanOS) drives the
 * virtio-gpu 3D path end to end — the guest GL stack, not a hand-built command buffer.
 *
 * Serial markers consumed by scripts/smoke-virtio-gpu-gl.sh:
 *   gles2info: renderer=<str>       GL_RENDERER (expect "virgl")
 *   gles2info: version=<str>        GL_VERSION
 *   gles2info: clear-readback OK    the FBO cleared to magenta and readback confirmed it
 *
 * On plain (non-GL) QEMU there is no virgl device → no-display / no-config, exit 1 (softpipe is
 * disabled in the port, so there is no CPU fallback here — that is expected and fine). */
#include <stdio.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

/* Bind libc.ndl's stdio streams to the plain globals Mesa's C++/libdrm TUs reference
 * (nx_stream_bridge.c) — must run before any Mesa/libdrm call. */
void nx_bind_std_streams(void);

int main(void)
{
    nx_bind_std_streams();
    EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, 0);
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, 0, 0)) { printf("gles2info: no-display\n"); return 1; }
    static const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                       EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(d, cfg_attr, &cfg, 1, &n) || !n) { printf("gles2info: no-config\n"); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c))
        { printf("gles2info: no-context\n"); return 1; }
    printf("gles2info: renderer=%s\n", (const char *)glGetString(GL_RENDERER));
    printf("gles2info: version=%s\n",  (const char *)glGetString(GL_VERSION));
    /* FBO render + readback — the pixel-true oracle Mesa gives us that drmtest could not: */
    GLuint fbo, rb; glGenFramebuffers(1, &fbo); glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA4 /* ES2-core */, 64, 64);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        { printf("gles2info: fbo-incomplete\n"); return 1; }
    glClearColor(1.f, 0.f, 1.f, 1.f); glClear(GL_COLOR_BUFFER_BIT); glFinish();
    unsigned char px[4] = {0};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf(px[0] > 200 && px[1] < 50 && px[2] > 200 ? "gles2info: clear-readback OK\n"
                                                    : "gles2info: clear-readback BAD\n");
    return !(px[0] > 200);
}
