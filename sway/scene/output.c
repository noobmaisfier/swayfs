#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/render/allocator.h>
#include <wlr/render/color.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/gles2.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/region.h>
#include <wlr/util/transform.h>
#include "log.h"
#include "sway/render/corner_renderer.h"
#include "scene_internal.h"

struct render_item {
	struct wlr_scene_node *node;
	struct wlr_box dst_box; // in output-local scaled coords (logical * scale)
	struct fx_corner_radii corners_scaled;
	struct clipped_region clip_scaled;
};

struct render_collect_data {
	struct wlr_scene_output *scene_output;
	int out_lx, out_ly;
	int eff_w, eff_h;
	float scale;
	struct wl_array *items;
	bool has_rounded_fx;
};

static int scale_length(int length, int offset, float scale) {
	return (int)lroundf((offset + length) * scale) - (int)lroundf(offset * scale);
}

static void scale_box(struct wlr_box *dst, const struct wlr_box *src, float scale) {
	dst->x = (int)lroundf(src->x * scale);
	dst->y = (int)lroundf(src->y * scale);
	dst->width = scale_length(src->width, src->x, scale);
	dst->height = scale_length(src->height, src->y, scale);
}

static struct fx_corner_radii scale_corners(struct fx_corner_radii c, float scale,
		int w, int h) {
	int max_r = (w < h ? w : h) / 2;
	if (max_r <= 0) {
		return corner_radii_none();
	}
	int tl = (int)lroundf(c.top_left * scale);
	int tr = (int)lroundf(c.top_right * scale);
	int br = (int)lroundf(c.bottom_right * scale);
	int bl = (int)lroundf(c.bottom_left * scale);
	if (tl > max_r) tl = max_r;
	if (tr > max_r) tr = max_r;
	if (br > max_r) br = max_r;
	if (bl > max_r) bl = max_r;
	return corner_radii_new(tl, tr, br, bl);
}

static void scene_subtract_corner_squares(pixman_region32_t *opaque,
		int w, int h, const struct fx_corner_radii *corners) {
	if (fx_corner_radii_is_empty(corners)) {
		return;
	}
	pixman_region32_t sub;
	pixman_region32_init(&sub);
	if (corners->top_left > 0) {
		pixman_region32_union_rect(&sub, &sub, 0, 0,
			corners->top_left, corners->top_left);
	}
	if (corners->top_right > 0) {
		pixman_region32_union_rect(&sub, &sub, w - corners->top_right, 0,
			corners->top_right, corners->top_right);
	}
	if (corners->bottom_right > 0) {
		pixman_region32_union_rect(&sub, &sub, w - corners->bottom_right,
			h - corners->bottom_right, corners->bottom_right, corners->bottom_right);
	}
	if (corners->bottom_left > 0) {
		pixman_region32_union_rect(&sub, &sub, 0, h - corners->bottom_left,
			corners->bottom_left, corners->bottom_left);
	}
	pixman_region32_subtract(opaque, opaque, &sub);
	pixman_region32_fini(&sub);
}

static void collect_nodes_rec(struct wlr_scene_node *node, int lx, int ly,
		struct render_collect_data *ctx) {
	if (!node->enabled) {
		return;
	}

	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			collect_nodes_rec(child, lx + child->x, ly + child->y, ctx);
		}
		return;
	}

	int w = 0, h = 0;
	scene_node_get_size(node, &w, &h);
	if (w <= 0 || h <= 0) {
		return;
	}

	struct wlr_box node_box = { lx, ly, w, h };
	struct wlr_box out_box = { ctx->out_lx, ctx->out_ly, ctx->eff_w, ctx->eff_h };
	struct wlr_box inter;
	if (!wlr_box_intersection(&inter, &node_box, &out_box)) {
		return;
	}

	struct wlr_box rel_box = {
		.x = lx - ctx->out_lx,
		.y = ly - ctx->out_ly,
		.width = w,
		.height = h,
	};

	struct render_item item = {
		.node = node,
	};
	scale_box(&item.dst_box, &rel_box, ctx->scale);
	if (item.dst_box.width <= 0 || item.dst_box.height <= 0) {
		return;
	}

	if (node->type == WLR_SCENE_NODE_RECT) {
		struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
		if (rect->color[3] <= 0.0f) {
			return;
		}
		item.corners_scaled = scale_corners(rect->corners, ctx->scale,
			item.dst_box.width, item.dst_box.height);
		if (rect->clipped_region.area.width > 0 && rect->clipped_region.area.height > 0) {
			scale_box(&item.clip_scaled.area, &rect->clipped_region.area, ctx->scale);
			item.clip_scaled.corners = scale_corners(rect->clipped_region.corners,
				ctx->scale, item.clip_scaled.area.width, item.clip_scaled.area.height);
			ctx->has_rounded_fx = true;
		}
		if (!fx_corner_radii_is_empty(&item.corners_scaled)) {
			ctx->has_rounded_fx = true;
		}
	} else if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
		if (!buffer->buffer || buffer->opacity <= 0.0f) {
			return;
		}
		item.corners_scaled = scale_corners(buffer->corners, ctx->scale,
			item.dst_box.width, item.dst_box.height);
		if (!fx_corner_radii_is_empty(&item.corners_scaled)) {
			ctx->has_rounded_fx = true;
		}
	}

	struct render_item *slot = wl_array_add(ctx->items, sizeof(*slot));
	if (slot) {
		*slot = item;
	}
}

void scene_damage_outputs(struct wlr_scene *scene, const pixman_region32_t *damage) {
	if (!scene || pixman_region32_empty(damage)) {
		return;
	}

	struct wlr_scene_output *so;
	wl_list_for_each(so, &scene->outputs, link) {
		if (!so->output->enabled) {
			continue;
		}
		int eff_w = 0, eff_h = 0;
		wlr_output_effective_resolution(so->output, &eff_w, &eff_h);

		pixman_region32_t output_damage;
		pixman_region32_init(&output_damage);
		pixman_region32_copy(&output_damage, damage);
		pixman_region32_translate(&output_damage, -so->x, -so->y);
		pixman_region32_intersect_rect(&output_damage, &output_damage,
			0, 0, eff_w, eff_h);

		if (!pixman_region32_empty(&output_damage)) {
			wlr_region_scale(&output_damage, &output_damage, so->output->scale);
			int scaled_w = (int)lroundf(eff_w * so->output->scale);
			int scaled_h = (int)lroundf(eff_h * so->output->scale);
			wlr_region_transform(&output_damage, &output_damage,
				wlr_output_transform_invert(so->output->transform),
				scaled_w, scaled_h);
			wlr_damage_ring_add(&so->damage_ring, &output_damage);
			wlr_output_schedule_frame(so->output);
		}
		pixman_region32_fini(&output_damage);
	}
}

static void handle_output_commit(struct wl_listener *listener, void *data) {
	struct wlr_scene_output *so = wl_container_of(
		listener, so, WLR_PRIVATE.output_commit);
	const struct wlr_output_event_commit *event = data;

	if (event->state->committed & (WLR_OUTPUT_STATE_MODE |
			WLR_OUTPUT_STATE_TRANSFORM | WLR_OUTPUT_STATE_SCALE |
			WLR_OUTPUT_STATE_ENABLED)) {
		wlr_damage_ring_add_whole(&so->damage_ring);
		scene_node_update(&so->scene->tree.node, NULL);
	}

	if (!pixman_region32_empty(&so->WLR_PRIVATE.pending_commit_damage)) {
		pixman_region32_clear(&so->WLR_PRIVATE.pending_commit_damage);
	}
}

static void handle_output_damage(struct wl_listener *listener, void *data) {
	struct wlr_scene_output *so = wl_container_of(
		listener, so, WLR_PRIVATE.output_damage);
	const struct wlr_output_event_damage *event = data;
	if (!pixman_region32_empty(event->damage)) {
		wlr_damage_ring_add(&so->damage_ring, event->damage);
		wlr_output_schedule_frame(so->output);
	}
}

static void handle_output_needs_frame(struct wl_listener *listener, void *data) {
	struct wlr_scene_output *so = wl_container_of(
		listener, so, WLR_PRIVATE.output_needs_frame);
	wlr_output_schedule_frame(so->output);
}

struct wlr_scene_output *wlr_scene_output_create(struct wlr_scene *scene,
		struct wlr_output *output) {
	assert(scene && output);
	struct wlr_scene_output *so = calloc(1, sizeof(*so));
	if (!so) {
		return NULL;
	}

	uint64_t used = 0;
	struct wlr_scene_output *other;
	wl_list_for_each(other, &scene->outputs, link) {
		if (other->WLR_PRIVATE.index < 64) {
			used |= (UINT64_C(1) << other->WLR_PRIVATE.index);
		}
	}
	uint8_t idx = 0;
	while (idx < 64 && (used & (UINT64_C(1) << idx))) {
		idx++;
	}

	so->scene = scene;
	so->output = output;
	so->WLR_PRIVATE.index = idx;
	wl_signal_init(&so->events.destroy);
	wlr_damage_ring_init(&so->damage_ring);
	pixman_region32_init(&so->WLR_PRIVATE.pending_commit_damage);
	wl_array_init(&so->WLR_PRIVATE.render_list);

	wl_list_insert(scene->outputs.prev, &so->link);

	so->WLR_PRIVATE.output_commit.notify = handle_output_commit;
	wl_signal_add(&output->events.commit, &so->WLR_PRIVATE.output_commit);
	so->WLR_PRIVATE.output_damage.notify = handle_output_damage;
	wl_signal_add(&output->events.damage, &so->WLR_PRIVATE.output_damage);
	so->WLR_PRIVATE.output_needs_frame.notify = handle_output_needs_frame;
	wl_signal_add(&output->events.needs_frame, &so->WLR_PRIVATE.output_needs_frame);

	wlr_damage_ring_add_whole(&so->damage_ring);
	scene_node_update(&scene->tree.node, NULL);
	return so;
}

void wlr_scene_output_destroy(struct wlr_scene_output *so) {
	if (!so) {
		return;
	}
	wl_signal_emit_mutable(&so->events.destroy, NULL);

	wl_list_remove(&so->WLR_PRIVATE.output_commit.link);
	wl_list_remove(&so->WLR_PRIVATE.output_damage.link);
	wl_list_remove(&so->WLR_PRIVATE.output_needs_frame.link);
	wl_list_remove(&so->link);

	if (so->ct_swapchain) {
		wlr_swapchain_destroy(so->ct_swapchain);
	}
	if (so->WLR_PRIVATE.gamma_lut_color_transform) {
		wlr_color_transform_unref(so->WLR_PRIVATE.gamma_lut_color_transform);
	}
	if (so->WLR_PRIVATE.prev_gamma_lut_color_transform) {
		wlr_color_transform_unref(so->WLR_PRIVATE.prev_gamma_lut_color_transform);
	}
	if (so->WLR_PRIVATE.prev_supplied_color_transform) {
		wlr_color_transform_unref(so->WLR_PRIVATE.prev_supplied_color_transform);
	}
	if (so->WLR_PRIVATE.combined_color_transform) {
		wlr_color_transform_unref(so->WLR_PRIVATE.combined_color_transform);
	}

	wlr_damage_ring_finish(&so->damage_ring);
	pixman_region32_fini(&so->WLR_PRIVATE.pending_commit_damage);
	wl_array_release(&so->WLR_PRIVATE.render_list);
	free(so);
}

void wlr_scene_output_set_position(struct wlr_scene_output *so, int lx, int ly) {
	if (so->x == lx && so->y == ly) {
		return;
	}
	so->x = lx;
	so->y = ly;
	wlr_damage_ring_add_whole(&so->damage_ring);
	scene_node_update(&so->scene->tree.node, NULL);
}

struct wlr_scene_output *wlr_scene_get_scene_output(struct wlr_scene *scene,
		struct wlr_output *output) {
	struct wlr_scene_output *so;
	wl_list_for_each(so, &scene->outputs, link) {
		if (so->output == output) {
			return so;
		}
	}
	return NULL;
}

bool wlr_scene_output_needs_frame(struct wlr_scene_output *so) {
	return so->output->needs_frame ||
		!pixman_region32_empty(&so->damage_ring.current) ||
		so->WLR_PRIVATE.gamma_lut_changed;
}

static void handle_buffer_renderer_destroy(struct wl_listener *listener, void *data) {
	struct wlr_scene_buffer *buffer = wl_container_of(
		listener, buffer, WLR_PRIVATE.renderer_destroy);
	wl_list_remove(&buffer->WLR_PRIVATE.renderer_destroy.link);
	wl_list_init(&buffer->WLR_PRIVATE.renderer_destroy.link);
	if (buffer->WLR_PRIVATE.texture) {
		wlr_texture_destroy(buffer->WLR_PRIVATE.texture);
		buffer->WLR_PRIVATE.texture = NULL;
	}
}

static struct wlr_texture *scene_buffer_get_texture(struct wlr_scene_buffer *buffer,
		struct wlr_renderer *renderer) {
	if (!buffer->buffer || buffer->WLR_PRIVATE.is_single_pixel_buffer) {
		return NULL;
	}
	if (buffer->WLR_PRIVATE.texture &&
			buffer->WLR_PRIVATE.texture->renderer == renderer) {
		return buffer->WLR_PRIVATE.texture;
	}
	if (buffer->WLR_PRIVATE.texture) {
		wl_list_remove(&buffer->WLR_PRIVATE.renderer_destroy.link);
		wl_list_init(&buffer->WLR_PRIVATE.renderer_destroy.link);
		wlr_texture_destroy(buffer->WLR_PRIVATE.texture);
		buffer->WLR_PRIVATE.texture = NULL;
	}
	struct wlr_texture *tex = wlr_texture_from_buffer(renderer, buffer->buffer);
	if (!tex) {
		return NULL;
	}
	buffer->WLR_PRIVATE.texture = tex;
	buffer->WLR_PRIVATE.renderer_destroy.notify = handle_buffer_renderer_destroy;
	wl_signal_add(&renderer->events.destroy, &buffer->WLR_PRIVATE.renderer_destroy);
	return tex;
}

static void render_items_wlr_pass(struct wlr_render_pass *pass,
		struct wlr_renderer *renderer, struct wl_array *items,
		enum wl_output_transform output_transform,
		int out_buf_w, int out_buf_h,
		const pixman_region32_t *damage) {
	// Clear damaged background to opaque black
	int n_dmg = 0;
	const pixman_box32_t *dmg_rects = pixman_region32_rectangles(damage, &n_dmg);
	for (int i = 0; i < n_dmg; i++) {
		struct wlr_box box = {
			.x = dmg_rects[i].x1,
			.y = dmg_rects[i].y1,
			.width = dmg_rects[i].x2 - dmg_rects[i].x1,
			.height = dmg_rects[i].y2 - dmg_rects[i].y1,
		};
		if (box.width <= 0 || box.height <= 0) continue;
		wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
			.box = box,
			.color = { 0, 0, 0, 1 },
			.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
		});
	}

	int eff_w = out_buf_w;
	int eff_h = out_buf_h;
	if (output_transform & WL_OUTPUT_TRANSFORM_90) {
		eff_w = out_buf_h;
		eff_h = out_buf_w;
	}

	struct render_item *item;
	wl_array_for_each(item, items) {
		struct wlr_box dst = item->dst_box;
		wlr_box_transform(&dst, &dst,
			wlr_output_transform_invert(output_transform), eff_w, eff_h);

		pixman_region32_t clip;
		pixman_region32_init(&clip);
		pixman_region32_intersect_rect(&clip, damage,
			dst.x, dst.y, dst.width, dst.height);
		if (pixman_region32_empty(&clip)) {
			pixman_region32_fini(&clip);
			continue;
		}

		if (item->node->type == WLR_SCENE_NODE_RECT) {
			struct wlr_scene_rect *rect = wlr_scene_rect_from_node(item->node);
			wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
				.box = dst,
				.color = { rect->color[0], rect->color[1], rect->color[2], rect->color[3] },
				.clip = &clip,
				.blend_mode = rect->color[3] >= 1.0f
					? WLR_RENDER_BLEND_MODE_NONE : WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
			});
		} else if (item->node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(item->node);
			if (buffer->WLR_PRIVATE.is_single_pixel_buffer) {
				float a = (float)buffer->WLR_PRIVATE.single_pixel_buffer_color[3] / UINT32_MAX * buffer->opacity;
				float r = (float)buffer->WLR_PRIVATE.single_pixel_buffer_color[0] / UINT32_MAX * a;
				float g = (float)buffer->WLR_PRIVATE.single_pixel_buffer_color[1] / UINT32_MAX * a;
				float b = (float)buffer->WLR_PRIVATE.single_pixel_buffer_color[2] / UINT32_MAX * a;
				wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
					.box = dst,
					.color = { r, g, b, a },
					.clip = &clip,
					.blend_mode = a >= 1.0f
						? WLR_RENDER_BLEND_MODE_NONE : WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
				});
			} else {
				struct wlr_texture *tex = scene_buffer_get_texture(buffer, renderer);
				if (tex) {
					enum wl_output_transform tr = wlr_output_transform_compose(
						wlr_output_transform_invert(buffer->transform), output_transform);
					wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
						.texture = tex,
						.src_box = buffer->src_box,
						.dst_box = dst,
						.alpha = &buffer->opacity,
						.clip = &clip,
						.transform = tr,
						.filter_mode = buffer->filter_mode,
						.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
						.transfer_function = buffer->transfer_function,
						.color_encoding = buffer->color_encoding,
						.color_range = buffer->color_range,
						.wait_timeline = buffer->WLR_PRIVATE.wait_timeline,
						.wait_point = buffer->WLR_PRIVATE.wait_point,
					});
				}
			}
		}
		pixman_region32_fini(&clip);
	}
}

static void render_items_gles2_rounded(struct sway_corner_renderer *cr,
		struct wlr_renderer *renderer, struct wl_array *items,
		enum wl_output_transform output_transform,
		int out_buf_w, int out_buf_h,
		const pixman_region32_t *damage) {
	sway_corner_renderer_clear_damage(cr, damage, 0.0f, 0.0f, 0.0f, 1.0f);

	int eff_w = out_buf_w;
	int eff_h = out_buf_h;
	if (output_transform & WL_OUTPUT_TRANSFORM_90) {
		eff_w = out_buf_h;
		eff_h = out_buf_w;
	}

	struct render_item *item;
	wl_array_for_each(item, items) {
		struct wlr_box tr_dst = item->dst_box;
		wlr_box_transform(&tr_dst, &tr_dst,
			wlr_output_transform_invert(output_transform), eff_w, eff_h);

		pixman_region32_t item_dmg;
		pixman_region32_init(&item_dmg);
		pixman_region32_intersect_rect(&item_dmg, damage,
			tr_dst.x, tr_dst.y, tr_dst.width, tr_dst.height);
		if (pixman_region32_empty(&item_dmg)) {
			pixman_region32_fini(&item_dmg);
			continue;
		}

		if (item->node->type == WLR_SCENE_NODE_RECT) {
			struct wlr_scene_rect *rect = wlr_scene_rect_from_node(item->node);
			sway_corner_renderer_draw_rect(cr, &item->dst_box,
				output_transform, out_buf_w, out_buf_h,
				rect->color, &item->corners_scaled, &item->clip_scaled, &item_dmg);
		} else if (item->node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(item->node);
			if (buffer->WLR_PRIVATE.is_single_pixel_buffer) {
				float a = (float)buffer->WLR_PRIVATE.single_pixel_buffer_color[3] / UINT32_MAX * buffer->opacity;
				float color[4] = {
					(float)buffer->WLR_PRIVATE.single_pixel_buffer_color[0] / UINT32_MAX * a,
					(float)buffer->WLR_PRIVATE.single_pixel_buffer_color[1] / UINT32_MAX * a,
					(float)buffer->WLR_PRIVATE.single_pixel_buffer_color[2] / UINT32_MAX * a,
					a,
				};
				sway_corner_renderer_draw_rect(cr, &item->dst_box,
					output_transform, out_buf_w, out_buf_h,
					color, &item->corners_scaled, NULL, &item_dmg);
			} else {
				struct wlr_texture *tex = scene_buffer_get_texture(buffer, renderer);
				if (tex) {
					sway_corner_renderer_draw_texture(cr, tex,
						&buffer->src_box, &item->dst_box,
						buffer->transform, output_transform,
						out_buf_w, out_buf_h,
						buffer->opacity, buffer->filter_mode,
						&item->corners_scaled,
						buffer->WLR_PRIVATE.wait_timeline,
						buffer->WLR_PRIVATE.wait_point,
						&item_dmg);
				}
			}
		}
		pixman_region32_fini(&item_dmg);
	}
}

static struct wlr_color_transform *update_output_color_transform(
		struct wlr_scene_output *so, struct wlr_color_transform *supplied) {
	if (so->WLR_PRIVATE.gamma_lut_changed) {
		so->WLR_PRIVATE.gamma_lut_changed = false;
		if (so->WLR_PRIVATE.gamma_lut_color_transform) {
			wlr_color_transform_unref(so->WLR_PRIVATE.gamma_lut_color_transform);
			so->WLR_PRIVATE.gamma_lut_color_transform = NULL;
		}
		if (so->WLR_PRIVATE.gamma_lut) {
			so->WLR_PRIVATE.gamma_lut_color_transform =
				wlr_gamma_control_v1_get_color_transform(so->WLR_PRIVATE.gamma_lut);
		}
	}

	struct wlr_color_transform *gamma = so->WLR_PRIVATE.gamma_lut_color_transform;
	if (!gamma) {
		return supplied;
	}
	if (!supplied) {
		return gamma;
	}

	if (so->WLR_PRIVATE.prev_gamma_lut_color_transform != gamma ||
			so->WLR_PRIVATE.prev_supplied_color_transform != supplied) {
		if (so->WLR_PRIVATE.combined_color_transform) {
			wlr_color_transform_unref(so->WLR_PRIVATE.combined_color_transform);
			so->WLR_PRIVATE.combined_color_transform = NULL;
		}
		struct wlr_color_transform *pipeline[2] = { supplied, gamma };
		so->WLR_PRIVATE.combined_color_transform =
			wlr_color_transform_init_pipeline(pipeline, 2);
		so->WLR_PRIVATE.prev_gamma_lut_color_transform = gamma;
		so->WLR_PRIVATE.prev_supplied_color_transform = supplied;
	}
	return so->WLR_PRIVATE.combined_color_transform;
}

bool wlr_scene_output_build_state(struct wlr_scene_output *so,
		struct wlr_output_state *state, const struct wlr_scene_output_state_options *options) {
	struct wlr_output *output = so->output;
	struct wlr_renderer *renderer = output->renderer;
	if (!renderer) {
		return false;
	}

	struct wlr_scene_output_state_options default_opts = {0};
	if (!options) {
		options = &default_opts;
	}

	// Determine target geometry from pending state or current output
	int buf_w = output->width;
	int buf_h = output->height;
	if (state->committed & WLR_OUTPUT_STATE_MODE) {
		if (state->mode_type == WLR_OUTPUT_STATE_MODE_FIXED && state->mode) {
			buf_w = state->mode->width;
			buf_h = state->mode->height;
		} else if (state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM) {
			buf_w = state->custom_mode.width;
			buf_h = state->custom_mode.height;
		}
	}
	enum wl_output_transform transform = (state->committed & WLR_OUTPUT_STATE_TRANSFORM)
		? state->transform : output->transform;
	float scale = (state->committed & WLR_OUTPUT_STATE_SCALE)
		? state->scale : output->scale;

	int tr_w = (transform & WL_OUTPUT_TRANSFORM_90) ? buf_h : buf_w;
	int tr_h = (transform & WL_OUTPUT_TRANSFORM_90) ? buf_w : buf_h;
	int eff_w = (int)ceilf(tr_w / scale);
	int eff_h = (int)ceilf(tr_h / scale);

	struct wlr_color_transform *color_transform =
		update_output_color_transform(so, options->color_transform);

	so->WLR_PRIVATE.render_list.size = 0;
	struct render_collect_data collect = {
		.scene_output = so,
		.out_lx = so->x,
		.out_ly = so->y,
		.eff_w = eff_w,
		.eff_h = eff_h,
		.scale = scale,
		.items = &so->WLR_PRIVATE.render_list,
		.has_rounded_fx = false,
	};
	collect_nodes_rec(&so->scene->tree.node, so->scene->tree.node.x,
		so->scene->tree.node.y, &collect);

	// Cull completely occluded items from top to bottom (accounting for rounded corners!)
	pixman_region32_t opaque;
	pixman_region32_init(&opaque);
	struct render_item *items = so->WLR_PRIVATE.render_list.data;
	size_t count = so->WLR_PRIVATE.render_list.size / sizeof(*items);
	for (size_t i = count; i-- > 0;) {
		struct render_item *it = &items[i];
		pixman_region32_t item_vis;
		pixman_region32_init_rect(&item_vis, it->dst_box.x, it->dst_box.y,
			it->dst_box.width, it->dst_box.height);
		pixman_region32_subtract(&item_vis, &item_vis, &opaque);
		if (pixman_region32_empty(&item_vis)) {
			it->node = NULL; // Culled
			pixman_region32_fini(&item_vis);
			continue;
		}
		pixman_region32_fini(&item_vis);

		if (it->node->type == WLR_SCENE_NODE_RECT) {
			struct wlr_scene_rect *rect = wlr_scene_rect_from_node(it->node);
			if (rect->color[3] >= 1.0f &&
					(it->clip_scaled.area.width <= 0 || it->clip_scaled.area.height <= 0)) {
				pixman_region32_t r_op;
				pixman_region32_init_rect(&r_op, 0, 0,
					it->dst_box.width, it->dst_box.height);
				scene_subtract_corner_squares(&r_op,
					it->dst_box.width, it->dst_box.height, &it->corners_scaled);
				pixman_region32_translate(&r_op, it->dst_box.x, it->dst_box.y);
				pixman_region32_union(&opaque, &opaque, &r_op);
				pixman_region32_fini(&r_op);
			}
		} else if (it->node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(it->node);
			if (buffer->opacity >= 1.0f) {
				pixman_region32_t b_op;
				pixman_region32_init(&b_op);
				if (buffer->WLR_PRIVATE.buffer_is_opaque) {
					int bw = 0, bh = 0;
					scene_buffer_get_dest_size(buffer, &bw, &bh);
					pixman_region32_union_rect(&b_op, &b_op, 0, 0, bw, bh);
				} else if (!pixman_region32_empty(&buffer->opaque_region)) {
					pixman_region32_copy(&b_op, &buffer->opaque_region);
				}
				if (!pixman_region32_empty(&b_op)) {
					// Scale using floor-like conservative intersection
					wlr_region_scale(&b_op, &b_op, scale);
					pixman_region32_intersect_rect(&b_op, &b_op, 0, 0,
						it->dst_box.width, it->dst_box.height);
					scene_subtract_corner_squares(&b_op,
						it->dst_box.width, it->dst_box.height, &it->corners_scaled);
					pixman_region32_translate(&b_op, it->dst_box.x, it->dst_box.y);
					pixman_region32_union(&opaque, &opaque, &b_op);
				}
				pixman_region32_fini(&b_op);
			}
		}
	}
	pixman_region32_fini(&opaque);

	// Compact culled entries
	size_t write_idx = 0;
	for (size_t i = 0; i < count; i++) {
		if (items[i].node) {
			items[write_idx++] = items[i];
		}
	}
	so->WLR_PRIVATE.render_list.size = write_idx * sizeof(*items);

	// Try direct scanout when single fullscreen unrounded opaque buffer covers output
	if (so->scene->WLR_PRIVATE.direct_scanout && write_idx == 1 && !collect.has_rounded_fx &&
			!color_transform && items[0].node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *buf = wlr_scene_buffer_from_node(items[0].node);
		if (buf->buffer && !buf->WLR_PRIVATE.is_single_pixel_buffer &&
				buf->opacity >= 1.0f && wlr_fbox_empty(&buf->src_box) &&
				buf->transform == transform &&
				items[0].dst_box.x == 0 && items[0].dst_box.y == 0 &&
				items[0].dst_box.width == tr_w && items[0].dst_box.height == tr_h &&
				buf->buffer->width == buf_w && buf->buffer->height == buf_h) {
			wlr_output_state_set_buffer(state, buf->buffer);
			if (buf->WLR_PRIVATE.wait_timeline) {
				wlr_output_state_set_wait_timeline(state,
					buf->WLR_PRIVATE.wait_timeline, buf->WLR_PRIVATE.wait_point);
			}
			pixman_region32_t dmg;
			pixman_region32_init(&dmg);
			wlr_damage_ring_rotate_buffer(&so->damage_ring, buf->buffer, &dmg);
			wlr_output_state_set_damage(state, &dmg);
			pixman_region32_fini(&dmg);

			struct wlr_scene_output_sample_event sample_ev = {
				.output = so,
				.direct_scanout = true,
			};
			wl_signal_emit_mutable(&buf->events.output_sample, &sample_ev);
			return true;
		}
	}

	struct wlr_swapchain *swapchain = options->swapchain;
	if (!swapchain) {
		if (!wlr_output_configure_primary_swapchain(output, state, &output->swapchain)) {
			return false;
		}
		swapchain = output->swapchain;
	}

	struct wlr_buffer *dst_buffer = wlr_swapchain_acquire(swapchain);
	if (!dst_buffer) {
		return false;
	}

	pixman_region32_t buffer_damage;
	pixman_region32_init(&buffer_damage);
	wlr_damage_ring_rotate_buffer(&so->damage_ring, dst_buffer, &buffer_damage);

	struct sway_corner_renderer *cr = collect.has_rounded_fx
		? sway_corner_renderer_for_wlr(renderer) : NULL;

	bool ok = true;
	if (cr && !color_transform) {
		if (!sway_corner_renderer_begin(cr, dst_buffer, &buffer_damage)) {
			ok = false;
		} else {
			render_items_gles2_rounded(cr, renderer, &so->WLR_PRIVATE.render_list,
				transform, dst_buffer->width, dst_buffer->height, &buffer_damage);
			sway_corner_renderer_end(cr);
		}
	} else if (cr && color_transform) {
		// Render rounded scene to intermediate swapchain buffer, then blit with color_transform
		if (!so->ct_swapchain ||
				so->ct_swapchain->width != dst_buffer->width ||
				so->ct_swapchain->height != dst_buffer->height) {
			if (so->ct_swapchain) {
				wlr_swapchain_destroy(so->ct_swapchain);
			}
			so->ct_swapchain = wlr_swapchain_create(output->allocator,
				dst_buffer->width, dst_buffer->height, &swapchain->format);
		}
		struct wlr_buffer *inter_buf = so->ct_swapchain
			? wlr_swapchain_acquire(so->ct_swapchain) : NULL;
		if (!inter_buf || !sway_corner_renderer_begin(cr, inter_buf, &buffer_damage)) {
			if (inter_buf) wlr_buffer_unlock(inter_buf);
			ok = false;
		} else {
			pixman_region32_t full;
			pixman_region32_init_rect(&full, 0, 0, inter_buf->width, inter_buf->height);
			render_items_gles2_rounded(cr, renderer, &so->WLR_PRIVATE.render_list,
				transform, inter_buf->width, inter_buf->height, &full);
			pixman_region32_fini(&full);
			sway_corner_renderer_end(cr);

			struct wlr_texture *inter_tex = wlr_texture_from_buffer(renderer, inter_buf);
			struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(renderer,
				dst_buffer, &(struct wlr_buffer_pass_options){
					.timer = options->timer ? options->timer->render_timer : NULL,
					.color_transform = color_transform,
				});
			if (pass && inter_tex) {
				wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
					.texture = inter_tex,
					.dst_box = { 0, 0, dst_buffer->width, dst_buffer->height },
					.clip = &buffer_damage,
					.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
				});
				ok = wlr_render_pass_submit(pass);
			} else {
				ok = false;
			}
			if (inter_tex) wlr_texture_destroy(inter_tex);
			wlr_buffer_unlock(inter_buf);
		}
	} else {
		struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(renderer,
			dst_buffer, &(struct wlr_buffer_pass_options){
				.timer = options->timer ? options->timer->render_timer : NULL,
				.color_transform = color_transform,
			});
		if (!pass) {
			ok = false;
		} else {
			render_items_wlr_pass(pass, renderer, &so->WLR_PRIVATE.render_list,
				transform, dst_buffer->width, dst_buffer->height, &buffer_damage);
			ok = wlr_render_pass_submit(pass);
		}
	}

	if (!ok) {
		wlr_damage_ring_add(&so->damage_ring, &buffer_damage);
		pixman_region32_fini(&buffer_damage);
		wlr_buffer_unlock(dst_buffer);
		return false;
	}

	wlr_output_state_set_buffer(state, dst_buffer);
	wlr_output_state_set_damage(state, &buffer_damage);
	pixman_region32_fini(&buffer_damage);
	wlr_buffer_unlock(dst_buffer);

	for (size_t i = 0; i < write_idx; i++) {
		if (items[i].node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_buffer *buf = wlr_scene_buffer_from_node(items[i].node);
			struct wlr_scene_output_sample_event sample_ev = {
				.output = so,
				.direct_scanout = false,
			};
			wl_signal_emit_mutable(&buf->events.output_sample, &sample_ev);
		}
	}

	return true;
}

bool wlr_scene_output_commit(struct wlr_scene_output *so,
		const struct wlr_scene_output_state_options *options) {
	if (!wlr_scene_output_needs_frame(so)) {
		return true;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (!wlr_scene_output_build_state(so, &state, options)) {
		wlr_output_state_finish(&state);
		return false;
	}
	bool ok = wlr_output_commit_state(so->output, &state);
	wlr_output_state_finish(&state);
	return ok;
}

int64_t wlr_scene_timer_get_duration_ns(struct wlr_scene_timer *timer) {
	if (!timer || !timer->render_timer) {
		return -1;
	}
	int ns = wlr_render_timer_get_duration_ns(timer->render_timer);
	if (ns < 0) {
		return -1;
	}
	return timer->pre_render_duration + ns;
}

void wlr_scene_timer_finish(struct wlr_scene_timer *timer) {
	if (timer && timer->render_timer) {
		wlr_render_timer_destroy(timer->render_timer);
		timer->render_timer = NULL;
	}
}

struct for_each_buffer_ctx {
	struct wlr_box output_box;
	wlr_scene_buffer_iterator_func_t iterator;
	void *user_data;
};

static void output_for_each_buffer_iter(struct wlr_scene_buffer *buffer,
		int sx, int sy, void *data) {
	struct for_each_buffer_ctx *ctx = data;
	int w = 0, h = 0;
	scene_buffer_get_dest_size(buffer, &w, &h);
	if (w <= 0 || h <= 0) {
		return;
	}
	struct wlr_box box = { sx, sy, w, h };
	struct wlr_box inter;
	if (wlr_box_intersection(&inter, &box, &ctx->output_box)) {
		ctx->iterator(buffer, sx, sy, ctx->user_data);
	}
}

void wlr_scene_output_for_each_buffer(struct wlr_scene_output *so,
		wlr_scene_buffer_iterator_func_t iterator, void *user_data) {
	int eff_w = 0, eff_h = 0;
	wlr_output_effective_resolution(so->output, &eff_w, &eff_h);
	struct for_each_buffer_ctx ctx = {
		.output_box = { so->x, so->y, eff_w, eff_h },
		.iterator = iterator,
		.user_data = user_data,
	};
	wlr_scene_node_for_each_buffer(&so->scene->tree.node,
		output_for_each_buffer_iter, &ctx);
}

static void send_frame_done_cb(struct wlr_scene_buffer *buffer,
		int sx, int sy, void *data) {
	struct wlr_scene_frame_done_event *ev = data;
	if (buffer->primary_output == ev->output) {
		wlr_scene_buffer_send_frame_done(buffer, ev);
	}
}

void wlr_scene_output_send_frame_done(struct wlr_scene_output *so,
		struct timespec *now) {
	struct wlr_scene_frame_done_event ev = {
		.output = so,
		.when = *now,
	};
	wlr_scene_output_for_each_buffer(so, send_frame_done_cb, &ev);
}

struct wlr_scene_output_layout {
	struct wlr_output_layout *layout;
	struct wlr_scene *scene;
	struct wl_list outputs;
	struct wl_listener layout_change;
	struct wl_listener layout_destroy;
	struct wl_listener scene_destroy;
};

struct scene_ol_output {
	struct wlr_output_layout_output *lo;
	struct wlr_scene_output *so;
	struct wl_list link;
	struct wl_listener lo_destroy;
	struct wl_listener so_destroy;
};

static void scene_ol_output_destroy(struct scene_ol_output *solo) {
	wl_list_remove(&solo->lo_destroy.link);
	wl_list_remove(&solo->so_destroy.link);
	wl_list_remove(&solo->link);
	free(solo);
}

static void handle_solo_destroy(struct wl_listener *listener, void *data) {
	struct scene_ol_output *solo = wl_container_of(listener, solo, lo_destroy);
	scene_ol_output_destroy(solo);
}

static void handle_solo_so_destroy(struct wl_listener *listener, void *data) {
	struct scene_ol_output *solo = wl_container_of(listener, solo, so_destroy);
	scene_ol_output_destroy(solo);
}

static void handle_ol_change(struct wl_listener *listener, void *data) {
	struct wlr_scene_output_layout *sol = wl_container_of(
		listener, sol, layout_change);
	struct scene_ol_output *solo;
	wl_list_for_each(solo, &sol->outputs, link) {
		wlr_scene_output_set_position(solo->so, solo->lo->x, solo->lo->y);
	}
}

static void scene_ol_destroy(struct wlr_scene_output_layout *sol) {
	struct scene_ol_output *solo, *tmp;
	wl_list_for_each_safe(solo, tmp, &sol->outputs, link) {
		scene_ol_output_destroy(solo);
	}
	wl_list_remove(&sol->layout_change.link);
	wl_list_remove(&sol->layout_destroy.link);
	wl_list_remove(&sol->scene_destroy.link);
	free(sol);
}

static void handle_ol_destroy(struct wl_listener *listener, void *data) {
	struct wlr_scene_output_layout *sol = wl_container_of(
		listener, sol, layout_destroy);
	scene_ol_destroy(sol);
}

static void handle_ol_scene_destroy(struct wl_listener *listener, void *data) {
	struct wlr_scene_output_layout *sol = wl_container_of(
		listener, sol, scene_destroy);
	scene_ol_destroy(sol);
}

struct wlr_scene_output_layout *wlr_scene_attach_output_layout(struct wlr_scene *scene,
		struct wlr_output_layout *output_layout) {
	struct wlr_scene_output_layout *sol = calloc(1, sizeof(*sol));
	if (!sol) {
		return NULL;
	}
	sol->layout = output_layout;
	sol->scene = scene;
	wl_list_init(&sol->outputs);

	sol->layout_change.notify = handle_ol_change;
	wl_signal_add(&output_layout->events.change, &sol->layout_change);
	sol->layout_destroy.notify = handle_ol_destroy;
	wl_signal_add(&output_layout->events.destroy, &sol->layout_destroy);
	sol->scene_destroy.notify = handle_ol_scene_destroy;
	wl_signal_add(&scene->tree.node.events.destroy, &sol->scene_destroy);

	return sol;
}

void wlr_scene_output_layout_add_output(struct wlr_scene_output_layout *sol,
		struct wlr_output_layout_output *lo, struct wlr_scene_output *so) {
	struct scene_ol_output *solo = calloc(1, sizeof(*solo));
	if (!solo) {
		return;
	}
	solo->lo = lo;
	solo->so = so;
	wl_list_insert(&sol->outputs, &solo->link);

	solo->lo_destroy.notify = handle_solo_destroy;
	wl_signal_add(&lo->events.destroy, &solo->lo_destroy);
	solo->so_destroy.notify = handle_solo_so_destroy;
	wl_signal_add(&so->events.destroy, &solo->so_destroy);

	wlr_scene_output_set_position(so, lo->x, lo->y);
}
