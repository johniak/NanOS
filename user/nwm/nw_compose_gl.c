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
#include "nwproto.h"         /* NW_STYLE_GLASS_CLIENT / NW_STYLE_DARK */
#include "nw_compose.h"      /* nw_compose_chrome, nw_draw_cursor */
#include "nw_gfx.h"          /* struct nw_surface */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>          /* read/close (the fail-after fault-injection knob) */
#include <GLES2/gl2.h>

/* ---- window geometry (mirror nw_compose.c's frame_w/frame_h) ---------------------------------- */
static int frame_w(const struct nw_window *w) { return w->cw + 2 * NW_BORDER; }
static int frame_h(const struct nw_window *w) { return NW_TITLEBAR_H + w->ch + NW_BORDER; }

/* px margin so refraction (Task 5's lens ring) can sample content just outside the window frame. */
#define NW_GLASS_PAD 24

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

/* window: the liquid-glass slab material (Task 5). A rounded-box SDF gives distance `d` to the
 * window edge; `rim` is 1 at the very edge and 0 on the flat body. The flat body shows the BLURRED
 * backdrop (frost); the rim mixes toward a chromatically-fringed SHARP sample displaced OUTWARD
 * along the SDF gradient (refraction), so the rim shows a compressed lensed sliver of whatever lies
 * just outside the window. Caption spheres repeat the same lens trick at small scale, tinted. Ink
 * (CPU-rendered band glow / app content) composites over the finished glass. u_glass=0 → plain
 * opaque content (NWM_NO_GLASS fallback / non-glass windows).
 *
 * Both u_backdrop (g_blurB) and u_sharp (g_grab) are screen-sized textures whose only valid data is
 * the lower-left g_gw×g_gh corner holding the current window's padded grab rect (see blur_backdrop());
 * they are populated by rendering/copying from g_scene_tex, which is real composited RGB (not a
 * packed 0xAARRGGBB surface), so backdrop() samples .rgb — NOT the .bgr swizzle used for u_content
 * (which IS a raw surface upload). backdrop() maps a screen-px point through u_grab to a 0..1
 * fraction of the padded rect (top-down), flips y once (the grab textures are bottom-up, matching
 * g_scene_tex's FBO storage), then rescales by u_bd_scale into the screen-sized texture's corner —
 * the one adaptation vs. a literal transcription, needed because these aren't gw×gh-sized textures. */
static const char *FS_WIN =
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"precision highp float;\n"
	"#else\n"
	"precision mediump float;\n"
	"#endif\n"
	"varying vec2 v_uv;\n"
	"uniform highp vec4 u_rect;\n"       /* window rect, screen px (program-scope, shared with the VS) */
	"uniform sampler2D u_content;\n"     /* unit 0: CPU frame render; band=key black, client=app pixels */
	"uniform sampler2D u_backdrop;\n"    /* unit 1: BLURRED padded backdrop grab (g_blurB corner) */
	"uniform sampler2D u_sharp;\n"       /* unit 2: SHARP padded backdrop grab (g_grab corner) */
	"uniform vec4  u_grab;\n"            /* padded grab rect, screen px: (g_gx,g_gy,g_gw,g_gh) */
	"uniform vec2  u_bd_scale;\n"        /* (g_gw/screen_w, g_gh/screen_h): the grab rect's texture corner */
	"uniform float u_glass;\n"           /* 1 = liquid-glass window, 0 = plain opaque window */
	"uniform vec2  u_size_px;\n"         /* frame w,h in px */
	"uniform float u_radius_px;\n"       /* corner radius */
	"uniform vec4  u_client;\n"          /* client rect, WINDOW-LOCAL px: x,y,w,h */
	"uniform vec4  u_caps;\n"            /* caption spheres: close centre x,y; radius; slot width (px) */
	"uniform float u_focus;\n"           /* 1 focused, 0 not */
	"uniform float u_dark;\n"            /* 1 = dark slab variant */
	"uniform float u_inkwin;\n"          /* 1 = client pixels carry ink alpha; 0 = legacy opaque client */
	"uniform float u_debug;\n"           /* 0 off; 1 rim; 2 |offset|; 3 sharp grab; 4 blurred grab */
	"const float BEVEL   = 14.0;\n"      /* px over which the glass edge curves */
	"const float REFRACT = 12.0;\n"      /* max lens displacement at the rim, px (focused) */
	"const vec2  LIGHT   = vec2(-0.555, -0.832);\n"  /* toward the light, screen coords (top-left) */
	"float sd_box(vec2 p, vec2 b, float r) {\n"      /* signed distance, rounded box centred at 0 */
	"    vec2 q = abs(p) - b + vec2(r);\n"
	"    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;\n"
	"}\n"
	"vec3 backdrop(sampler2D t, vec2 spx) {\n"       /* sample a grab texture at a SCREEN-px point */
	"    vec2 uv = (spx - u_grab.xy) / u_grab.zw;\n"
	"    return texture2D(t, vec2(uv.x, 1.0 - uv.y) * u_bd_scale).rgb;\n" /* grab rows bottom-up; g_scene_tex is RGB */
	"}\n"
	"void main() {\n"
	"    vec2  p    = v_uv * u_size_px;\n"                       /* window-local px, top-down */
	"    vec2  pc   = p - 0.5 * u_size_px;\n"
	"    vec2  hb   = 0.5 * u_size_px;\n"
	"    float d    = sd_box(pc, hb, u_radius_px);\n"            /* < 0 inside the rounded window */
	"    float mask = clamp(0.5 - d, 0.0, 1.0);\n"               /* 1px AA edge */
	"    if (mask <= 0.0) discard;\n"
	"\n"
	"    vec4 ctex    = texture2D(u_content, v_uv);\n"
	"    vec3 content = ctex.bgr;\n"
	"    if (u_glass < 0.5) { gl_FragColor = vec4(content, mask); return; }\n"
	"\n"
	"    vec2  cd  = max(u_client.xy - p, p - (u_client.xy + u_client.zw));\n"
	"    float cin = 1.0 - clamp(max(cd.x, cd.y) + 0.5, 0.0, 1.0);\n"
	"\n"
	"    float rim = 1.0 - clamp(-d / BEVEL, 0.0, 1.0);\n"       /* 1 at the edge, 0 on the body */
	"\n"
	"    float e = 1.0;\n"                                        /* SDF gradient: points OUTWARD */
	"    vec2 g = vec2(sd_box(pc + vec2(e, 0.0), hb, u_radius_px) - sd_box(pc - vec2(e, 0.0), hb, u_radius_px),\n"
	"                  sd_box(pc + vec2(0.0, e), hb, u_radius_px) - sd_box(pc - vec2(0.0, e), hb, u_radius_px));\n"
	"    g = normalize(g + vec2(1e-4));\n"
	"\n"
	"    float refr   = REFRACT * mix(0.45, 1.0, u_focus);\n"     /* unfocused: gentler lens */
	"    vec2  off_px = g * (rim * rim) * refr;\n"                /* outward, quadratic toward the rim */
	"\n"
	"    vec3 ring;\n"                                            /* chromatic aberration on the lens */
	"    ring.r = backdrop(u_sharp, u_rect.xy + p + off_px * 0.92).r;\n"
	"    ring.g = backdrop(u_sharp, u_rect.xy + p + off_px       ).g;\n"
	"    ring.b = backdrop(u_sharp, u_rect.xy + p + off_px * 1.08).b;\n"
	"    vec3 body  = backdrop(u_backdrop, u_rect.xy + p);\n"     /* frosted flat body */
	"    vec3 glass = mix(body, ring, smoothstep(0.15, 0.8, rim));\n"
	"\n"
	"    float luma = dot(glass, vec3(0.299, 0.587, 0.114));\n"   /* vibrancy + frost lift */
	"    glass = mix(vec3(luma), glass, 1.22);\n"
	"    glass = glass * 0.90 + vec3(0.085);\n"
	"\n"
	"    vec3  tcol = mix(vec3(0.76, 0.78, 0.81), vec3(0.46, 0.60, 0.80), u_focus);\n"
	"    float tamt = mix(0.34, 0.22, u_focus);\n"                /* unfocused: paler, denser tint */
	"    tcol  = mix(tcol, vec3(0.10, 0.11, 0.13), u_dark);\n"
	"    tamt  = mix(tamt, 0.48, u_dark);\n"                      /* dark glass is denser */
	"    glass = mix(glass, tcol, tamt);\n"
	"\n"
	"    float facing = max(dot(g, LIGHT), 0.0);\n"               /* specular glint on the lit bevel */
	"    glass += vec3(pow(facing, 3.0) * rim * rim * (0.35 + 0.25 * u_focus));\n"
	"\n"
	"    float outer = 1.0 - clamp(-d - 0.5, 0.0, 1.0);\n"        /* 1px dark outer hairline */
	"    float inner = clamp(-d - 1.0, 0.0, 1.0) * (1.0 - clamp(-d - 2.2, 0.0, 1.0));\n" /* 1px white inner */
	"    glass = mix(glass, vec3(0.13, 0.16, 0.20), outer * 0.55);\n"
	"    glass += vec3(inner * 0.22);\n"
	"\n"
	"    for (int i = 0; i < 3; i++) {\n"     /* caption spheres: yellow / green / red-in-corner glass balls */
	"        vec2  c = vec2(u_caps.x - float(2 - i) * u_caps.w, u_caps.y);\n"
	"        vec2  q = p - c;\n"
	"        float r = length(q);\n"
	"        float R = u_caps.z;\n"
	"        if (r < R + 1.0) {\n"
	"            vec3 ccol = (i == 0) ? vec3(1.00, 0.74, 0.18)\n"
	"                      : ((i == 1) ? vec3(0.16, 0.78, 0.25)\n"
	"                                  : vec3(1.00, 0.37, 0.34));\n"
	"            ccol = mix(vec3(0.72, 0.76, 0.81), ccol, u_focus);\n"        /* unfocused: grey glass */
	"            vec2  n  = q / R;\n"
	"            float rr = clamp(r / R, 0.0, 1.0);\n"
	"            vec3 ball = backdrop(u_sharp, u_rect.xy + p + n * rr * rr * 5.0);\n" /* mini-lens */
	"            ball = mix(ball, ccol, 0.42 + 0.38 * smoothstep(0.45, 1.0, rr));\n"  /* rim-dense colour */
	"            ball *= 0.78 + 0.42 * (1.0 - rr);\n"              /* bright core, darker rim */
	"            vec2 sq = q - R * vec2(-0.30, -0.42);\n"
	"            vec2 cq = q - R * vec2( 0.00,  0.55);\n"
	"            ball += vec3(exp(-dot(sq, sq) / (R * R * 0.10)) * 0.90);\n"  /* specular dot, up-left */
	"            ball += vec3(exp(-dot(cq, cq) / (R * R * 0.16)) * 0.35);\n" /* caustic, bottom */
	"            ball += vec3(smoothstep(R - 1.6, R - 0.4, r)\n"
	"                         * (1.0 - smoothstep(R - 0.4, R + 0.6, r)) * 0.35);\n" /* rim ring */
	"            float cov = 1.0 - smoothstep(R - 0.6, R + 0.6, r);\n"        /* AA edge */
	"            glass = mix(glass, ball, cov);\n"
	"        }\n"
	"    }\n"
	"\n"
	"    float band_ink = smoothstep(0.02, 0.10, max(content.r, max(content.g, content.b)));\n"
	"    float inkcov   = mix(band_ink, ctex.a, cin * u_inkwin);\n"
	"    vec3  lit      = mix(glass, content, inkcov);\n"
	"    vec3  col      = mix(lit, content, cin * (1.0 - u_inkwin));\n"
	"\n"
	"    if (u_debug > 0.5) {\n"
	"        if      (u_debug < 1.5) col = vec3(rim, 0.0, 0.0);\n"
	"        else if (u_debug < 2.5) col = vec3(length(off_px) / max(refr, 0.001));\n"
	"        else if (u_debug < 3.5) col = backdrop(u_sharp,    u_rect.xy + p);\n"
	"        else                    col = backdrop(u_backdrop, u_rect.xy + p);\n"
	"    }\n"
	"    gl_FragColor = vec4(col, mask);\n"
	"}\n";

/* ---- program handles + cached uniform locations ----------------------------------------------- */
struct prog { GLuint id; GLint rect, screen; };
static struct prog p_tex, p_blit, p_keyed, p_solid, p_blur, p_win;
static GLint u_tex_tex, u_blit_tex, u_key_tex, u_key_key, u_solid_color;
static GLint u_blur_tex, u_blur_dir, u_blur_uv0, u_blur_uvsize;
static GLint u_win_content, u_win_backdrop, u_win_bd_scale, u_win_glass,
             u_win_size, u_win_radius, u_win_grab, u_win_sharp;
static GLint u_win_client, u_win_caps, u_win_focus, u_win_dark, u_win_inkwin, u_win_debug;
static float g_debug;   /* NWM_GLASS_DEBUG=1..4: 1 rim, 2 |offset|, 3 sharp grab, 4 blurred grab */

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

/* g_grab: the SHARP (unblurred) copy of the padded backdrop, refreshed per glass window via
 * glCopyTexSubImage2D into its lower-left corner — same screen-sized/corner-addressed convention as
 * g_blurA/g_blurB (no glTexImage2D/glFramebufferTexture2D on it, so it's fork-rule-safe: it is never
 * an FBO attachment, only a plain sampled texture). Kept for Task 5's refraction ring; harmless
 * plumbing until then. g_gx/g_gy/g_gw/g_gh (screen px, top-down origin) is the current grab rect —
 * both g_grab and g_blurB hold valid data only in their [0,g_gw)x[0,g_gh) corner. */
static GLuint g_grab = 0;
/* g_gw/g_gh default to 1 (not 0): FS_WIN divides by u_grab.zw, and when glass is fully disabled
 * (NWM_NO_GLASS or a failed blur-FBO init) blur_backdrop() never runs, so these must never be zero. */
static int g_gx = 0, g_gy = 0, g_gw = 1, g_gh = 1;

/* per-window content textures + their allocated size (recreated on resize). SINGLE-buffered: upload
 * (glTexImage2D + glFinish) then sample the same texture that frame. A double-buffered variant
 * (5f2b1c1) existed as a workaround for the "typing garble", blamed on async uploads outrunning
 * glFinish — but the garble's real cause was the kernel's fixed-VA device-mmap aliasing (fe3c6e2);
 * with that fixed, the fenced upload is coherent. The double buffer itself was buggy: content lagged
 * one COMPOSITE (not one frame — with damage-driven compositing the last keystroke never appeared
 * until the next event), and a drag flipped parity without uploading, flickering between two stale
 * buffers. Do not bring it back. */
static GLuint   g_win_tex[NW_MAX_WINDOWS];
static int      g_win_tw[NW_MAX_WINDOWS], g_win_th[NW_MAX_WINDOWS];
static unsigned g_win_gen[NW_MAX_WINDOWS];     /* content version last uploaded per slot; 0 = never */
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
	u_win_size     = glGetUniformLocation(p_win.id, "u_size_px");
	u_win_radius   = glGetUniformLocation(p_win.id, "u_radius_px");
	u_win_grab     = glGetUniformLocation(p_win.id, "u_grab");
	u_win_sharp    = glGetUniformLocation(p_win.id, "u_sharp");
	u_win_client   = glGetUniformLocation(p_win.id, "u_client");
	u_win_caps     = glGetUniformLocation(p_win.id, "u_caps");
	u_win_focus    = glGetUniformLocation(p_win.id, "u_focus");
	u_win_dark     = glGetUniformLocation(p_win.id, "u_dark");
	u_win_inkwin   = glGetUniformLocation(p_win.id, "u_inkwin");
	u_win_debug    = glGetUniformLocation(p_win.id, "u_debug");
	{ const char *dbg = getenv("NWM_GLASS_DEBUG"); g_debug = dbg ? (float) atoi(dbg) : 0.0f; }

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
		g_grab  = make_tex(screen_w, screen_h, 0);   /* sharp grab (never FBO-attached) */
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

/* ensure the content texture for window idx is (re)allocated to w×h; g_win_tw/th track the current
 * size. Returns 1 if it (re)allocated (new storage is uninitialised — caller must upload). */
static int ensure_win_tex(int idx, int w, int h)
{
	if (g_win_tex[idx] && g_win_tw[idx] == w && g_win_th[idx] == h) return 0;
	if (g_win_tex[idx]) glDeleteTextures(1, &g_win_tex[idx]);
	g_win_tex[idx] = make_tex(w, h, 0);
	g_win_tw[idx] = w; g_win_th[idx] = h;
	return 1;
}

/* Serial trace of each GL call in blur_backdrop — the oracle while the fork-hang is being chased
 * (Fable Phase 0). NWM_GL_TRACE=1 turns it on; off by default (silent once glass is proven). */
static int g_blur_trace = -1;
#define BLUR_MARK(tag) do { if (g_blur_trace) { printf("nw_gl blur: " tag "\n"); fflush(stdout); } } while (0)

/* GPU two-pass separable Gaussian of the PADDED region (window frame + NW_GLASS_PAD margin, clamped
 * to the screen) under the window into g_blurB's lower-left corner. The pad lets the glass shader
 * (and Task 5's refraction ring) sample content just outside the window frame — without it, a
 * cropped grab would CLAMP_TO_EDGE right at the frame boundary rather than reading real neighbouring
 * scene pixels. FORK RULE: no glGenTextures / glFramebufferTexture2D / glTexImage2D here — g_fboA and
 * g_fboB are pre-attached at init; we only switch framebuffer binding + viewport, and g_grab is only
 * ever glCopyTexSubImage2D'd into (never (re)attached), so it's fork-rule-safe too. Pass 1 (H) reads
 * the padded sub-region of g_scene_tex into g_fboA's corner; pass 2 (V) reads that corner of g_blurA
 * into g_fboB. FS_WIN maps the fragment's screen position through u_grab + u_bd_scale to sample
 * g_blurB's corner (see FS_WIN's guv/bd_uv); g_grab holds the matching SHARP copy for later use. */
static void blur_backdrop(int wx, int wy, int fw, int fh)
{
	if (g_blur_trace < 0) g_blur_trace = getenv("NWM_GL_TRACE") ? 1 : 0;
	const float STEP = 2.5f;                  /* per-tap spread (window px) → a soft, glass-like blur */

	/* pad the window frame + clamp to the screen -> the grab rect bookkeeping (screen px, top-down) */
	int gx0 = wx - NW_GLASS_PAD, gy0 = wy - NW_GLASS_PAD;
	int gx1 = wx + fw + NW_GLASS_PAD, gy1 = wy + fh + NW_GLASS_PAD;
	if (gx0 < 0) gx0 = 0;  if (gy0 < 0) gy0 = 0;
	if (gx1 > g_sw) gx1 = g_sw;  if (gy1 > g_sh) gy1 = g_sh;
	g_gx = gx0; g_gy = gy0; g_gw = gx1 - gx0; g_gh = gy1 - gy0;

	const float uw = (float) g_gw / g_sw, uh = (float) g_gh / g_sh;   /* the corner's uv extent */
	BLUR_MARK("enter");
	glDisable(GL_BLEND);                                            BLUR_MARK("blend-off");

	/* sharp copy (Task 5's ring): grab the padded region straight from the currently-bound scene FBO
	 * into g_grab's lower-left corner — same corner convention as g_blurA/g_blurB. Plain texture copy,
	 * no FBO attachment change, so it doesn't touch the fork rule. */
	glBindTexture(GL_TEXTURE_2D, g_grab);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_gx, g_sh - g_gy - g_gh, g_gw, g_gh);
	                                                                 BLUR_MARK("grab-sharp");

	glUseProgram(p_blur.id);                                        BLUR_MARK("use-prog");
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(u_blur_tex, 0);
	glUniform2f(u_blur_uvsize, uw, uh);
	glViewport(0, 0, g_gw, g_gh);              /* every pass draws into the lower-left corner */

	/* pass 1 (H): padded sub-region of g_scene_tex (y-flipped: scene tex is bottom-up in the FBO) */
	glBindFramebuffer(GL_FRAMEBUFFER, g_fboA);                      BLUR_MARK("bind-fboA");
	glBindTexture(GL_TEXTURE_2D, g_scene_tex);                      BLUR_MARK("bind-scene-tex");
	glUniform2f(u_blur_uv0, (float) g_gx / g_sw, 1.0f - (float) (g_gy + g_gh) / g_sh);
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

	/* DIAG (Dell GL-desktop freeze): mark frame entry and the hand-off to glkms_swap. If a boot log
	 * shows "enter #N" but never "pre-swap #N", the stall is in this function's upload/glFinish/
	 * compose region (render side); if it reaches "pre-swap #N", the glkms DIAG lines localise the
	 * swap. Self-limiting to 8 frames; remove once root-caused. */
	static int g_dbgf;
	int dbgf = g_dbgf < 8;
	if (dbgf) glkms_diag("nw_gl_frame: enter #%d (scene_dirty=%d)\n", g_dbgf, scene_dirty);

	/* Fault injection: fail this frame (as a real GL error would) once N frames have presented —
	 * the QEMU repro for the Dell boot #49 aftermath, where the mid-session GL->CPU fallback left
	 * the desktop frozen. Knob = marker file with the frame count (like nwm-solid); absent = off. */
	{
		static long fail_after = -2;                 /* -2 unread, -1 off, >=0 armed */
		static long frames_done;
		if (fail_after == -2) {
			fail_after = -1;
			int ffd = open("/disks/main/nanos/nwm-gl-fail-after", O_RDONLY);
			if (ffd >= 0) {
				char nb[16];
				int nn = (int) read(ffd, nb, sizeof nb - 1);
				close(ffd);
				if (nn > 0) { nb[nn] = 0; fail_after = atol(nb); }
			}
		}
		if (fail_after >= 0 && frames_done++ >= fail_after) {
			printf("nw_gl: FAULT-INJECT failing frame %ld (knob nwm-gl-fail-after)\n", frames_done);
			return -1;
		}
	}

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
			int dark = (w->title[0] == '\x01') || (w->style & NW_STYLE_DARK);

			/* Upload this window's cached frame render into its content texture (glTexImage2D +
			 * glFinish — the fence makes the sample-after-upload coherent; see the g_win_tex comment
			 * for why this is single-buffered). Upload gating:
			 *   - NOT interacting → refresh every frame (cheap; desktop idle between events);
			 *   - during a drag/resize a move changes only x/y (frame_gen static) → skip the upload, so a
			 *     drag frame still uploads ZERO window bytes and stays smooth. A window whose content
			 *     genuinely changed mid-interaction (frame_gen bumped) is still refreshed. */
			glActiveTexture(GL_TEXTURE0);
			int realloced = ensure_win_tex(idx, fw, fh);
			/* NWM_GL_SOLID=1 — D1 discriminator: upload an idx-keyed flat colour instead of
			 * w->frame (same traffic, same gating). Windows keeping their own solid colour under
			 * a terminal flood exonerates the whole GL transport; mixed/garbled colours convict it. */
			static int gl_solid = -1;
			static uint32_t *solid_px;
			if (gl_solid < 0) {
				gl_solid = getenv("NWM_GL_SOLID") ? 1 : 0;
				if (!gl_solid) {                     /* guest knob: marker file in the image */
					int mfd = open("/disks/main/nanos/nwm-solid", O_RDONLY);
					if (mfd >= 0) { gl_solid = 1; close(mfd); }
				}
			}
			const uint32_t *up_src = w->frame;
			if (gl_solid) {
				static const uint32_t cols[8] = {
					0xff2222ee, 0xff22cc22, 0xffee2222, 0xff22cccc,
					0xffcc22cc, 0xffcccc22, 0xffeeeeee, 0xff222222 };
				if (!solid_px) solid_px = (uint32_t *) malloc((size_t) 2048 * 2048 * 4);
				if (solid_px) {
					uint32_t c = cols[idx & 7];
					for (long i = 0; i < (long) fw * fh; i++) solid_px[i] = c;
					up_src = solid_px;
				}
			}
			if (realloced || !interacting || g_win_gen[idx] != w->frame_gen) {
				glBindTexture(GL_TEXTURE_2D, g_win_tex[idx]);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, up_src);
				glFinish();
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
			glUniform2f(u_win_bd_scale, (float) g_gw / g_sw, (float) g_gh / g_sh);
			glUniform1f(u_win_glass, glass ? 1.0f : 0.0f);
			glUniform2f(u_win_size, (float) fw, (float) fh);
			glUniform1f(u_win_radius, (float) g_radius);
			glUniform4f(u_win_grab, (float) g_gx, (float) g_gy, (float) g_gw, (float) g_gh);
			glUniform4f(u_win_client, (float) NW_BORDER, (float) NW_TITLEBAR_H,
			            (float) w->cw, (float) w->ch);
			glUniform4f(u_win_caps, (float) fw - NW_BORDER - 2.0f - NW_CLOSE * 0.5f,
			            NW_TITLEBAR_H * 0.5f, 8.0f, (float) NW_CLOSE);
			glUniform1f(u_win_focus, idx == s->focus ? 1.0f : 0.0f);
			glUniform1f(u_win_dark, dark ? 1.0f : 0.0f);
			glUniform1f(u_win_inkwin, (w->style & NW_STYLE_GLASS_CLIENT) ? 1.0f : 0.0f);
			glUniform1f(u_win_debug, g_debug);
			glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, w->glass ? g_grab : g_win_tex[idx]);
			glUniform1i(u_win_sharp, 2);
			glActiveTexture(GL_TEXTURE0);
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
	if (dbgf) { glkms_diag("nw_gl_frame: pre-swap #%d\n", g_dbgf); g_dbgf++; }
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

/* Mid-session FAILURE teardown: the context just failed a frame, so it may hold unsignalled
 * fences — every graceful teardown call (glDelete*, eglMakeCurrent, eglTerminate, gbm destroy)
 * can wait on them WITHOUT timeout, and that hang froze the desktop after the GL->CPU fallback
 * (Dell boot #49, reproduced on QEMU with the fail-after knob). Touch NOTHING that can block:
 * restore the scanout + close our DRM fd (glkms_close_wedged) and leak the GL state — the CPU
 * compositor takes over, the kernel reclaims the rest at process exit. */
void nw_gl_shutdown_wedged(void)
{
	printf("nw_gl: wedged shutdown (GL state leaked, scanout restored)\n");
	if (g_ok) glkms_close_wedged(&g_kms);
	if (g_chrome_px) { free(g_chrome_px); g_chrome_px = 0; }   /* plain malloc — safe to free */
	g_ok = 0; g_sw = g_sh = 0;
	g_chrome_ready = 0; g_wall_dirty = 1;
}

void nw_gl_shutdown(void)
{
	printf("nw_gl: shutdown: delete GL objects\n");   /* stage marker — see glkms_close() */
	for (int i = 0; i < NW_MAX_WINDOWS; i++) {
		if (g_win_tex[i]) { glDeleteTextures(1, &g_win_tex[i]); g_win_tex[i] = 0; }
		g_win_tw[i] = g_win_th[i] = 0; g_win_gen[i] = 0;
	}
	g_chrome_ready = 0; g_wall_dirty = 1;
	if (g_grab)   { glDeleteTextures(1, &g_grab);   g_grab = 0; }
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
