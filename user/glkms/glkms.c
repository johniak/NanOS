/*
 * glkms.nxe — the GBM+KMS present oracle (Plan 1, Task 9). GL-renders a two-axis colour gradient
 * into a GBM scanout surface and presents it to the physical display via eglSwapBuffers → KMS
 * (AddFB + SetCrtc). This is the canonical Linux GPU-present path and it is the exact code
 * (glkms_init.c) that nwm's GL compositor (Task 10) reuses verbatim — and the path that carries
 * unchanged to i915 on the Dell.
 *
 * Why a gradient (not a flat clear): the gradient is a strong distinct-colours oracle for the
 * screendump smoke gate (thousands of colours, unlike any console/greeter surface), and it proves
 * the fragment shader actually ran per-pixel — not just a clear loadAction.
 *
 * Serial markers consumed by scripts/smoke-virtio-gpu-gl.sh:
 *   glkms: mode WxH ...        (from glkms_init)
 *   glkms: flip OK             first successful present
 * On plain (non-GL) QEMU: glkms_open fails at the EGL/GBM stage (no virgl driver) → nonzero exit,
 * the display is untouched (no CPU fallback in this oracle — nwm provides that).
 */
#include "glkms_init.h"
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <GLES2/gl2.h>

/* Tee every marker to the stick (same idiom as i915test/gles2info): the Dell has no serial
 * console, so `make i915-log` pulling /nanos/logs/gltest.txt is the hands-off verdict channel.
 * Only THIS TU is teed — glkms_init.c is shared verbatim with nwm's GL backend, so its lines
 * stay console-only and main re-prints the mode after a successful open. */
#define TEE_PATH "/disks/main/nanos/logs/gltest.txt"
static FILE *g_tee;
static int tee_printf(const char *fmt, ...)
{
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = vprintf(fmt, ap);
	va_end(ap);
	if (g_tee) {
		va_start(ap, fmt);
		vfprintf(g_tee, fmt, ap);
		va_end(ap);
		fflush(g_tee);
	}
	return r;
}
#define printf tee_printf

/* nx_stream_bridge.c binds libc.ndl's stdio streams to the plain globals Mesa/libdrm reference. */
void nx_bind_std_streams(void);

/* ES3 fullscreen triangle, vertices sourced from a real vertex buffer (GL_ARRAY_BUFFER).
 * This exercises the canonical guest->host buffer-upload path that nwm's GL compositor
 * (Task 10) relies on. */
static const char *VS =
	"#version 300 es\n"
	"in vec2 p;\n"
	"out vec2 v_uv;\n"
	"void main(){ v_uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
static const char *FS =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec2 v_uv;\n"
	"out vec4 o_col;\n"
	"void main(){ o_col = vec4(v_uv.x, v_uv.y, 0.5, 1.0); }\n";

static GLuint compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, 0);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512] = {0};
		glGetShaderInfoLog(s, sizeof log - 1, 0, log);
		printf("glkms: shader compile failed: %s\n", log);
		return 0;
	}
	return s;
}

static int atoi_simple(const char *s)
{
	int v = 0;
	while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
	return v;
}

int main(int argc, char **argv)
{
	nx_bind_std_streams();

	/* Optional argv[1] = seconds to hold the final gradient on screen (default 3). Longer holds
	 * give a stable window for a macOS screencapture of the cocoa window — the only valid visual
	 * oracle on the gl=es path (QEMU monitor screendump cannot read back the ANGLE/Metal scanout). */
	int hold = (argc > 1) ? atoi_simple(argv[1]) : 3;
	if (hold <= 0) hold = 3;

	g_tee = fopen(TEE_PATH, "a");
	printf("glkms: ===== run =====%s\n", g_tee ? "" : " [tee unavailable -- console only]");

	struct glkms g;
	if (glkms_open(&g) != 0) { printf("glkms: open failed\n"); return 1; }
	printf("glkms: opened %dx%d\n", g.mode_w, g.mode_h);

	GLuint v = compile(GL_VERTEX_SHADER, VS);
	GLuint f = compile(GL_FRAGMENT_SHADER, FS);
	if (!v || !f) return 1;
	GLuint prog = glCreateProgram();
	glAttachShader(prog, v);
	glAttachShader(prog, f);
	glBindAttribLocation(prog, 0, "p");
	glLinkProgram(prog);
	GLint lok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &lok);
	if (!lok) {
		char log[512] = {0};
		glGetProgramInfoLog(prog, sizeof log - 1, 0, log);
		printf("glkms: link failed: %s\n", log);
		return 1;
	}
	glUseProgram(prog);

	/* One fullscreen triangle (clip space) sourced from a real vertex buffer. */
	static const float tri[] = { -1.f, -1.f,  3.f, -1.f,  -1.f, 3.f };
	GLuint vbo = 0;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);
	glViewport(0, 0, g.mode_w, g.mode_h);

	int flipped = 0;
	for (int i = 0; i < 60; i++) {
		glClearColor(0.f, 0.f, 0.f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		if (glGetError() != GL_NO_ERROR) { printf("glkms: GL error during draw\n"); return 1; }
		if (glkms_swap(&g) != 0) { printf("glkms: swap failed at frame %d\n", i); return 1; }
		if (!flipped) { printf("glkms: flip OK\n"); flipped = 1; }
	}

	sleep(hold);   /* hold the gradient on scanout (screencapture window; smoke default 3s) */
	glkms_close(&g);
	return 0;
}
