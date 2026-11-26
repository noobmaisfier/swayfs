#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/region.h>
#include <wlr/util/transform.h>
#include <wlr/config.h>
#if WLR_HAS_XWAYLAND
#include <wlr/xwayland/xwayland.h>
#endif
#include "scene_internal.h"

static void scene_surface_addon_destroy(struct wlr_addon *addon) {
	struct wlr_scene_surface *scene_surface = wl_container_of(
		addon, scene_surface, WLR_PRIVATE.addon);
	wlr_addon_finish(&scene_surface->WLR_PRIVATE.addon);
	wl_list_remove(&scene_surface->WLR_PRIVATE.outputs_update.link);
	wl_list_remove(&scene_surface->WLR_PRIVATE.output_sample.link);
	wl_list_remove(&scene_surface->WLR_PRIVATE.frame_done.link);
	wl_list_remove(&scene_surface->WLR_PRIVATE.surface_destroy.link);
	wl_list_remove(&scene_surface->WLR_PRIVATE.surface_commit.link);
	free(scene_surface);
}

static const struct wlr_addon_interface scene_surface_addon_impl = {
	.name = "wlr_scene_surface",
	.destroy = scene_surface_addon_destroy,
};

struct wlr_scene_surface *wlr_scene_surface_try_from_buffer(
		struct wlr_scene_buffer *scene_buffer) {
	if (!scene_buffer) {
		return NULL;
	}
	struct wlr_addon *addon = wlr_addon_find(&scene_buffer->node.addons,
		scene_buffer, &scene_surface_addon_impl);
	if (!addon) {
		return NULL;
	}
	return wl_container_of(addon, (struct wlr_scene_surface *)NULL, WLR_PRIVATE.addon);
}

static bool scene_surface_point_accepts_input(struct wlr_scene_buffer *scene_buffer,
		double *sx, double *sy) {
	struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface || !scene_surface->surface) {
		return false;
	}
	*sx += scene_surface->WLR_PRIVATE.clip.x;
	*sy += scene_surface->WLR_PRIVATE.clip.y;
	return pixman_region32_contains_point(
		&scene_surface->surface->input_region, (int)*sx, (int)*sy, NULL);
}

static void scene_surface_update_state(struct wlr_scene_surface *scene_surface) {
	struct wlr_surface *surface = scene_surface->surface;
	struct wlr_scene_buffer *scene_buffer = scene_surface->buffer;
	struct wlr_box clip = scene_surface->WLR_PRIVATE.clip;

	wlr_scene_node_set_enabled(&scene_buffer->node, surface->mapped);

	struct wlr_buffer *wlr_buf = surface->buffer ? &surface->buffer->base : NULL;
	struct wlr_scene_buffer_set_buffer_options opts = {
		.damage = &surface->buffer_damage,
	};
	struct wlr_linux_drm_syncobj_surface_v1_state *syncobj =
		wlr_linux_drm_syncobj_v1_get_surface_state(surface);
	if (syncobj && syncobj->acquire_timeline) {
		opts.wait_timeline = syncobj->acquire_timeline;
		opts.wait_point = syncobj->acquire_point;
	}

	int dst_w = surface->current.width;
	int dst_h = surface->current.height;
	struct wlr_fbox src_box = surface->current.viewport.has_src
		? surface->current.viewport.src
		: (struct wlr_fbox){0};

	if (!wlr_box_empty(&clip)) {
		if (wlr_fbox_empty(&src_box) && wlr_buf) {
			int bw = surface->current.buffer_width;
			int bh = surface->current.buffer_height;
			if (surface->current.transform & WL_OUTPUT_TRANSFORM_90) {
				int tmp = bw;
				bw = bh;
				bh = tmp;
			}
			src_box = (struct wlr_fbox){ 0, 0, bw, bh };
		}
		if (dst_w > 0 && dst_h > 0 && !wlr_fbox_empty(&src_box)) {
			double sx_scale = src_box.width / dst_w;
			double sy_scale = src_box.height / dst_h;
			int cx = clip.x < 0 ? 0 : clip.x;
			int cy = clip.y < 0 ? 0 : clip.y;
			int cw = clip.width;
			int ch = clip.height;
			if (cx + cw > dst_w) cw = dst_w - cx;
			if (cy + ch > dst_h) ch = dst_h - cy;
			if (cw < 0) cw = 0;
			if (ch < 0) ch = 0;
			src_box.x += cx * sx_scale;
			src_box.y += cy * sy_scale;
			src_box.width = cw * sx_scale;
			src_box.height = ch * sy_scale;
			dst_w = cw;
			dst_h = ch;
		}
	}

	wlr_scene_buffer_set_source_box(scene_buffer, &src_box);
	wlr_scene_buffer_set_dest_size(scene_buffer, dst_w, dst_h);
	wlr_scene_buffer_set_transform(scene_buffer, surface->current.transform);

	pixman_region32_t opaque;
	pixman_region32_init(&opaque);
	pixman_region32_copy(&opaque, &surface->opaque_region);
	if (!wlr_box_empty(&clip)) {
		pixman_region32_translate(&opaque, -clip.x, -clip.y);
		pixman_region32_intersect_rect(&opaque, &opaque, 0, 0, dst_w, dst_h);
	}
	wlr_scene_buffer_set_opaque_region(scene_buffer, &opaque);
	pixman_region32_fini(&opaque);

	wlr_scene_buffer_set_buffer_with_options(scene_buffer, wlr_buf, &opts);
}

static void handle_scene_surface_commit(struct wl_listener *listener, void *data) {
	struct wlr_scene_surface *scene_surface = wl_container_of(
		listener, scene_surface, WLR_PRIVATE.surface_commit);
	struct wlr_linux_drm_syncobj_surface_v1_state *syncobj =
		wlr_linux_drm_syncobj_v1_get_surface_state(scene_surface->surface);
	if (syncobj && scene_surface->surface->buffer) {
		wlr_linux_drm_syncobj_v1_state_signal_release_with_buffer(
			syncobj, &scene_surface->surface->buffer->base);
	}
	scene_surface_update_state(scene_surface);
}

static void handle_scene_surface_destroy(struct wl_listener *listener, void *data) {
	struct wlr_scene_surface *scene_surface = wl_container_of(
		listener, scene_surface, WLR_PRIVATE.surface_destroy);
	wlr_scene_node_destroy(&scene_surface->buffer->node);
}

static void handle_scene_surface_outputs_update(struct wl_listener *listener, void *data) {
	struct wlr_scene_surface *scene_surface = wl_container_of(
		listener, scene_surface, WLR_PRIVATE.outputs_update);
	struct wlr_scene_outputs_update_event *event = data;
	struct wlr_surface *surface = scene_surface->surface;

	// Enter / leave outputs
	struct wlr_surface_output *so, *tmp;
	wl_list_for_each_safe(so, tmp, &surface->current_outputs, link) {
		bool found = false;
		for (size_t i = 0; i < event->size; i++) {
			if (event->active[i]->output == so->output) {
				found = true;
				break;
			}
		}
		if (!found) {
			wlr_surface_send_leave(surface, so->output);
		}
	}

	float best_scale = 0.0f;
	enum wl_output_transform best_transform = WL_OUTPUT_TRANSFORM_NORMAL;
	for (size_t i = 0; i < event->size; i++) {
		struct wlr_output *output = event->active[i]->output;
		wlr_surface_send_enter(surface, output);
		if (output->scale > best_scale) {
			best_scale = output->scale;
			best_transform = output->transform;
		}
	}

	if (best_scale > 0.0f) {
		wlr_surface_set_preferred_buffer_scale(surface, (int32_t)ceilf(best_scale));
		wlr_surface_set_preferred_buffer_transform(surface, best_transform);
		wlr_fractional_scale_v1_notify_scale(surface, best_scale);
	}
}

static void handle_scene_surface_output_sample(struct wl_listener *listener, void *data) {
	struct wlr_scene_surface *scene_surface = wl_container_of(
		listener, scene_surface, WLR_PRIVATE.output_sample);
	struct wlr_scene_output_sample_event *event = data;
	struct wlr_surface *surface = scene_surface->surface;
	if (!surface) {
		return;
	}
	wlr_presentation_surface_textured_on_output(surface, event->output->output);
}

static void handle_scene_surface_frame_done(struct wl_listener *listener, void *data) {
	struct wlr_scene_surface *scene_surface = wl_container_of(
		listener, scene_surface, WLR_PRIVATE.frame_done);
	struct wlr_scene_frame_done_event *event = data;
	wlr_scene_surface_send_frame_done(scene_surface, &event->when);
}

struct wlr_scene_surface *wlr_scene_surface_create(struct wlr_scene_tree *parent,
		struct wlr_surface *surface) {
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_create(parent, NULL);
	if (!scene_buffer) {
		return NULL;
	}
	struct wlr_scene_surface *scene_surface = calloc(1, sizeof(*scene_surface));
	if (!scene_surface) {
		wlr_scene_node_destroy(&scene_buffer->node);
		return NULL;
	}

	scene_surface->buffer = scene_buffer;
	scene_surface->surface = surface;
	scene_buffer->point_accepts_input = scene_surface_point_accepts_input;

	wlr_addon_init(&scene_surface->WLR_PRIVATE.addon, &scene_buffer->node.addons,
		scene_buffer, &scene_surface_addon_impl);

	scene_surface->WLR_PRIVATE.outputs_update.notify = handle_scene_surface_outputs_update;
	wl_signal_add(&scene_buffer->events.outputs_update,
		&scene_surface->WLR_PRIVATE.outputs_update);

	scene_surface->WLR_PRIVATE.output_sample.notify = handle_scene_surface_output_sample;
	wl_signal_add(&scene_buffer->events.output_sample,
		&scene_surface->WLR_PRIVATE.output_sample);

	scene_surface->WLR_PRIVATE.frame_done.notify = handle_scene_surface_frame_done;
	wl_signal_add(&scene_buffer->events.frame_done,
		&scene_surface->WLR_PRIVATE.frame_done);

	scene_surface->WLR_PRIVATE.surface_destroy.notify = handle_scene_surface_destroy;
	wl_signal_add(&surface->events.destroy, &scene_surface->WLR_PRIVATE.surface_destroy);

	scene_surface->WLR_PRIVATE.surface_commit.notify = handle_scene_surface_commit;
	wl_signal_add(&surface->events.commit, &scene_surface->WLR_PRIVATE.surface_commit);

	scene_surface_update_state(scene_surface);
	return scene_surface;
}

void wlr_scene_surface_send_frame_done(struct wlr_scene_surface *scene_surface,
		const struct timespec *when) {
	if (!scene_surface || !scene_surface->surface) {
		return;
	}
	if (!pixman_region32_empty(&scene_surface->buffer->node.WLR_PRIVATE.visible) ||
			scene_surface->buffer->primary_output != NULL) {
		wlr_surface_send_frame_done(scene_surface->surface, when);
	}
}

// --- Subsurface tree ---

struct scene_subsurface_tree {
	struct wlr_scene_tree *tree;
	struct wlr_surface *surface;
	struct wlr_scene_surface *scene_surface;
	struct wlr_scene_tree *subsurfaces_below;
	struct wlr_scene_tree *subsurfaces_above;
	struct wlr_addon addon;
	struct wl_listener tree_destroy;
	struct wl_listener surface_destroy;
	struct wl_listener surface_commit;
	struct wl_listener new_subsurface;
};

struct scene_subsurface_child {
	struct wlr_subsurface *subsurface;
	struct wlr_scene_tree *tree;
	struct wl_listener sub_destroy;
	struct wl_listener tree_destroy;
};

static void scene_subsurface_child_free(struct scene_subsurface_child *child) {
	wl_list_remove(&child->sub_destroy.link);
	wl_list_remove(&child->tree_destroy.link);
	free(child);
}

static void handle_subsurface_child_sub_destroy(struct wl_listener *listener, void *data) {
	struct scene_subsurface_child *child = wl_container_of(listener, child, sub_destroy);
	struct wlr_scene_tree *tree = child->tree;
	scene_subsurface_child_free(child);
	wlr_scene_node_destroy(&tree->node);
}

static void handle_subsurface_child_tree_destroy(struct wl_listener *listener, void *data) {
	struct scene_subsurface_child *child = wl_container_of(listener, child, tree_destroy);
	scene_subsurface_child_free(child);
}

static void sync_subsurface_list(struct wl_list *subsurfaces,
		struct wlr_scene_tree *target_tree) {
	struct wlr_subsurface *sub;
	struct wlr_scene_node *prev_node = NULL;
	wl_list_for_each(sub, subsurfaces, current.link) {
		struct wlr_scene_node *found = NULL;
		struct wlr_scene_node *n;
		wl_list_for_each(n, &target_tree->children, link) {
			if (n->data == sub) {
				found = n;
				break;
			}
		}
		if (!found) {
			struct wlr_scene_tree *sub_tree =
				wlr_scene_subsurface_tree_create(target_tree, sub->surface);
			if (!sub_tree) {
				continue;
			}
			sub_tree->node.data = sub;
			struct scene_subsurface_child *c = calloc(1, sizeof(*c));
			if (c) {
				c->subsurface = sub;
				c->tree = sub_tree;
				c->sub_destroy.notify = handle_subsurface_child_sub_destroy;
				wl_signal_add(&sub->events.destroy, &c->sub_destroy);
				c->tree_destroy.notify = handle_subsurface_child_tree_destroy;
				wl_signal_add(&sub_tree->node.events.destroy, &c->tree_destroy);
			}
			found = &sub_tree->node;
		}
		wlr_scene_node_set_position(found, sub->current.x, sub->current.y);
		if (prev_node) {
			wlr_scene_node_place_above(found, prev_node);
		} else {
			wlr_scene_node_lower_to_bottom(found);
		}
		prev_node = found;
	}
}

static void handle_subsurface_tree_commit(struct wl_listener *listener, void *data) {
	struct scene_subsurface_tree *st = wl_container_of(listener, st, surface_commit);
	sync_subsurface_list(&st->surface->current.subsurfaces_below, st->subsurfaces_below);
	sync_subsurface_list(&st->surface->current.subsurfaces_above, st->subsurfaces_above);
}

static void handle_subsurface_tree_new_sub(struct wl_listener *listener, void *data) {
	struct scene_subsurface_tree *st = wl_container_of(listener, st, new_subsurface);
	sync_subsurface_list(&st->surface->current.subsurfaces_below, st->subsurfaces_below);
	sync_subsurface_list(&st->surface->current.subsurfaces_above, st->subsurfaces_above);
}

static void scene_subsurface_tree_cleanup(struct scene_subsurface_tree *st) {
	wl_list_remove(&st->tree_destroy.link);
	wl_list_remove(&st->surface_destroy.link);
	wl_list_remove(&st->surface_commit.link);
	wl_list_remove(&st->new_subsurface.link);
	free(st);
}

static void handle_subsurface_tree_node_destroy(struct wl_listener *listener, void *data) {
	struct scene_subsurface_tree *st = wl_container_of(listener, st, tree_destroy);
	wlr_addon_finish(&st->addon);
	scene_subsurface_tree_cleanup(st);
}

static void handle_subsurface_tree_surface_destroy(struct wl_listener *listener, void *data) {
	struct scene_subsurface_tree *st = wl_container_of(listener, st, surface_destroy);
	wlr_scene_node_destroy(&st->tree->node);
}

static void subsurface_addon_destroy(struct wlr_addon *addon) {
	wlr_addon_finish(addon);
}

static const struct wlr_addon_interface subsurface_tree_addon_impl = {
	.name = "wlr_scene_subsurface_tree",
	.destroy = subsurface_addon_destroy,
};

struct wlr_scene_tree *wlr_scene_subsurface_tree_create(
		struct wlr_scene_tree *parent, struct wlr_surface *surface) {
	struct wlr_scene_tree *tree = wlr_scene_tree_create(parent);
	if (!tree) {
		return NULL;
	}
	struct scene_subsurface_tree *st = calloc(1, sizeof(*st));
	if (!st) {
		wlr_scene_node_destroy(&tree->node);
		return NULL;
	}

	st->tree = tree;
	st->surface = surface;
	st->subsurfaces_below = wlr_scene_tree_create(tree);
	st->scene_surface = wlr_scene_surface_create(tree, surface);
	st->subsurfaces_above = wlr_scene_tree_create(tree);
	if (!st->subsurfaces_below || !st->scene_surface || !st->subsurfaces_above) {
		free(st);
		wlr_scene_node_destroy(&tree->node);
		return NULL;
	}

	wlr_addon_init(&st->addon, &tree->node.addons, tree, &subsurface_tree_addon_impl);

	st->tree_destroy.notify = handle_subsurface_tree_node_destroy;
	wl_signal_add(&tree->node.events.destroy, &st->tree_destroy);

	st->surface_destroy.notify = handle_subsurface_tree_surface_destroy;
	wl_signal_add(&surface->events.destroy, &st->surface_destroy);

	st->surface_commit.notify = handle_subsurface_tree_commit;
	wl_signal_add(&surface->events.commit, &st->surface_commit);

	st->new_subsurface.notify = handle_subsurface_tree_new_sub;
	wl_signal_add(&surface->events.new_subsurface, &st->new_subsurface);

	sync_subsurface_list(&surface->current.subsurfaces_below, st->subsurfaces_below);
	sync_subsurface_list(&surface->current.subsurfaces_above, st->subsurfaces_above);
	return tree;
}

void wlr_scene_subsurface_tree_set_clip(struct wlr_scene_node *node,
		const struct wlr_box *clip) {
	if (!node || node->type != WLR_SCENE_NODE_TREE) {
		return;
	}
	struct wlr_addon *addon = wlr_addon_find(&node->addons,
		wlr_scene_tree_from_node(node), &subsurface_tree_addon_impl);
	if (!addon) {
		return;
	}
	struct scene_subsurface_tree *st = wl_container_of(addon, st, addon);
	struct wlr_box new_clip = clip ? *clip : (struct wlr_box){0};
	if (memcmp(&st->scene_surface->WLR_PRIVATE.clip, &new_clip, sizeof(new_clip)) != 0) {
		st->scene_surface->WLR_PRIVATE.clip = new_clip;
		scene_surface_update_state(st->scene_surface);
	}
}

// --- XDG surface ---

struct scene_xdg_surface {
	struct wlr_scene_tree *tree;
	struct wlr_xdg_surface *xdg_surface;
	struct wlr_scene_tree *surface_tree;
	struct wl_listener tree_destroy;
	struct wl_listener xdg_destroy;
	struct wl_listener xdg_commit;
};

static void scene_xdg_update(struct scene_xdg_surface *sx) {
	if (sx->xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		struct wlr_xdg_popup *popup = sx->xdg_surface->popup;
		if (popup) {
			struct wlr_box geo = popup->current.geometry;
			wlr_scene_node_set_position(&sx->tree->node, geo.x, geo.y);
		}
	}
	struct wlr_box xdg_geo = sx->xdg_surface->geometry;
	wlr_scene_node_set_position(&sx->surface_tree->node, -xdg_geo.x, -xdg_geo.y);
}

static void handle_scene_xdg_commit(struct wl_listener *listener, void *data) {
	struct scene_xdg_surface *sx = wl_container_of(listener, sx, xdg_commit);
	scene_xdg_update(sx);
}

static void handle_scene_xdg_tree_destroy(struct wl_listener *listener, void *data) {
	struct scene_xdg_surface *sx = wl_container_of(listener, sx, tree_destroy);
	wl_list_remove(&sx->tree_destroy.link);
	wl_list_remove(&sx->xdg_destroy.link);
	wl_list_remove(&sx->xdg_commit.link);
	free(sx);
}

static void handle_scene_xdg_destroy(struct wl_listener *listener, void *data) {
	struct scene_xdg_surface *sx = wl_container_of(listener, sx, xdg_destroy);
	wlr_scene_node_destroy(&sx->tree->node);
}

struct wlr_scene_tree *wlr_scene_xdg_surface_create(
		struct wlr_scene_tree *parent, struct wlr_xdg_surface *xdg_surface) {
	struct wlr_scene_tree *tree = wlr_scene_tree_create(parent);
	if (!tree) {
		return NULL;
	}
	struct scene_xdg_surface *sx = calloc(1, sizeof(*sx));
	if (!sx) {
		wlr_scene_node_destroy(&tree->node);
		return NULL;
	}
	sx->tree = tree;
	sx->xdg_surface = xdg_surface;
	sx->surface_tree = wlr_scene_subsurface_tree_create(tree, xdg_surface->surface);
	if (!sx->surface_tree) {
		free(sx);
		wlr_scene_node_destroy(&tree->node);
		return NULL;
	}

	sx->tree_destroy.notify = handle_scene_xdg_tree_destroy;
	wl_signal_add(&tree->node.events.destroy, &sx->tree_destroy);
	sx->xdg_destroy.notify = handle_scene_xdg_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &sx->xdg_destroy);
	sx->xdg_commit.notify = handle_scene_xdg_commit;
	wl_signal_add(&xdg_surface->surface->events.commit, &sx->xdg_commit);

	scene_xdg_update(sx);
	return tree;
}

// --- Layer surface ---

static void handle_layer_tree_destroy(struct wl_listener *listener, void *data) {
	struct wlr_scene_layer_surface_v1 *ls = wl_container_of(
		listener, ls, WLR_PRIVATE.tree_destroy);
	wl_list_remove(&ls->WLR_PRIVATE.tree_destroy.link);
	wl_list_remove(&ls->WLR_PRIVATE.layer_surface_destroy.link);
	wl_list_remove(&ls->WLR_PRIVATE.layer_surface_map.link);
	wl_list_remove(&ls->WLR_PRIVATE.layer_surface_unmap.link);
	free(ls);
}

static void handle_layer_surface_destroy(struct wl_listener *listener, void *data) {
	struct wlr_scene_layer_surface_v1 *ls = wl_container_of(
		listener, ls, WLR_PRIVATE.layer_surface_destroy);
	wlr_scene_node_destroy(&ls->tree->node);
}

static void handle_layer_surface_map(struct wl_listener *listener, void *data) {
	struct wlr_scene_layer_surface_v1 *ls = wl_container_of(
		listener, ls, WLR_PRIVATE.layer_surface_map);
	wlr_scene_node_set_enabled(&ls->tree->node, true);
}

static void handle_layer_surface_unmap(struct wl_listener *listener, void *data) {
	struct wlr_scene_layer_surface_v1 *ls = wl_container_of(
		listener, ls, WLR_PRIVATE.layer_surface_unmap);
	wlr_scene_node_set_enabled(&ls->tree->node, false);
}

struct wlr_scene_layer_surface_v1 *wlr_scene_layer_surface_v1_create(
		struct wlr_scene_tree *parent, struct wlr_layer_surface_v1 *layer_surface) {
	struct wlr_scene_layer_surface_v1 *ls = calloc(1, sizeof(*ls));
	if (!ls) {
		return NULL;
	}
	struct wlr_scene_tree *tree = wlr_scene_tree_create(parent);
	if (!tree) {
		free(ls);
		return NULL;
	}
	if (!wlr_scene_subsurface_tree_create(tree, layer_surface->surface)) {
		wlr_scene_node_destroy(&tree->node);
		free(ls);
		return NULL;
	}

	ls->tree = tree;
	ls->layer_surface = layer_surface;

	ls->WLR_PRIVATE.tree_destroy.notify = handle_layer_tree_destroy;
	wl_signal_add(&tree->node.events.destroy, &ls->WLR_PRIVATE.tree_destroy);
	ls->WLR_PRIVATE.layer_surface_destroy.notify = handle_layer_surface_destroy;
	wl_signal_add(&layer_surface->events.destroy, &ls->WLR_PRIVATE.layer_surface_destroy);
	ls->WLR_PRIVATE.layer_surface_map.notify = handle_layer_surface_map;
	wl_signal_add(&layer_surface->surface->events.map, &ls->WLR_PRIVATE.layer_surface_map);
	ls->WLR_PRIVATE.layer_surface_unmap.notify = handle_layer_surface_unmap;
	wl_signal_add(&layer_surface->surface->events.unmap, &ls->WLR_PRIVATE.layer_surface_unmap);

	wlr_scene_node_set_enabled(&tree->node, layer_surface->surface->mapped);
	return ls;
}

void wlr_scene_layer_surface_v1_configure(
		struct wlr_scene_layer_surface_v1 *ls,
		const struct wlr_box *full_area, struct wlr_box *usable_area) {
	struct wlr_layer_surface_v1 *layer = ls->layer_surface;
	struct wlr_layer_surface_v1_state *state = &layer->current;

	struct wlr_box bounds = state->exclusive_zone == -1 ? *full_area : *usable_area;
	struct wlr_box box = {
		.width = state->desired_width,
		.height = state->desired_height,
	};

	uint32_t both_horiz = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	uint32_t both_vert = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;

	if ((state->anchor & both_horiz) == both_horiz && box.width == 0) {
		box.x = bounds.x;
		box.width = bounds.width;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) {
		box.x = bounds.x;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
		box.x = bounds.x + bounds.width - box.width;
	} else {
		box.x = bounds.x + bounds.width / 2 - box.width / 2;
	}

	if ((state->anchor & both_vert) == both_vert && box.height == 0) {
		box.y = bounds.y;
		box.height = bounds.height;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) {
		box.y = bounds.y;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
		box.y = bounds.y + bounds.height - box.height;
	} else {
		box.y = bounds.y + bounds.height / 2 - box.height / 2;
	}

	if ((state->anchor & both_horiz) == both_horiz) {
		box.x += state->margin.left;
		box.width -= state->margin.left + state->margin.right;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) {
		box.x += state->margin.left;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
		box.x -= state->margin.right;
	}

	if ((state->anchor & both_vert) == both_vert) {
		box.y += state->margin.top;
		box.height -= state->margin.top + state->margin.bottom;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) {
		box.y += state->margin.top;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
		box.y -= state->margin.bottom;
	}

	if (box.width < 0 || box.height < 0) {
		wlr_layer_surface_v1_destroy(layer);
		return;
	}

	if (state->exclusive_zone > 0) {
		enum wlr_edges edge = wlr_layer_surface_v1_get_exclusive_edge(layer);
		switch (edge) {
		case WLR_EDGE_TOP:
			usable_area->y += state->exclusive_zone + state->margin.top;
			usable_area->height -= state->exclusive_zone + state->margin.top;
			break;
		case WLR_EDGE_BOTTOM:
			usable_area->height -= state->exclusive_zone + state->margin.bottom;
			break;
		case WLR_EDGE_LEFT:
			usable_area->x += state->exclusive_zone + state->margin.left;
			usable_area->width -= state->exclusive_zone + state->margin.left;
			break;
		case WLR_EDGE_RIGHT:
			usable_area->width -= state->exclusive_zone + state->margin.right;
			break;
		case WLR_EDGE_NONE:
			break;
		}
	}

	wlr_layer_surface_v1_configure(layer, box.width, box.height);
	wlr_scene_node_set_position(&ls->tree->node, box.x, box.y);
}

// --- Drag icon ---

struct scene_drag_icon {
	struct wlr_scene_tree *tree;
	struct wl_listener tree_destroy;
	struct wl_listener icon_destroy;
};

static void handle_drag_icon_tree_destroy(struct wl_listener *listener, void *data) {
	struct scene_drag_icon *sdi = wl_container_of(listener, sdi, tree_destroy);
	wl_list_remove(&sdi->tree_destroy.link);
	wl_list_remove(&sdi->icon_destroy.link);
	free(sdi);
}

static void handle_drag_icon_destroy(struct wl_listener *listener, void *data) {
	struct scene_drag_icon *sdi = wl_container_of(listener, sdi, icon_destroy);
	wlr_scene_node_destroy(&sdi->tree->node);
}

struct wlr_scene_tree *wlr_scene_drag_icon_create(
		struct wlr_scene_tree *parent, struct wlr_drag_icon *drag_icon) {
	struct wlr_scene_tree *tree = wlr_scene_subsurface_tree_create(
		parent, drag_icon->surface);
	if (!tree) {
		return NULL;
	}
	struct scene_drag_icon *sdi = calloc(1, sizeof(*sdi));
	if (!sdi) {
		wlr_scene_node_destroy(&tree->node);
		return NULL;
	}
	sdi->tree = tree;
	sdi->tree_destroy.notify = handle_drag_icon_tree_destroy;
	wl_signal_add(&tree->node.events.destroy, &sdi->tree_destroy);
	sdi->icon_destroy.notify = handle_drag_icon_destroy;
	wl_signal_add(&drag_icon->events.destroy, &sdi->icon_destroy);
	return tree;
}
