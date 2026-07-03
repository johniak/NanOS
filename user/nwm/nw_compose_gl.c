/*
 * nw_compose_gl.c — GL ES present backend for nwm (Plan 1, Task 10). See nw_compose_gl.h.
 *
 * The scene the CPU compositor produces (g_scene: 0x00RRGGBB pixels, one uint32 per pixel, tight
 * rows) is uploaded once per frame into a screen-sized GL texture and drawn as a single full-screen
 * quad sourced from a real GL_ARRAY_BUFFER — the exact guest→host buffer+texture upload path proven
 * by the glkms oracle (and the reason Task 10 needed the 64-bit mmap-offset fix). eglSwapBuffers +
 * KMS SetCrtc then puts the rendered buffer on the physical scanout.
 *
 * Pixel format: NanOS surfaces store 0x00RRGGBB, i.e. bytes B,G,R,0 in memory. We upload as plain
 * GL_RGBA/UNSIGNED_BYTE (so byte0→R samples the B channel) and swizzle back with `.bgr` in the
 * fragment shader, forcing alpha=1 — no BGRA extension dependency, no CPU swizzle.
 *
 * This file is compiled only in the Mesa-linked GL build (build-nwm-gl.sh); nwm.c guards every call
 * with #ifdef NWM_GL, so the in-tree CPU-only nwm.nxe never references these symbols.
 */
#include "nw_compose_gl.h"
#include "glkms_init.h"
#include "nw_gfx.h"          /* struct nw_surface { uint32_t *px; int w,h,stride; ... } */
#include <stdio.h>
#include <GLES2/gl2.h>

static struct glkms g_kms;
static int          g_ok = 0;
static int          g_sw = 0, g_sh = 0;
static GLuint       g_prog = 0, g_vbo = 0, g_tex = 0;
static GLint        g_loc_tex = -1;

/* Full-screen quad in clip space; v_uv flips Y so scene row 0 (top) maps to the top of the display
 * (GL samples uv.y=0 at the bottom otherwise). */
static const char *VS =
	"attribute vec2 a_pos;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"	v_uv = vec2(a_pos.x * 0.5 + 0.5, 0.5 - a_pos.y * 0.5);\n"
	"	gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";
static const char *FS =
	"precision mediump float;\n"
	"varying vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"void main() { gl_FragColor = vec4(texture2D(u_tex, v_uv).bgr, 1.0); }\n";

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
		printf("nw_gl: shader compile failed: %s\n", log);
		return 0;
	}
	return s;
}

int nw_gl_init(int screen_w, int screen_h)
{
	if (g_ok) return 0;
	if (glkms_open(&g_kms) != 0) { printf("nw_gl: glkms_open failed (no virgl/DRM?)\n"); return -1; }

	GLuint v = compile(GL_VERTEX_SHADER, VS);
	GLuint f = compile(GL_FRAGMENT_SHADER, FS);
	if (!v || !f) { nw_gl_shutdown(); return -1; }
	g_prog = glCreateProgram();
	glAttachShader(g_prog, v);
	glAttachShader(g_prog, f);
	glBindAttribLocation(g_prog, 0, "a_pos");
	glLinkProgram(g_prog);
	GLint lok = 0;
	glGetProgramiv(g_prog, GL_LINK_STATUS, &lok);
	if (!lok) {
		char log[512] = {0};
		glGetProgramInfoLog(g_prog, sizeof log - 1, 0, log);
		printf("nw_gl: link failed: %s\n", log);
		nw_gl_shutdown();
		return -1;
	}
	g_loc_tex = glGetUniformLocation(g_prog, "u_tex");

	/* Two triangles covering NDC [-1,1]^2. */
	static const float quad[] = {
		-1.f, -1.f,   1.f, -1.f,   -1.f, 1.f,
		-1.f,  1.f,   1.f, -1.f,    1.f, 1.f,
	};
	glGenBuffers(1, &g_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);

	glGenTextures(1, &g_tex);
	glBindTexture(GL_TEXTURE_2D, g_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, screen_w, screen_h, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, 0);
	if (glGetError() != GL_NO_ERROR) { printf("nw_gl: texture alloc failed\n"); nw_gl_shutdown(); return -1; }

	g_sw = screen_w;
	g_sh = screen_h;
	g_ok = 1;
	return 0;
}

int nw_gl_frame(const struct nw_surface *scene)
{
	if (!g_ok) return -1;
	/* The scene surface is tight (stride == width == g_sw); a full glTexSubImage2D each frame. */
	glBindTexture(GL_TEXTURE_2D, g_tex);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_sw, g_sh,
	                GL_RGBA, GL_UNSIGNED_BYTE, scene->px);

	glViewport(0, 0, (int) g_kms.mode_w, (int) g_kms.mode_h);
	glDisable(GL_BLEND);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(g_prog);
	glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *) 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, g_tex);
	glUniform1i(g_loc_tex, 0);
	glDrawArrays(GL_TRIANGLES, 0, 6);

	if (glGetError() != GL_NO_ERROR) return -1;
	return glkms_swap(&g_kms);
}

void nw_gl_shutdown(void)
{
	if (g_tex)  { glDeleteTextures(1, &g_tex); g_tex = 0; }
	if (g_vbo)  { glDeleteBuffers(1, &g_vbo);  g_vbo = 0; }
	if (g_prog) { glDeleteProgram(g_prog);     g_prog = 0; }
	if (g_ok)   glkms_close(&g_kms);
	g_ok = 0;
	g_sw = g_sh = 0;
}

int nw_gl_active(void) { return g_ok; }
