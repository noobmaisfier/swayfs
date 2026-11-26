#ifndef EGL_EGLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES
#endif

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/util/transform.h>
#include "log.h"
#include "sway/render/corner_renderer.h"

struct sway_rect_program {
	GLuint program;
	GLint pos_attrib;
	GLint proj_loc;
	GLint size_loc;
	GLint color_loc;
	GLint corners_loc;
	GLint clip_box_loc;
	GLint clip_corners_loc;
};

struct sway_tex_program {
	GLuint program;
	GLint pos_attrib;
	GLint proj_loc;
	GLint tex_proj_loc;
	GLint tex_loc;
	GLint size_loc;
	GLint alpha_loc;
	GLint has_alpha_loc;
	GLint corners_loc;
	GLint gamma_lut_loc;
	GLint gamma_params_loc;
};

struct sway_corner_renderer {
	struct wlr_renderer *wlr_renderer;
	struct wl_listener renderer_destroy;

	bool initialized;
	bool has_oes_external;
	bool has_native_fence_sync;
	PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR;
	PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR;
	PFNEGLWAITSYNCKHRPROC eglWaitSyncKHR;
	PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupNativeFenceFDANDROID;

	struct sway_rect_program rect_prog;
	struct sway_tex_program tex_2d_prog;
	struct sway_tex_program tex_oes_prog;
	GLuint gamma_tex;

	EGLDisplay prev_dpy;
	EGLSurface prev_draw;
	EGLSurface prev_read;
	EGLContext prev_ctx;
	bool ctx_active;
	int out_width;
	int out_height;
};

// Keep a single cached wrapper matching the active wlr_renderer (Sway uses one renderer).
static struct sway_corner_renderer *active_cr = NULL;

#define GLSL_PRECISION \
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n" \
	"precision highp float;\n" \
	"#else\n" \
	"precision mediump float;\n" \
	"#endif\n"

static const char *rect_vert_src =
	GLSL_PRECISION
	"attribute vec2 a_pos;\n"
	"uniform mat3 u_proj;\n"
	"uniform vec2 u_size;\n"
	"varying vec2 v_local_pos;\n"
	"void main() {\n"
	"    vec3 p = u_proj * vec3(a_pos, 1.0);\n"
	"    gl_Position = vec4(p.xy, 0.0, 1.0);\n"
	"    v_local_pos = a_pos * u_size;\n"
	"}\n";

static const char *quad_vert_src =
	GLSL_PRECISION
	"attribute vec2 a_pos;\n"
	"uniform mat3 u_proj;\n"
	"uniform mat3 u_tex_proj;\n"
	"uniform vec2 u_size;\n"
	"varying vec2 v_local_pos;\n"
	"varying vec2 v_texcoord;\n"
	"void main() {\n"
	"    vec3 p = u_proj * vec3(a_pos, 1.0);\n"
	"    gl_Position = vec4(p.xy, 0.0, 1.0);\n"
	"    v_local_pos = a_pos * u_size;\n"
	"    v_texcoord = (u_tex_proj * vec3(a_pos, 1.0)).xy;\n"
	"}\n";

static const char *rect_frag_src =
	GLSL_PRECISION
	"varying vec2 v_local_pos;\n"
	"uniform vec2 u_size;\n"
	"uniform vec4 u_color;\n"
	"uniform vec4 u_corners;\n"
	"uniform vec4 u_clip_box;\n"
	"uniform vec4 u_clip_corners;\n"
	"float sdRoundedBox(vec2 p, vec2 half_size, vec4 r) {\n"
	"    vec2 r2 = (p.x > 0.0) ? vec2(r.y, r.z) : vec2(r.x, r.w);\n"
	"    float rad = (p.y > 0.0) ? r2.y : r2.x;\n"
	"    rad = min(rad, min(half_size.x, half_size.y));\n"
	"    vec2 q = abs(p) - half_size + rad;\n"
	"    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - rad;\n"
	"}\n"
	"void main() {\n"
	"    vec2 half_size = 0.5 * u_size;\n"
	"    float d = sdRoundedBox(v_local_pos - half_size, half_size, u_corners);\n"
	"    float alpha = clamp(0.5 - d, 0.0, 1.0);\n"
	"    if (u_clip_box.z > 0.0 && u_clip_box.w > 0.0) {\n"
	"        vec2 chalf = 0.5 * u_clip_box.zw;\n"
	"        vec2 cp = v_local_pos - (u_clip_box.xy + chalf);\n"
	"        float cd = sdRoundedBox(cp, chalf, u_clip_corners);\n"
	"        alpha *= clamp(cd + 0.5, 0.0, 1.0);\n"
	"    }\n"
	"    gl_FragColor = u_color * alpha;\n"
	"}\n";

#define TEX_FRAG_BODY \
	"varying vec2 v_local_pos;\n" \
	"varying vec2 v_texcoord;\n" \
	"uniform vec2 u_size;\n" \
	"uniform float u_alpha;\n" \
	"uniform float u_has_alpha;\n" \
	"uniform vec4 u_corners;\n" \
	"uniform sampler2D u_gamma_lut;\n" \
	"uniform vec2 u_gamma_params;\n" \
	"float sdRoundedBox(vec2 p, vec2 half_size, vec4 r) {\n" \
	"    vec2 r2 = (p.x > 0.0) ? vec2(r.y, r.z) : vec2(r.x, r.w);\n" \
	"    float rad = (p.y > 0.0) ? r2.y : r2.x;\n" \
	"    rad = min(rad, min(half_size.x, half_size.y));\n" \
	"    vec2 q = abs(p) - half_size + rad;\n" \
	"    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - rad;\n" \
	"}\n" \
	"void main() {\n" \
	"    vec4 tex = texture2D(u_tex, v_texcoord);\n" \
	"    if (u_has_alpha < 0.5) {\n" \
	"        tex.a = 1.0;\n" \
	"    }\n" \
	"    if (u_gamma_params.x > 0.0) {\n" \
	"        vec3 lut_uv = clamp(tex.rgb, 0.0, 1.0) * u_gamma_params.x + u_gamma_params.y;\n" \
	"        tex.r = texture2D(u_gamma_lut, vec2(lut_uv.r, 0.5)).r;\n" \
	"        tex.g = texture2D(u_gamma_lut, vec2(lut_uv.g, 0.5)).g;\n" \
	"        tex.b = texture2D(u_gamma_lut, vec2(lut_uv.b, 0.5)).b;\n" \
	"    }\n" \
	"    vec2 half_size = 0.5 * u_size;\n" \
	"    float d = sdRoundedBox(v_local_pos - half_size, half_size, u_corners);\n" \
	"    float corner_alpha = clamp(0.5 - d, 0.0, 1.0);\n" \
	"    gl_FragColor = tex * (u_alpha * corner_alpha);\n" \
	"}\n"

static const char *tex_2d_frag_src =
	GLSL_PRECISION
	"uniform sampler2D u_tex;\n"
	TEX_FRAG_BODY;

static const char *tex_oes_frag_src =
	"#extension GL_OES_EGL_image_external : require\n"
	GLSL_PRECISION
	"uniform samplerExternalOES u_tex;\n"
	TEX_FRAG_BODY;

static GLuint compile_shader(GLenum type, const char *src) {
	GLuint shader = glCreateShader(type);
	if (!shader) {
		return 0;
	}
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);
	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		sway_log(SWAY_ERROR, "Corner shader compile error: %s", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static GLuint link_program(const char *vert_src, const char *frag_src) {
	GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_src);
	GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
	if (!vs || !fs) {
		if (vs) glDeleteShader(vs);
		if (fs) glDeleteShader(fs);
		return 0;
	}
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetProgramInfoLog(prog, sizeof(log), NULL, log);
		sway_log(SWAY_ERROR, "Corner shader link error: %s", log);
		glDeleteProgram(prog);
		return 0;
	}
	return prog;
}

static void push_egl_ctx(struct sway_corner_renderer *cr) {
	assert(!cr->ctx_active);
	cr->prev_dpy = eglGetCurrentDisplay();
	cr->prev_draw = eglGetCurrentSurface(EGL_DRAW);
	cr->prev_read = eglGetCurrentSurface(EGL_READ);
	cr->prev_ctx = eglGetCurrentContext();

	struct wlr_egl *wlr_egl = wlr_gles2_renderer_get_egl(cr->wlr_renderer);
	EGLDisplay dpy = wlr_egl_get_display(wlr_egl);
	EGLContext ctx = wlr_egl_get_context(wlr_egl);
	if (cr->prev_dpy != dpy || cr->prev_ctx != ctx) {
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
	}
	cr->ctx_active = true;
}

static void pop_egl_ctx(struct sway_corner_renderer *cr) {
	assert(cr->ctx_active);
	struct wlr_egl *wlr_egl = wlr_gles2_renderer_get_egl(cr->wlr_renderer);
	EGLDisplay dpy = wlr_egl_get_display(wlr_egl);
	EGLContext ctx = wlr_egl_get_context(wlr_egl);
	if (cr->prev_dpy != dpy || cr->prev_ctx != ctx) {
		if (cr->prev_dpy != EGL_NO_DISPLAY && cr->prev_ctx != EGL_NO_CONTEXT) {
			eglMakeCurrent(cr->prev_dpy, cr->prev_draw, cr->prev_read, cr->prev_ctx);
		} else {
			eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		}
	}
	cr->ctx_active = false;
}

static bool init_programs(struct sway_corner_renderer *cr) {
	if (cr->initialized) {
		return cr->rect_prog.program != 0 && cr->tex_2d_prog.program != 0;
	}
	cr->initialized = true;

	GLuint rp = link_program(rect_vert_src, rect_frag_src);
	if (!rp) {
		return false;
	}
	cr->rect_prog = (struct sway_rect_program){
		.program = rp,
		.pos_attrib = glGetAttribLocation(rp, "a_pos"),
		.proj_loc = glGetUniformLocation(rp, "u_proj"),
		.size_loc = glGetUniformLocation(rp, "u_size"),
		.color_loc = glGetUniformLocation(rp, "u_color"),
		.corners_loc = glGetUniformLocation(rp, "u_corners"),
		.clip_box_loc = glGetUniformLocation(rp, "u_clip_box"),
		.clip_corners_loc = glGetUniformLocation(rp, "u_clip_corners"),
	};

	GLuint tp2d = link_program(quad_vert_src, tex_2d_frag_src);
	if (!tp2d) {
		return false;
	}
	cr->tex_2d_prog = (struct sway_tex_program){
		.program = tp2d,
		.pos_attrib = glGetAttribLocation(tp2d, "a_pos"),
		.proj_loc = glGetUniformLocation(tp2d, "u_proj"),
		.tex_proj_loc = glGetUniformLocation(tp2d, "u_tex_proj"),
		.tex_loc = glGetUniformLocation(tp2d, "u_tex"),
		.size_loc = glGetUniformLocation(tp2d, "u_size"),
		.alpha_loc = glGetUniformLocation(tp2d, "u_alpha"),
		.has_alpha_loc = glGetUniformLocation(tp2d, "u_has_alpha"),
		.corners_loc = glGetUniformLocation(tp2d, "u_corners"),
		.gamma_lut_loc = glGetUniformLocation(tp2d, "u_gamma_lut"),
		.gamma_params_loc = glGetUniformLocation(tp2d, "u_gamma_params"),
	};

	cr->has_oes_external = wlr_gles2_renderer_check_ext(
		cr->wlr_renderer, "GL_OES_EGL_image_external");
	if (cr->has_oes_external) {
		GLuint tpoes = link_program(quad_vert_src, tex_oes_frag_src);
		if (tpoes) {
			cr->tex_oes_prog = (struct sway_tex_program){
				.program = tpoes,
				.pos_attrib = glGetAttribLocation(tpoes, "a_pos"),
				.proj_loc = glGetUniformLocation(tpoes, "u_proj"),
				.tex_proj_loc = glGetUniformLocation(tpoes, "u_tex_proj"),
				.tex_loc = glGetUniformLocation(tpoes, "u_tex"),
				.size_loc = glGetUniformLocation(tpoes, "u_size"),
				.alpha_loc = glGetUniformLocation(tpoes, "u_alpha"),
				.has_alpha_loc = glGetUniformLocation(tpoes, "u_has_alpha"),
				.corners_loc = glGetUniformLocation(tpoes, "u_corners"),
				.gamma_lut_loc = glGetUniformLocation(tpoes, "u_gamma_lut"),
				.gamma_params_loc = glGetUniformLocation(tpoes, "u_gamma_params"),
			};
		}
	}

	struct wlr_egl *wlr_egl = wlr_gles2_renderer_get_egl(cr->wlr_renderer);
	EGLDisplay dpy = wlr_egl_get_display(wlr_egl);
	const char *egl_exts = eglQueryString(dpy, EGL_EXTENSIONS);
	if (egl_exts && strstr(egl_exts, "EGL_ANDROID_native_fence_sync") &&
			strstr(egl_exts, "EGL_KHR_wait_sync")) {
		cr->eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
		cr->eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
		cr->eglWaitSyncKHR = (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
		cr->eglDupNativeFenceFDANDROID = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)
			eglGetProcAddress("eglDupNativeFenceFDANDROID");
		cr->has_native_fence_sync = cr->eglCreateSyncKHR && cr->eglDestroySyncKHR && cr->eglWaitSyncKHR;
	}

	return true;
}

static void handle_renderer_destroy(struct wl_listener *listener, void *data) {
	struct sway_corner_renderer *cr = wl_container_of(listener, cr, renderer_destroy);
	sway_corner_renderer_destroy(cr);
}

struct sway_corner_renderer *sway_corner_renderer_for_wlr(struct wlr_renderer *renderer) {
	if (!renderer || !wlr_renderer_is_gles2(renderer)) {
		return NULL;
	}
	if (active_cr && active_cr->wlr_renderer == renderer) {
		return active_cr;
	}
	if (active_cr) {
		sway_corner_renderer_destroy(active_cr);
	}

	struct sway_corner_renderer *cr = calloc(1, sizeof(*cr));
	if (!cr) {
		return NULL;
	}
	cr->wlr_renderer = renderer;
	cr->renderer_destroy.notify = handle_renderer_destroy;
	wl_signal_add(&renderer->events.destroy, &cr->renderer_destroy);
	active_cr = cr;
	return cr;
}

void sway_corner_renderer_destroy(struct sway_corner_renderer *cr) {
	if (!cr) {
		return;
	}
	if (active_cr == cr) {
		active_cr = NULL;
	}
	wl_list_remove(&cr->renderer_destroy.link);
	if (cr->initialized) {
		push_egl_ctx(cr);
		if (cr->rect_prog.program) {
			glDeleteProgram(cr->rect_prog.program);
		}
		if (cr->tex_2d_prog.program) {
			glDeleteProgram(cr->tex_2d_prog.program);
		}
		if (cr->tex_oes_prog.program) {
			glDeleteProgram(cr->tex_oes_prog.program);
		}
		if (cr->gamma_tex) {
			glDeleteTextures(1, &cr->gamma_tex);
		}
		pop_egl_ctx(cr);
	}
	free(cr);
}

bool sway_corner_renderer_begin(struct sway_corner_renderer *cr,
		struct wlr_buffer *dst_buffer, const pixman_region32_t *damage) {
	if (!cr || !dst_buffer) {
		return false;
	}
	GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(cr->wlr_renderer, dst_buffer);
	if (!fbo) {
		return false;
	}

	push_egl_ctx(cr);
	if (!init_programs(cr)) {
		pop_egl_ctx(cr);
		return false;
	}

	cr->out_width = dst_buffer->width;
	cr->out_height = dst_buffer->height;

	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, dst_buffer->width, dst_buffer->height);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_SCISSOR_TEST);
	(void)damage;
	return true;
}

bool sway_corner_renderer_end_sync(struct sway_corner_renderer *cr,
		struct wlr_drm_syncobj_timeline *signal_timeline, uint64_t signal_point) {
	if (!cr || !cr->ctx_active) {
		return false;
	}
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	bool synced = false;
	if (signal_timeline && cr->has_native_fence_sync && cr->eglDupNativeFenceFDANDROID) {
		struct wlr_egl *wlr_egl = wlr_gles2_renderer_get_egl(cr->wlr_renderer);
		EGLDisplay dpy = wlr_egl_get_display(wlr_egl);
		EGLSyncKHR sync = cr->eglCreateSyncKHR(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
		if (sync != EGL_NO_SYNC_KHR) {
			int fence_fd = cr->eglDupNativeFenceFDANDROID(dpy, sync);
			cr->eglDestroySyncKHR(dpy, sync);
			if (fence_fd >= 0) {
				synced = wlr_drm_syncobj_timeline_import_sync_file(
					signal_timeline, signal_point, fence_fd);
				close(fence_fd);
			}
		}
	}

	if (!synced) {
		// Ensure FBO rendering is complete before scanout when no fence FD was exported
		glFinish();
	}

	pop_egl_ctx(cr);
	return synced;
}

void sway_corner_renderer_end_flush(struct sway_corner_renderer *cr) {
	if (!cr || !cr->ctx_active) {
		return;
	}
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glFlush();
	pop_egl_ctx(cr);
}

void sway_corner_renderer_end(struct sway_corner_renderer *cr) {
	sway_corner_renderer_end_sync(cr, NULL, 0);
}

void sway_corner_renderer_clear_damage(struct sway_corner_renderer *cr,
		const pixman_region32_t *damage, float r, float g, float b, float a) {
	int n = 0;
	const pixman_box32_t *rects = pixman_region32_rectangles(damage, &n);
	glClearColor(r, g, b, a);
	for (int i = 0; i < n; i++) {
		int x = rects[i].x1;
		int y = rects[i].y1;
		int w = rects[i].x2 - rects[i].x1;
		int h = rects[i].y2 - rects[i].y1;
		if (w <= 0 || h <= 0) {
			continue;
		}
		glScissor(x, y, w, h);
		glClear(GL_COLOR_BUFFER_BIT);
	}
}

// Column-major 3x3 matrix multiplication: out = a * b
static void mat3_mul(float out[static 9], const float a[static 9], const float b[static 9]) {
	float res[9];
	for (int col = 0; col < 3; col++) {
		for (int row = 0; row < 3; row++) {
			res[col * 3 + row] =
				a[0 * 3 + row] * b[col * 3 + 0] +
				a[1 * 3 + row] * b[col * 3 + 1] +
				a[2 * 3 + row] * b[col * 3 + 2];
		}
	}
	memcpy(out, res, sizeof(res));
}

static void mat3_transform_unit_square(float out[static 9], enum wl_output_transform tr) {
	// Maps unit square [0,1]^2 into transformed unit square [0,1]^2
	// Equivalent to wlroots's output transform matrices on unit quad.
	static const float mats[8][9] = {
		[WL_OUTPUT_TRANSFORM_NORMAL] = {
			1, 0, 0,
			0, 1, 0,
			0, 0, 1,
		},
		[WL_OUTPUT_TRANSFORM_90] = {
			0, 1, 0,
			-1, 0, 0,
			1, 0, 1,
		},
		[WL_OUTPUT_TRANSFORM_180] = {
			-1, 0, 0,
			0, -1, 0,
			1, 1, 1,
		},
		[WL_OUTPUT_TRANSFORM_270] = {
			0, -1, 0,
			1, 0, 0,
			0, 1, 1,
		},
		[WL_OUTPUT_TRANSFORM_FLIPPED] = {
			-1, 0, 0,
			0, 1, 0,
			1, 0, 1,
		},
		[WL_OUTPUT_TRANSFORM_FLIPPED_90] = {
			0, 1, 0,
			1, 0, 0,
			0, 0, 1,
		},
		[WL_OUTPUT_TRANSFORM_FLIPPED_180] = {
			1, 0, 0,
			0, -1, 0,
			0, 1, 1,
		},
		[WL_OUTPUT_TRANSFORM_FLIPPED_270] = {
			0, -1, 0,
			-1, 0, 0,
			1, 1, 1,
		},
	};
	memcpy(out, mats[tr & 7], sizeof(float) * 9);
}

// Compute column-major mat3 mapping unit quad a_pos in [0,1]^2 -> NDC [-1,1]^2
// dst_box is in logical output-transformed coordinates (before output_transform),
// wait: scene render transforms dst_box into output-buffer coordinates!
// Here dst_box is ALREADY in output-buffer pixel coordinates, OR we apply output_transform:
// Let's keep it crystal clear:
// dst_box is in output-LOCAL (post-output-scale, pre-output-transform) coords,
// and output_transform maps from output-local to output-buffer coords (out_width x out_height).
static void build_ndc_proj(float proj[static 9], const struct wlr_box *dst_box,
		enum wl_output_transform output_transform, int out_buf_w, int out_buf_h) {
	// First scale unit quad [0,1]^2 to box in output-local pixel coords (logical * scale)
	int eff_w = out_buf_w;
	int eff_h = out_buf_h;
	if (output_transform & WL_OUTPUT_TRANSFORM_90) {
		eff_w = out_buf_h;
		eff_h = out_buf_w;
	}

	// Unit quad -> normalized output-local [0,1]^2
	float local_mat[9] = {
		(float)dst_box->width / (float)eff_w, 0.0f, 0.0f,
		0.0f, (float)dst_box->height / (float)eff_h, 0.0f,
		(float)dst_box->x / (float)eff_w, (float)dst_box->y / (float)eff_h, 1.0f,
	};

	// Output transform in [0,1]^2: note wlroots output_transform rotates the *output*,
	// so mapping output-local coords into buffer coords applies wlr_output_transform_invert.
	// Wait: let's verify! For TRANSFORM_90 (90 deg CCW output rotation in Wayland? No:
	// wl_output.transform_90 means output is rotated 90 deg clockwise when viewed, so top-left
	// of logical view goes to top-right of physical buffer (1,0), bottom-left goes to (0,0)).
	// Wait: mats[WL_OUTPUT_TRANSFORM_90] maps (0,0)->(1,0), (1,0)->(1,1), (0,1)->(0,0)!
	// Check: x' = -y + 1, y' = x.
	// (0,0) -> (1,0) [top-right of buffer]
	// (1,0) -> (1,1) [bottom-right of buffer]
	// (0,1) -> (0,0) [top-left of buffer]
	// That IS 90 deg clockwise!
	float tr_mat[9];
	mat3_transform_unit_square(tr_mat, output_transform);

	// [0,1]^2 buffer coords -> NDC [-1,1]^2
	float ndc_mat[9] = {
		2.0f, 0.0f, 0.0f,
		0.0f, 2.0f, 0.0f,
		-1.0f, -1.0f, 1.0f,
	};

	float tmp[9];
	mat3_mul(tmp, tr_mat, local_mat);
	mat3_mul(proj, ndc_mat, tmp);
}

static const GLfloat quad_verts[8] = {
	0.0f, 0.0f,
	1.0f, 0.0f,
	0.0f, 1.0f,
	1.0f, 1.0f,
};

static void draw_scissored_quad(const pixman_region32_t *damage) {
	int n = 0;
	const pixman_box32_t *rects = pixman_region32_rectangles(damage, &n);
	for (int i = 0; i < n; i++) {
		int x = rects[i].x1;
		int y = rects[i].y1;
		int w = rects[i].x2 - rects[i].x1;
		int h = rects[i].y2 - rects[i].y1;
		if (w <= 0 || h <= 0) {
			continue;
		}
		glScissor(x, y, w, h);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
}

void sway_corner_renderer_draw_rect(struct sway_corner_renderer *cr,
		const struct wlr_box *dst_box,
		enum wl_output_transform output_transform,
		int out_width, int out_height,
		const float color[static 4],
		const struct fx_corner_radii *corners_scaled,
		const struct clipped_region *clip_scaled,
		const pixman_region32_t *damage) {
	if (dst_box->width <= 0 || dst_box->height <= 0 || pixman_region32_empty(damage)) {
		return;
	}

	float proj[9];
	build_ndc_proj(proj, dst_box, output_transform, out_width, out_height);

	struct sway_rect_program *p = &cr->rect_prog;
	glUseProgram(p->program);
	glUniformMatrix3fv(p->proj_loc, 1, GL_FALSE, proj);
	glUniform2f(p->size_loc, (float)dst_box->width, (float)dst_box->height);
	glUniform4fv(p->color_loc, 1, color);

	if (corners_scaled) {
		glUniform4f(p->corners_loc,
			(float)corners_scaled->top_left,
			(float)corners_scaled->top_right,
			(float)corners_scaled->bottom_right,
			(float)corners_scaled->bottom_left);
	} else {
		glUniform4f(p->corners_loc, 0.0f, 0.0f, 0.0f, 0.0f);
	}

	if (clip_scaled && clip_scaled->area.width > 0 && clip_scaled->area.height > 0) {
		glUniform4f(p->clip_box_loc,
			(float)clip_scaled->area.x,
			(float)clip_scaled->area.y,
			(float)clip_scaled->area.width,
			(float)clip_scaled->area.height);
		glUniform4f(p->clip_corners_loc,
			(float)clip_scaled->corners.top_left,
			(float)clip_scaled->corners.top_right,
			(float)clip_scaled->corners.bottom_right,
			(float)clip_scaled->corners.bottom_left);
	} else {
		glUniform4f(p->clip_box_loc, 0.0f, 0.0f, 0.0f, 0.0f);
		glUniform4f(p->clip_corners_loc, 0.0f, 0.0f, 0.0f, 0.0f);
	}

	glVertexAttribPointer(p->pos_attrib, 2, GL_FLOAT, GL_FALSE, 0, quad_verts);
	glEnableVertexAttribArray(p->pos_attrib);
	draw_scissored_quad(damage);
	glDisableVertexAttribArray(p->pos_attrib);
}

static void draw_texture_internal(struct sway_corner_renderer *cr,
		struct wlr_texture *texture,
		const struct wlr_fbox *src_box,
		const struct wlr_box *dst_box,
		enum wl_output_transform tex_transform,
		enum wl_output_transform output_transform,
		int out_width, int out_height,
		float alpha,
		enum wlr_scale_filter_mode filter_mode,
		const struct fx_corner_radii *corners_scaled,
		struct wlr_drm_syncobj_timeline *wait_timeline,
		uint64_t wait_point,
		const uint16_t *gamma_table,
		size_t gamma_size,
		const pixman_region32_t *damage) {
	if (!texture || dst_box->width <= 0 || dst_box->height <= 0 ||
			pixman_region32_empty(damage)) {
		return;
	}

	if (wait_timeline) {
		int sync_fd = wlr_drm_syncobj_timeline_export_sync_file(wait_timeline, wait_point);
		if (sync_fd >= 0) {
			if (cr->has_native_fence_sync) {
				struct wlr_egl *wlr_egl = wlr_gles2_renderer_get_egl(cr->wlr_renderer);
				EGLDisplay dpy = wlr_egl_get_display(wlr_egl);
				EGLint attribs[] = {
					EGL_SYNC_NATIVE_FENCE_FD_ANDROID, sync_fd,
					EGL_NONE,
				};
				EGLSyncKHR sync = cr->eglCreateSyncKHR(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
				if (sync != EGL_NO_SYNC_KHR) {
					cr->eglWaitSyncKHR(dpy, sync, 0);
					cr->eglDestroySyncKHR(dpy, sync);
				} else {
					close(sync_fd);
				}
			} else {
				close(sync_fd);
			}
		}
	}

	struct wlr_gles2_texture_attribs attribs;
	wlr_gles2_texture_get_attribs(texture, &attribs);

	struct sway_tex_program *p = NULL;
	if (attribs.target == GL_TEXTURE_EXTERNAL_OES && cr->tex_oes_prog.program) {
		p = &cr->tex_oes_prog;
	} else if (attribs.target == GL_TEXTURE_2D) {
		p = &cr->tex_2d_prog;
	} else {
		return;
	}

	// src_box is in buffer-transformed coordinates (after tex_transform).
	int tr_w = (int)texture->width;
	int tr_h = (int)texture->height;
	if (tex_transform & WL_OUTPUT_TRANSFORM_90) {
		tr_w = (int)texture->height;
		tr_h = (int)texture->width;
	}

	struct wlr_fbox sbox = {0};
	if (src_box && !wlr_fbox_empty(src_box)) {
		sbox = *src_box;
	} else {
		sbox = (struct wlr_fbox){ 0, 0, tr_w, tr_h };
	}

	// Unit quad [0,1]^2 -> normalized transformed texture coords [0,1]^2
	float src_mat[9] = {
		(float)(sbox.width / tr_w), 0.0f, 0.0f,
		0.0f, (float)(sbox.height / tr_h), 0.0f,
		(float)(sbox.x / tr_w), (float)(sbox.y / tr_h), 1.0f,
	};

	float tex_tr_mat[9];
	mat3_transform_unit_square(tex_tr_mat, wlr_output_transform_invert(tex_transform));

	float tex_proj[9];
	mat3_mul(tex_proj, tex_tr_mat, src_mat);

	float proj[9];
	build_ndc_proj(proj, dst_box, output_transform, out_width, out_height);

	glUseProgram(p->program);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(attribs.target, attribs.tex);

	GLint gl_filter = filter_mode == WLR_SCALE_FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
	glTexParameteri(attribs.target, GL_TEXTURE_MIN_FILTER, gl_filter);
	glTexParameteri(attribs.target, GL_TEXTURE_MAG_FILTER, gl_filter);
	glTexParameteri(attribs.target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(attribs.target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glUniformMatrix3fv(p->proj_loc, 1, GL_FALSE, proj);
	glUniformMatrix3fv(p->tex_proj_loc, 1, GL_FALSE, tex_proj);
	glUniform1i(p->tex_loc, 0);
	glUniform2f(p->size_loc, (float)dst_box->width, (float)dst_box->height);
	glUniform1f(p->alpha_loc, alpha);
	glUniform1f(p->has_alpha_loc, attribs.has_alpha ? 1.0f : 0.0f);

	if (corners_scaled) {
		glUniform4f(p->corners_loc,
			(float)corners_scaled->top_left,
			(float)corners_scaled->top_right,
			(float)corners_scaled->bottom_right,
			(float)corners_scaled->bottom_left);
	} else {
		glUniform4f(p->corners_loc, 0.0f, 0.0f, 0.0f, 0.0f);
	}

	if (gamma_table && gamma_size > 1) {
		uint8_t *rgba = malloc(gamma_size * 4);
		if (rgba) {
			const uint16_t *r = gamma_table;
			const uint16_t *g = gamma_table + gamma_size;
			const uint16_t *b = gamma_table + 2 * gamma_size;
			for (size_t i = 0; i < gamma_size; i++) {
				rgba[i * 4 + 0] = (uint8_t)(r[i] >> 8);
				rgba[i * 4 + 1] = (uint8_t)(g[i] >> 8);
				rgba[i * 4 + 2] = (uint8_t)(b[i] >> 8);
				rgba[i * 4 + 3] = 255;
			}
			if (!cr->gamma_tex) {
				glGenTextures(1, &cr->gamma_tex);
			}
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, cr->gamma_tex);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)gamma_size, 1, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, rgba);
			free(rgba);

			glUniform1i(p->gamma_lut_loc, 1);
			float n = (float)gamma_size;
			glUniform2f(p->gamma_params_loc, (n - 1.0f) / n, 0.5f / n);
		} else {
			glUniform2f(p->gamma_params_loc, 0.0f, 0.0f);
		}
	} else {
		glUniform2f(p->gamma_params_loc, 0.0f, 0.0f);
	}

	glVertexAttribPointer(p->pos_attrib, 2, GL_FLOAT, GL_FALSE, 0, quad_verts);
	glEnableVertexAttribArray(p->pos_attrib);
	draw_scissored_quad(damage);
	glDisableVertexAttribArray(p->pos_attrib);

	if (gamma_table && gamma_size > 1) {
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, 0);
	}
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(attribs.target, 0);
}

void sway_corner_renderer_draw_texture(struct sway_corner_renderer *cr,
		struct wlr_texture *texture,
		const struct wlr_fbox *src_box,
		const struct wlr_box *dst_box,
		enum wl_output_transform tex_transform,
		enum wl_output_transform output_transform,
		int out_width, int out_height,
		float alpha,
		enum wlr_scale_filter_mode filter_mode,
		const struct fx_corner_radii *corners_scaled,
		struct wlr_drm_syncobj_timeline *wait_timeline,
		uint64_t wait_point,
		const pixman_region32_t *damage) {
	draw_texture_internal(cr, texture, src_box, dst_box, tex_transform,
		output_transform, out_width, out_height, alpha, filter_mode,
		corners_scaled, wait_timeline, wait_point, NULL, 0, damage);
}

void sway_corner_renderer_draw_gamma_blit(struct sway_corner_renderer *cr,
		struct wlr_texture *texture,
		int out_width, int out_height,
		const uint16_t *gamma_table, size_t gamma_size,
		const pixman_region32_t *damage) {
	struct wlr_box dst = { 0, 0, out_width, out_height };
	glDisable(GL_BLEND);
	draw_texture_internal(cr, texture, NULL, &dst,
		WL_OUTPUT_TRANSFORM_NORMAL, WL_OUTPUT_TRANSFORM_NORMAL,
		out_width, out_height, 1.0f, WLR_SCALE_FILTER_NEAREST,
		NULL, NULL, 0, gamma_table, gamma_size, damage);
	glEnable(GL_BLEND);
}
