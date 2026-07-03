/*
 * nw_compose_gl.c — GPU-native GL ES compositor backend for nwm (Plan 1, Task 10). See
 * nw_compose_gl.h.
 *
 * Unlike a plain present of the CPU scene, this composites on the GPU: the wallpaper and each
 * window's content are textures; glass windows get a REAL GPU two-pass separable Gaussian blur of
 * the backdrop beneath them (grabbed from the framebuffer, blurred in an FBO ping-pong) which the
 * window shader samples under a rounded-corner mask at the per-window body alpha. The desktop chrome
 * (panel, taskbar, dropdowns, Run/Auth modals) is the ONE thing kept on the CPU 2D toolkit
 * (text/rounded-rects) — nw_compose_chrome() renders it into a transparent overlay that we upload
 * and draw last; the modal desktop-dim is a GPU quad. Cursor is a small keyed quad drawn last.
 *
 * Pixel format: NanOS surfaces are 0x00RRGGBB → memory bytes B,G,R,0. We upload GL_RGBA/UNSIGNED_BYTE
 * (so sampled.rgb = (B,G,R)) and swizzle back with .bgr in the shaders. Transparent keys are matched
 * in that sampled space (black = (0,0,0) for chrome, magenta = (1,0,1) for the cursor).
 *
 * Y: our VS maps a unit quad through a pixel rect with top-down screen y. Content/wallpaper/chrome/
 * cursor textures are uploaded top-down (row 0 = top) so they sample straight. The glass backdrop is
 * the offscreen scene texture (rendered into an FBO, hence stored bottom-up); the blur passes and the
 * window shader keep that bottom-up convention and flip once at the final sample (u, 1-v).
 *
 * Glass blur (Fable fork rule): the ANGLE-Metal fork deadlocks on mid-frame FBO attachment churn, so
 * the blur ping-pong textures + FBOs are allocated and attached ONCE at init (never re-attached or
 * re-sized mid-frame); each window blurs into a fw×fh corner sub-viewport of the screen-sized targets.
 *
 * Compiled only in the Mesa-linked GL build (build-nwm-gl.sh); nwm.c guards every call with
 * #ifdef NWM_GL.
 */
#include "nw_compose_gl.h"
#include "glkms_init.h"
#include "nwm_core.h"        /* struct nw_server/nw_window + NW_BORDER/NW_TITLEBAR_H/NW_CURSOR_* */
#include "nw_compose.h"      /* nw_compose_chrome, nw_draw_cursor */
#include "nw_gfx.h"          /* struct nw_surface */
#include <stdio.h>
#include <stdlib.h>
#include <GLES2/gl2.h>

/* ---- window geometry (mirror nw_compose.c's frame_w/frame_h) ---------------------------------- */
static int frame_w(const struct nw_window *w) { return w->cw + 2 * NW_BORDER; }
static int frame_h(const struct nw_window *w) { return NW_TITLEBAR_H + w->ch + NW_BORDER; }

/* glass body alpha (out of 255, matching nw_compose.c WIN_ALPHA/DARK_ALPHA). */
#define GL_WIN_ALPHA   206
#define GL_DARK_ALPHA  214
#define GL_BORDER_RGB  0.623f, 0.698f, 0.800f   /* COL_BORDER 0x9fb2cc */

/* ---- shaders ---------------------------------------------------------------------------------- */
static const char *VS_QUAD =
	"attribute vec2 a_pos;\n"                 /* unit quad 0..1 */
	"uniform vec4 u_rect;\n"                  /* x,y,w,h in pixels */
	"uniform vec2 u_screen;\n"
	"varying vec2 v_uv;\n"
	"void main(){\n"
	"  v_uv = a_pos;\n"
	"  vec2 px = u_rect.xy + a_pos * u_rect.zw;\n"
	"  vec2 ndc = vec2(px.x / u_screen.x * 2.0 - 1.0, 1.0 - px.y / u_screen.y * 2.0);\n"
	"  gl_Position = vec4(ndc, 0.0, 1.0);\n"
	"}\n";

/* full-viewport unit quad (the blur passes): fills whatever glViewport is bound, v_uv = a_pos.
 * The blur renders into a fw×fh sub-viewport of the screen-sized blur textures (lower-left corner),
 * so it must NOT go through VS_QUAD's pixel-rect mapping. */
static const char *VS_FULL =
	"attribute vec2 a_pos;\n"
	"varying vec2 v_uv;\n"
	"void main(){ v_uv = a_pos; gl_Position = vec4(a_pos * 2.0 - 1.0, 0.0, 1.0); }\n";

/* opaque textured quad (wallpaper) — .bgr swizzle, alpha 1 */
static const char *FS_TEX =
	"precision mediump float;\n"
	"varying vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"void main(){ gl_FragColor = vec4(texture2D(u_tex, v_uv).bgr, 1.0); }\n";

/* final scene blit: sample the already-correct-RGB offscreen scene texture (no swizzle), flipping Y
 * because it was rendered into an FBO (bottom-up) with our top-down VS. */
static const char *FS_BLIT =
	"precision mediump float;\n"
	"varying vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"void main(){ gl_FragColor = vec4(texture2D(u_tex, vec2(v_uv.x, 1.0 - v_uv.y)).rgb, 1.0); }\n";

/* keyed textured quad (chrome overlay / cursor) — discard pixels matching u_key (sampled space) */
static const char *FS_KEYED =
	"precision mediump float;\n"
	"varying vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec3 u_key;\n"
	"void main(){\n"
	"  vec4 c = texture2D(u_tex, v_uv);\n"
	"  if (all(lessThan(abs(c.rgb - u_key), vec3(0.02)))) discard;\n"
	"  gl_FragColor = vec4(c.bgr, 1.0);\n"
	"}\n";

/* solid fill (modal desktop-dim) */
static const char *FS_SOLID =
	"precision mediump float;\n"
	"uniform vec4 u_color;\n"
	"void main(){ gl_FragColor = u_color; }\n";

/* one separable Gaussian pass, sampling a sub-region (u_uv0 + v_uv*u_uvsize) of the source texture so
 * the FIRST pass reads the window's region straight out of the offscreen scene texture (no readback,
 * no copy). u_dir = the per-tap step in source-uv space. Rendered full-viewport via VS_FULL into a
 * fw×fh corner of the pre-attached blur FBOs. */
static const char *FS_BLUR =
	"precision mediump float;\n"
	"varying vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_dir;\n"
	"uniform vec2 u_uv0;\n"
	"uniform vec2 u_uvsize;\n"
	"void main(){\n"
	"  vec2 b = u_uv0 + v_uv * u_uvsize;\n"
	"  vec4 c = texture2D(u_tex, b) * 0.227027;\n"
	"  c += texture2D(u_tex, b + u_dir * 1.0) * 0.1945946;\n"
	"  c += texture2D(u_tex, b - u_dir * 1.0) * 0.1945946;\n"
	"  c += texture2D(u_tex, b + u_dir * 2.0) * 0.1216216;\n"
	"  c += texture2D(u_tex, b - u_dir * 2.0) * 0.1216216;\n"
	"  c += texture2D(u_tex, b + u_dir * 3.0) * 0.054054;\n"
	"  c += texture2D(u_tex, b - u_dir * 3.0) * 0.054054;\n"
	"  c += texture2D(u_tex, b + u_dir * 4.0) * 0.016216;\n"
	"  c += texture2D(u_tex, b - u_dir * 4.0) * 0.016216;\n"
	"  gl_FragColor = c;\n"
	"}\n";

/* window: content over a rounded-corner mask + hairline border. Glass windows show a REAL GPU
 * two-pass Gaussian blur of the scene beneath them: blur_backdrop() renders the blurred backdrop into
 * the lower-left fw×fh corner of the screen-sized g_blurB (sampled here via u_backdrop + u_bd_scale),
 * and the body is content mixed over that blur at u_alpha. u_glass=0 → plain opaque content (the
 * NWM_NO_GLASS fallback / non-glass windows). Backdrop is stored bottom-up (rendered into an FBO) so
 * the sample flips y: (v_uv.x, 1-v_uv.y)*u_bd_scale. mask is the coverage alpha (1 inside, AA at the
 * corners) blended over the already-composited scene. */
static const char *FS_WIN =
	"precision mediump float;\n"
	"varying vec2 v_uv;\n"
	"uniform sampler2D u_content;\n"
	"uniform sampler2D u_backdrop;\n"   /* g_blurB: blurred scene under the window (bottom-up, corner) */
	"uniform vec2 u_bd_scale;\n"        /* (fw/screen_w, fh/screen_h): the window's corner of g_blurB */
	"uniform float u_glass;\n"          /* 1 = frosted glass, 0 = opaque */
	"uniform float u_alpha;\n"          /* body alpha for glass (content over the blurred backdrop) */
	"uniform vec2 u_size_px;\n"
	"uniform float u_radius_px;\n"
	"uniform vec3 u_border;\n"
	"void main(){\n"
	"  vec2 p = v_uv * u_size_px;\n"
	"  vec2 k = min(p, u_size_px - p);\n"
	"  float r = u_radius_px;\n"
	"  vec2 cq = max(vec2(r) - k, vec2(0.0));\n"
	"  float d = length(cq);\n"
	"  float mask = 1.0 - smoothstep(r - 1.0, r + 1.0, d);\n"
	"  vec3 content = texture2D(u_content, v_uv).bgr;\n"
	"  vec2 bd_uv = vec2(v_uv.x, 1.0 - v_uv.y) * u_bd_scale;\n"
	"  vec3 bd = texture2D(u_backdrop, bd_uv).rgb;\n"          /* scene tex is already RGB */
	"  vec3 glass = mix(bd, content, u_alpha);\n"              /* content over the blurred backdrop */
	"  vec3 col = mix(content, glass, u_glass);\n"
	"  float edge = smoothstep(r - 2.5, r - 1.0, d) * mask * 0.6;\n"  /* hairline border near the corner */
	"  col = mix(col, u_border, edge);\n"
	"  gl_FragColor = vec4(col, mask);\n"
	"}\n";

/* ---- program handles + cached uniform locations ----------------------------------------------- */
struct prog { GLuint id; GLint rect, screen; };
static struct prog p_tex, p_blit, p_keyed, p_solid, p_blur, p_win;
static GLint u_tex_tex, u_blit_tex, u_key_tex, u_key_key, u_solid_color;
static GLint u_blur_tex, u_blur_dir, u_blur_uv0, u_blur_uvsize;
static GLint u_win_content, u_win_backdrop, u_win_bd_scale, u_win_glass, u_win_alpha,
             u_win_size, u_win_radius, u_win_border;

static struct glkms g_kms;
static int  g_ok = 0;
static int  g_no_glass = 0;                    /* NWM_NO_GLASS or a failed blur-chain init: opaque only */
static int  g_sw = 0, g_sh = 0;
static int  g_radius = NW_RADIUS;
static GLuint g_vbo = 0;
static GLuint g_wall_tex = 0, g_chrome_tex = 0, g_cursor_tex = 0;
static GLuint g_scene_tex = 0, g_scene_fbo = 0;/* offscreen scene: compose here, blit to fb0 at the end */

/* Blur chain — FORK RULE (kosmickrisp ANGLE-Metal): allocate ONCE at init, attach ONCE; never
 * glTexImage2D or glFramebufferTexture2D on these mid-frame. Mid-frame FBO attachment churn deadlocks
 * the host (every attachment change is a Metal render-pass boundary the borrow patch synchronizes on).
 * Both are screen-sized; each window blurs into the lower-left fw×fh corner via a sub-viewport. */
static GLuint g_blurA = 0, g_blurB = 0;        /* screen-sized ping-pong textures */
static GLuint g_fboA = 0, g_fboB = 0;          /* g_fboA<-g_blurA, g_fboB<-g_blurB, attached at init */

/* per-window content textures + their allocated size (recreated on resize) */
static GLuint   g_win_tex[NW_MAX_WINDOWS];
static int      g_win_tw[NW_MAX_WINDOWS], g_win_th[NW_MAX_WINDOWS];
static unsigned g_win_gen[NW_MAX_WINDOWS];     /* last frame_gen uploaded per slot; 0 = never */
/* the CPU chrome overlay surface (screen-sized) */
static uint32_t *g_chrome_px = 0;
static struct nw_surface g_chrome_surf;
static int g_chrome_ready = 0;                  /* the chrome texture holds at least one real render */
static int g_wall_dirty  = 1;                   /* the wallpaper texture must be (re)uploaded */

static GLuint compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, 0);
	glCompileShader(s);
	GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) { char log[512] = {0}; glGetShaderInfoLog(s, sizeof log - 1, 0, log);
		printf("nw_gl: shader compile failed: %s\n", log); return 0; }
	return s;
}

static int link_prog_vs(struct prog *p, const char *vs, const char *fs)
{
	GLuint v = compile(GL_VERTEX_SHADER, vs);
	GLuint f = compile(GL_FRAGMENT_SHADER, fs);
	if (!v || !f) return -1;
	p->id = glCreateProgram();
	glAttachShader(p->id, v); glAttachShader(p->id, f);
	glBindAttribLocation(p->id, 0, "a_pos");
	glLinkProgram(p->id);
	GLint ok = 0; glGetProgramiv(p->id, GL_LINK_STATUS, &ok);
	if (!ok) { char log[512] = {0}; glGetProgramInfoLog(p->id, sizeof log - 1, 0, log);
		printf("nw_gl: link failed: %s\n", log); return -1; }
	p->rect   = glGetUniformLocation(p->id, "u_rect");
	p->screen = glGetUniformLocation(p->id, "u_screen");
	return 0;
}

static int link_prog(struct prog *p, const char *fs) { return link_prog_vs(p, VS_QUAD, fs); }

static GLuint make_tex(int w, int h, const void *px)
{
	GLuint t; glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	return t;
}

/* draw the unit quad through program p over the pixel rect (x,y,w,h) */
static void quad(struct prog *p, float x, float y, float w, float h)
{
	glUseProgram(p->id);
	glUniform4f(p->rect, x, y, w, h);
	glUniform2f(p->screen, (float) g_sw, (float) g_sh);
	glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *) 0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* draw the raw unit quad filling the current glViewport (blur passes; program already bound) */
static void draw_full_quad(void)
{
	glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *) 0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

int nw_gl_init(int screen_w, int screen_h)
{
	if (g_ok) return 0;
	if (glkms_open(&g_kms) != 0) { printf("nw_gl: glkms_open failed (no virgl/DRM?)\n"); return -1; }

	if (link_prog(&p_tex, FS_TEX) || link_prog(&p_blit, FS_BLIT) || link_prog(&p_keyed, FS_KEYED) ||
	    link_prog(&p_solid, FS_SOLID) || link_prog_vs(&p_blur, VS_FULL, FS_BLUR) ||
	    link_prog(&p_win, FS_WIN)) {
		nw_gl_shutdown(); return -1; }
	u_tex_tex     = glGetUniformLocation(p_tex.id, "u_tex");
	u_blit_tex    = glGetUniformLocation(p_blit.id, "u_tex");
	u_key_tex     = glGetUniformLocation(p_keyed.id, "u_tex");
	u_key_key     = glGetUniformLocation(p_keyed.id, "u_key");
	u_solid_color = glGetUniformLocation(p_solid.id, "u_color");
	u_blur_tex    = glGetUniformLocation(p_blur.id, "u_tex");
	u_blur_dir    = glGetUniformLocation(p_blur.id, "u_dir");
	u_blur_uv0    = glGetUniformLocation(p_blur.id, "u_uv0");
	u_blur_uvsize = glGetUniformLocation(p_blur.id, "u_uvsize");
	u_win_content  = glGetUniformLocation(p_win.id, "u_content");
	u_win_backdrop = glGetUniformLocation(p_win.id, "u_backdrop");
	u_win_bd_scale = glGetUniformLocation(p_win.id, "u_bd_scale");
	u_win_glass    = glGetUniformLocation(p_win.id, "u_glass");
	u_win_alpha    = glGetUniformLocation(p_win.id, "u_alpha");
	u_win_size     = glGetUniformLocation(p_win.id, "u_size_px");
	u_win_radius   = glGetUniformLocation(p_win.id, "u_radius_px");
	u_win_border   = glGetUniformLocation(p_win.id, "u_border");

	static const float uquad[] = { 0.f, 0.f,  1.f, 0.f,  0.f, 1.f,  1.f, 1.f };  /* triangle strip */
	glGenBuffers(1, &g_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof uquad, uquad, GL_STATIC_DRAW);

	g_chrome_px = (uint32_t *) malloc((size_t) screen_w * screen_h * 4);
	if (!g_chrome_px) { printf("nw_gl: no memory for chrome overlay\n"); nw_gl_shutdown(); return -1; }
	g_chrome_surf.px = g_chrome_px; g_chrome_surf.w = screen_w; g_chrome_surf.h = screen_h;
	g_chrome_surf.stride = screen_w; nw_surface_noclip(&g_chrome_surf);
	g_chrome_tex = make_tex(screen_w, screen_h, 0);

	/* offscreen scene target: everything composites here, then blits to fb0 (avoids default-fb reads) */
	g_scene_tex = make_tex(screen_w, screen_h, 0);
	glGenFramebuffers(1, &g_scene_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_scene_tex, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	/* Blur chain: allocate + attach ONCE (the fork rule — see the g_blurA/g_blurB comment). If a knob
	 * disables glass, or either FBO is incomplete, run opaque (never a hang, never a hard init fail). */
	g_no_glass = getenv("NWM_NO_GLASS") ? 1 : 0;
	if (!g_no_glass) {
		g_blurA = make_tex(screen_w, screen_h, 0);
		g_blurB = make_tex(screen_w, screen_h, 0);
		glGenFramebuffers(1, &g_fboA);
		glBindFramebuffer(GL_FRAMEBUFFER, g_fboA);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_blurA, 0);
		int okA = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
		glGenFramebuffers(1, &g_fboB);
		glBindFramebuffer(GL_FRAMEBUFFER, g_fboB);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_blurB, 0);
		int okB = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		if (!okA || !okB) { printf("nw_gl: blur FBO incomplete — opaque glass fallback\n"); g_no_glass = 1; }
	}

	if (glGetError() != GL_NO_ERROR) { printf("nw_gl: init GL error\n"); nw_gl_shutdown(); return -1; }
	g_sw = screen_w; g_sh = screen_h; g_ok = 1;
	printf("nw_gl: GPU compositor init ok (%dx%d, glass=%s)\n", screen_w, screen_h, g_no_glass ? "off" : "on");
	return 0;
}

/* ensure a texture handle is (re)allocated to at least w×h; *tw/*th track the current size.
 * Returns 1 if it (re)allocated (caller must re-upload — the new storage is uninitialised). */
static int ensure_tex(GLuint *t, int *tw, int *th, int w, int h)
{
	if (*t && *tw == w && *th == h) return 0;
	if (*t) glDeleteTextures(1, t);
	*t = make_tex(w, h, 0);
	*tw = w; *th = h;
	return 1;
}

/* Serial trace of each GL call in blur_backdrop — the oracle while the fork-hang is being chased
 * (Fable Phase 0). NWM_GL_TRACE=1 turns it on; off by default (silent once glass is proven). */
static int g_blur_trace = -1;
#define BLUR_MARK(tag) do { if (g_blur_trace) { printf("nw_gl blur: " tag "\n"); fflush(stdout); } } while (0)

/* GPU two-pass separable Gaussian of the scene region under the window into g_blurB's lower-left
 * fw×fh corner. FORK RULE: no glGenTextures / glFramebufferTexture2D / glTexImage2D here — g_fboA and
 * g_fboB are pre-attached at init; we only switch framebuffer binding + viewport. Pass 1 (H) reads
 * the window's sub-region of g_scene_tex into g_fboA's corner; pass 2 (V) reads that corner of
 * g_blurA into g_fboB. FS_WIN then samples g_blurB at (v_uv.x, 1-v_uv.y)*u_bd_scale. */
static void blur_backdrop(int wx, int wy, int fw, int fh)
{
	if (g_blur_trace < 0) g_blur_trace = getenv("NWM_GL_TRACE") ? 1 : 0;
	const float STEP = 2.5f;                  /* per-tap spread (window px) → a soft, glass-like blur */
	const float uw = (float) fw / g_sw, uh = (float) fh / g_sh;   /* the corner's uv extent */
	BLUR_MARK("enter");
	glDisable(GL_BLEND);                                            BLUR_MARK("blend-off");
	glUseProgram(p_blur.id);                                        BLUR_MARK("use-prog");
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(u_blur_tex, 0);
	glUniform2f(u_blur_uvsize, uw, uh);
	glViewport(0, 0, fw, fh);                 /* every pass draws into the lower-left corner */

	/* pass 1 (H): window sub-region of g_scene_tex (y-flipped: scene tex is bottom-up in the FBO) */
	glBindFramebuffer(GL_FRAMEBUFFER, g_fboA);                      BLUR_MARK("bind-fboA");
	glBindTexture(GL_TEXTURE_2D, g_scene_tex);                      BLUR_MARK("bind-scene-tex");
	glUniform2f(u_blur_uv0, (float) wx / g_sw, 1.0f - (float) (wy + fh) / g_sh);
	glUniform2f(u_blur_dir, STEP / g_sw, 0.0f);
	draw_full_quad();                                              BLUR_MARK("draw-H1");

	/* pass 2 (V): the corner of g_blurA → g_fboB */
	glBindFramebuffer(GL_FRAMEBUFFER, g_fboB);                      BLUR_MARK("bind-fboB");
	glBindTexture(GL_TEXTURE_2D, g_blurA);                          BLUR_MARK("bind-blurA");
	glUniform2f(u_blur_uv0, 0.0f, 0.0f);
	glUniform2f(u_blur_dir, 0.0f, STEP / g_sh);
	draw_full_quad();                                              BLUR_MARK("draw-V1");

	glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);   /* back to the offscreen scene target */
	glViewport(0, 0, g_sw, g_sh);
	glEnable(GL_BLEND);                      /* restore for the window/chrome draws */
	BLUR_MARK("done");
}

int nw_gl_frame(const struct nw_server *s, const struct nw_surface *wall, int scene_dirty,
                int interacting)
{
	if (!g_ok) return -1;

	/* Recompose the desktop into the CURSOR-FREE offscreen scene only when something other than the
	 * pointer changed. A bare cursor move skips this whole (TCG-expensive) Mesa composite + blur +
	 * upload path and just re-presents g_scene_tex with the cursor at its new spot — so the pointer
	 * stays smooth even though a GPU-swapped buffer has no cheap partial update. */
	if (scene_dirty) {
		/* Wallpaper: (re)upload ONLY when it first appears or actually changes (settings reload flags
		 * g_wall_dirty via nw_gl_wallpaper_changed), NEVER per frame — a per-frame 4 MB re-upload was
		 * both a drag-time upload-storm cost AND the corruptor of the top "white band" (each re-upload
		 * raced the host's read of the previous frame's transfer). glFinish after the single upload so
		 * the transfer completes before the composite samples it — that missing fence is what the old
		 * per-frame upload accidentally papered over. glTexImage2D (full realloc), not glTexSubImage2D:
		 * the partial-update path garbles the top rows on this ANGLE-Metal fork. */
		if (wall && (!g_wall_tex || g_wall_dirty)) {
			if (!g_wall_tex) g_wall_tex = make_tex(wall->w, wall->h, wall->px);
			else { glBindTexture(GL_TEXTURE_2D, g_wall_tex);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, wall->w, wall->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, wall->px); }
			glFinish();
			g_wall_dirty = 0;
		}

		glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);   /* compose into the offscreen scene */
		glViewport(0, 0, g_sw, g_sh);
		glDisable(GL_BLEND);
		glClearColor(0.f, 0.f, 0.f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT);

		/* wallpaper, opaque */
		if (g_wall_tex) {
			glUseProgram(p_tex.id); glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, g_wall_tex); glUniform1i(u_tex_tex, 0);
			quad(&p_tex, 0, 0, (float) g_sw, (float) g_sh);
		}

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		/* windows back-to-front (same z-order walk as nw_compose_scene) */
		for (int z = 0; z < s->zn; z++) {
			int idx = s->zorder[z];
			const struct nw_window *w = &s->win[idx];
			if (!w->used || w->minimized || !w->frame) continue;
			int fw = frame_w(w), fh = frame_h(w);
			int dark = (w->title[0] == '\x01');
			float alpha = (dark ? GL_DARK_ALPHA : GL_WIN_ALPHA) / 255.0f;

			/* Upload this window's cached frame render as its content texture. Two forces shape this:
			 * (1) a SINGLE upload of a window texture lands garbled on this fork (a top-band transfer
			 *     glitch); only repeated re-uploads settle to a clean image — so when NOT interacting we
			 *     re-upload every frame, exactly as the pre-GPU path did (cheap: the desktop is idle
			 *     between events, and this masks the glitch the way the wallpaper's glTexImage2D+glFinish
			 *     can't afford per window);
			 * (2) during a drag/resize NO window is re-rendered (a move changes only x/y), so we skip the
			 *     upload entirely — the resident texture is reused and a drag frame uploads ZERO window
			 *     bytes. A window whose content genuinely changed mid-interaction (frame_gen bumped) is
			 *     still refreshed. */
			glActiveTexture(GL_TEXTURE0);
			ensure_tex(&g_win_tex[idx], &g_win_tw[idx], &g_win_th[idx], fw, fh);
			glBindTexture(GL_TEXTURE_2D, g_win_tex[idx]);
			if (!interacting || g_win_gen[idx] != w->frame_gen) {
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, w->frame);
				glFinish();                       /* fence: the fork glitches one un-fenced texture
				                                   * transfer per frame (the wallpaper needs this too) */
				g_win_gen[idx] = w->frame_gen;
			}

			int glass = (w->glass && !g_no_glass);
			if (glass)                               /* blur the scene beneath into g_blurB's fw×fh corner */
				blur_backdrop(w->x, w->y, fw, fh);   /* leaves us back on g_scene_fbo, full viewport, blend on */

			glUseProgram(p_win.id);
			glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, g_blurB);  /* backdrop (unit 1) */
			glUniform1i(u_win_backdrop, 1);
			glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_win_tex[idx]);
			glUniform1i(u_win_content, 0);
			glUniform2f(u_win_bd_scale, (float) fw / g_sw, (float) fh / g_sh);
			glUniform1f(u_win_glass, glass ? 1.0f : 0.0f);
			glUniform1f(u_win_alpha, alpha);
			glUniform2f(u_win_size, (float) fw, (float) fh);
			glUniform1f(u_win_radius, (float) g_radius);
			glUniform3f(u_win_border, GL_BORDER_RGB);
			quad(&p_win, (float) w->x, (float) w->y, (float) fw, (float) fh);
		}

		/* modal desktop-dim (auth): a full-screen translucent black quad over the windows */
		if (s->auth_open) {
			glUseProgram(p_solid.id);
			glUniform4f(u_solid_color, 0.f, 0.f, 0.f, 130.0f / 255.0f);
			quad(&p_solid, 0, 0, (float) g_sw, (float) g_sh);
		}

		/* chrome overlay: CPU-rendered panel/taskbar/dropdown/modals, keyed on BLACK (the transparent
		 * fill). Black, NOT magenta: a magenta key was tried (to stop black UI pixels keying out) but it
		 * flooded the window bodies with magenta — the panel/taskbar are drawn with soft AA edges that
		 * blend toward the key colour, and those near-key blends survive a magenta key while a black key
		 * (matching the actual dark UI) drops them. Re-render + re-upload the
		 * full-screen overlay ONLY when NOT interacting: during a drag/resize the panel & taskbar can't
		 * change, so we reuse the resident chrome texture (zero CPU render, zero 4 MB upload). Always
		 * upload once so the first frame — or a drag that begins before any idle frame — has real chrome. */
		if (!interacting || !g_chrome_ready) {
			nw_compose_chrome(s, &g_chrome_surf);
			glBindTexture(GL_TEXTURE_2D, g_chrome_tex);
			/* glTexImage2D + glFinish, same fenced recipe as the wallpaper/windows: an un-fenced
			 * glTexSubImage2D can land the fork's one-per-frame transfer glitch on the chrome's top
			 * band, and because a drag FREEZES the chrome (no re-upload), that garbled panel would then
			 * persist for the whole drag. */
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_sw, g_sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_chrome_px);
			glFinish();
			g_chrome_ready = 1;
		}
		glUseProgram(p_keyed.id);
		glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_chrome_tex);
		glUniform1i(u_key_tex, 0);
		glUniform3f(u_key_key, 0.f, 0.f, 0.f);   /* key out black (untouched overlay + shadows) */
		quad(&p_keyed, 0, 0, (float) g_sw, (float) g_sh);
	}

	/* present: blit the (cursor-free) scene to the default framebuffer, then draw the cursor on top */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, (int) g_kms.mode_w, (int) g_kms.mode_h);
	glDisable(GL_BLEND);
	glUseProgram(p_blit.id);
	glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_scene_tex);
	glUniform1i(u_blit_tex, 0);
	quad(&p_blit, 0, 0, (float) g_sw, (float) g_sh);

	/* cursor: a small keyed quad (magenta key discarded) — drawn per present at the live position */
	if (g_cursor_tex) {
		glUseProgram(p_keyed.id);
		glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_cursor_tex);
		glUniform1i(u_key_tex, 0);
		glUniform3f(u_key_key, 1.f, 0.f, 1.f);   /* magenta key (sampled space) */
		quad(&p_keyed, (float) s->cursor_x, (float) s->cursor_y, (float) NW_CURSOR_W, (float) NW_CURSOR_H);
	}

	if (glGetError() != GL_NO_ERROR) return -1;
	return glkms_swap(&g_kms);
}

/* Build the cursor texture once from the CPU arrow bitmap (magenta-keyed background). Called by
 * nwm.c after init. Safe no-op if the backend isn't live. */
void nw_gl_build_cursor(void)
{
	if (!g_ok || g_cursor_tex) return;
	int cw = NW_CURSOR_W, ch = NW_CURSOR_H;
	uint32_t *px = (uint32_t *) malloc((size_t) cw * ch * 4);
	if (!px) return;
	for (int i = 0; i < cw * ch; i++) px[i] = 0xff00ff;   /* magenta transparent key */
	struct nw_surface cs = { px, cw, ch, cw, 0, 0, 0, 0 };
	nw_draw_cursor(&cs, 0, 0);
	g_cursor_tex = make_tex(cw, ch, px);
	free(px);
}

void nw_gl_set_radius(int radius) { if (radius >= 0 && radius <= 20) g_radius = radius; }

void nw_gl_wallpaper_changed(void) { g_wall_dirty = 1; }

void nw_gl_shutdown(void)
{
	for (int i = 0; i < NW_MAX_WINDOWS; i++)
		if (g_win_tex[i]) { glDeleteTextures(1, &g_win_tex[i]); g_win_tex[i] = 0; g_win_tw[i] = g_win_th[i] = 0;
			g_win_gen[i] = 0; }
	g_chrome_ready = 0; g_wall_dirty = 1;
	if (g_blurA)  { glDeleteTextures(1, &g_blurA);  g_blurA = 0; }
	if (g_blurB)  { glDeleteTextures(1, &g_blurB);  g_blurB = 0; }
	if (g_fboA)   { glDeleteFramebuffers(1, &g_fboA); g_fboA = 0; }
	if (g_fboB)   { glDeleteFramebuffers(1, &g_fboB); g_fboB = 0; }
	if (g_wall_tex)   { glDeleteTextures(1, &g_wall_tex);   g_wall_tex = 0; }
	if (g_chrome_tex) { glDeleteTextures(1, &g_chrome_tex); g_chrome_tex = 0; }
	if (g_cursor_tex) { glDeleteTextures(1, &g_cursor_tex); g_cursor_tex = 0; }
	if (g_scene_tex) { glDeleteTextures(1, &g_scene_tex); g_scene_tex = 0; }
	if (g_scene_fbo) { glDeleteFramebuffers(1, &g_scene_fbo); g_scene_fbo = 0; }
	if (g_vbo) { glDeleteBuffers(1, &g_vbo); g_vbo = 0; }
	if (g_chrome_px) { free(g_chrome_px); g_chrome_px = 0; }
	if (g_ok) glkms_close(&g_kms);
	g_ok = 0; g_sw = g_sh = 0;
}

int nw_gl_active(void) { return g_ok; }
