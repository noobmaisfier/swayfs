#ifndef SWAY_RENDER_CORNER_RENDERER_H
#define SWAY_RENDER_CORNER_RENDERER_H

#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <pixman.h>
#include <stdbool.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/box.h>
#include "sway/scene/corner_radii.h"

struct sway_corner_renderer;
struct wlr_drm_syncobj_timeline;

struct sway_corner_renderer *sway_corner_renderer_for_wlr(struct wlr_renderer *renderer);

void sway_corner_renderer_destroy(struct sway_corner_renderer *cr);

bool sway_corner_renderer_begin(struct sway_corner_renderer *cr,
		struct wlr_buffer *dst_buffer, const pixman_region32_t *damage);

void sway_corner_renderer_end(struct sway_corner_renderer *cr);

void sway_corner_renderer_end_flush(struct sway_corner_renderer *cr);

bool sway_corner_renderer_end_sync(struct sway_corner_renderer *cr,
		struct wlr_drm_syncobj_timeline *signal_timeline, uint64_t signal_point);

void sway_corner_renderer_draw_gamma_blit(struct sway_corner_renderer *cr,
		struct wlr_texture *texture,
		int out_width, int out_height,
		const uint16_t *gamma_table, size_t gamma_size,
		const pixman_region32_t *damage);

void sway_corner_renderer_clear_damage(struct sway_corner_renderer *cr,
		const pixman_region32_t *damage, float r, float g, float b, float a);

void sway_corner_renderer_draw_rect(struct sway_corner_renderer *cr,
		const struct wlr_box *dst_box,
		enum wl_output_transform output_transform,
		int out_width, int out_height,
		const float color[static 4],
		const struct fx_corner_radii *corners_scaled,
		const struct clipped_region *clip_scaled,
		const pixman_region32_t *damage);

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
		const pixman_region32_t *damage);

#endif
