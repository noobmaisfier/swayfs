#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/util/region.h>
#include "scene_internal.h"

static void scene_node_init(struct wlr_scene_node *node,
		enum wlr_scene_node_type type, struct wlr_scene_tree *parent) {
	node->type = type;
	node->parent = parent;
	node->enabled = true;
	wl_signal_init(&node->events.destroy);
	wlr_addon_set_init(&node->addons);
	pixman_region32_init(&node->WLR_PRIVATE.visible);

	if (parent) {
		wl_list_insert(parent->children.prev, &node->link);
	} else {
		wl_list_init(&node->link);
	}
}

struct wlr_scene *scene_node_get_root(struct wlr_scene_node *node) {
	struct wlr_scene_tree *tree = NULL;
	if (node->type == WLR_SCENE_NODE_TREE) {
		tree = wlr_scene_tree_from_node(node);
	} else {
		tree = node->parent;
	}
	while (tree && tree->node.parent) {
		tree = tree->node.parent;
	}
	if (!tree) {
		return NULL;
	}
	// Root tree has parent == NULL and is embedded at start of struct wlr_scene
	return wl_container_of(tree, (struct wlr_scene *)NULL, tree);
}

void scene_buffer_get_dest_size(struct wlr_scene_buffer *buffer, int *width, int *height) {
	if (buffer->dst_width > 0 && buffer->dst_height > 0) {
		*width = buffer->dst_width;
		*height = buffer->dst_height;
		return;
	}
	if (!buffer->buffer) {
		*width = 0;
		*height = 0;
		return;
	}
	int w = buffer->WLR_PRIVATE.buffer_width;
	int h = buffer->WLR_PRIVATE.buffer_height;
	if (buffer->transform & WL_OUTPUT_TRANSFORM_90) {
		*width = h;
		*height = w;
	} else {
		*width = w;
		*height = h;
	}
}

void scene_node_get_size(struct wlr_scene_node *node, int *width, int *height) {
	switch (node->type) {
	case WLR_SCENE_NODE_TREE:
		*width = 0;
		*height = 0;
		break;
	case WLR_SCENE_NODE_RECT:;
		struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
		*width = rect->width;
		*height = rect->height;
		break;
	case WLR_SCENE_NODE_BUFFER:;
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
		scene_buffer_get_dest_size(buffer, width, height);
		break;
	}
}

static void scene_node_bbox(struct wlr_scene_node *node, int lx, int ly,
		pixman_region32_t *region) {
	if (!node->enabled) {
		return;
	}
	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			scene_node_bbox(child, lx + child->x, ly + child->y, region);
		}
		return;
	}
	int w = 0, h = 0;
	scene_node_get_size(node, &w, &h);
	if (w > 0 && h > 0) {
		pixman_region32_union_rect(region, region, lx, ly, w, h);
	}
}

static void scene_update_buffer_outputs_recursive(struct wlr_scene *scene,
		struct wlr_scene_node *node, int lx, int ly, bool parent_enabled) {
	bool enabled = parent_enabled && node->enabled;
	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			scene_update_buffer_outputs_recursive(scene, child,
				lx + child->x, ly + child->y, enabled);
		}
		return;
	}
	if (node->type != WLR_SCENE_NODE_BUFFER) {
		return;
	}

	struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
	uint64_t active_mask = 0;
	struct wlr_scene_output *active[64];
	size_t active_len = 0;
	struct wlr_scene_output *primary = NULL;
	int64_t best_area = -1;

	int w = 0, h = 0;
	scene_buffer_get_dest_size(buffer, &w, &h);

	if (enabled && w > 0 && h > 0) {
		struct wlr_box buf_box = { lx, ly, w, h };
		struct wlr_scene_output *so;
		wl_list_for_each(so, &scene->outputs, link) {
			if (!so->output->enabled) {
				continue;
			}
			int ow = 0, oh = 0;
			wlr_output_effective_resolution(so->output, &ow, &oh);
			struct wlr_box out_box = { so->x, so->y, ow, oh };
			struct wlr_box inter;
			if (wlr_box_intersection(&inter, &buf_box, &out_box)) {
				uint8_t idx = so->WLR_PRIVATE.index;
				if (idx < 64) {
					active_mask |= (UINT64_C(1) << idx);
				}
				if (active_len < 64) {
					active[active_len++] = so;
				}
				int64_t area = (int64_t)inter.width * inter.height;
				if (area > best_area) {
					best_area = area;
					primary = so;
				}
			}
		}
	}

	uint64_t prev_mask = buffer->WLR_PRIVATE.active_outputs;
	if (prev_mask != active_mask || buffer->primary_output != primary) {
		buffer->primary_output = primary;

		struct wlr_scene_output *so;
		wl_list_for_each(so, &scene->outputs, link) {
			uint64_t bit = so->WLR_PRIVATE.index < 64
				? (UINT64_C(1) << so->WLR_PRIVATE.index) : 0;
			bool was = (prev_mask & bit) != 0;
			bool now = (active_mask & bit) != 0;
			if (!was && now) {
				scene_buffer_send_output_enter(buffer, so);
			} else if (was && !now) {
				scene_buffer_send_output_leave(buffer, so);
			}
		}

		buffer->WLR_PRIVATE.active_outputs = active_mask;
		struct wlr_scene_outputs_update_event event = {
			.active = active,
			.size = active_len,
		};
		wl_signal_emit_mutable(&buffer->events.outputs_update, &event);
	}
}

void scene_node_update(struct wlr_scene_node *node, const pixman_region32_t *damage) {
	struct wlr_scene *scene = scene_node_get_root(node);
	if (!scene) {
		return;
	}

	int lx = 0, ly = 0;
	bool visible = wlr_scene_node_coords(node, &lx, &ly);

	if (damage) {
		if (visible) {
			pixman_region32_t region;
			pixman_region32_init(&region);
			pixman_region32_copy(&region, damage);
			pixman_region32_translate(&region, lx, ly);
			scene_damage_outputs(scene, &region);
			pixman_region32_fini(&region);
		}
	} else {
		pixman_region32_t region;
		pixman_region32_init(&region);
		if (visible) {
			scene_node_bbox(node, lx, ly, &region);
		}
		if (!pixman_region32_empty(&node->WLR_PRIVATE.visible)) {
			pixman_region32_union(&region, &region, &node->WLR_PRIVATE.visible);
		}
		if (!pixman_region32_empty(&region)) {
			scene_damage_outputs(scene, &region);
		}
		pixman_region32_fini(&region);
	}

	scene_update_buffer_outputs_recursive(scene, node, lx, ly, visible);
}

static void scene_buffer_clear_buffer(struct wlr_scene_buffer *scene_buffer) {
	if (!wl_list_empty(&scene_buffer->WLR_PRIVATE.buffer_release.link)) {
		wl_list_remove(&scene_buffer->WLR_PRIVATE.buffer_release.link);
		wl_list_init(&scene_buffer->WLR_PRIVATE.buffer_release.link);
	}
	if (scene_buffer->buffer) {
		wlr_buffer_unlock(scene_buffer->buffer);
		scene_buffer->buffer = NULL;
	}
	scene_buffer->WLR_PRIVATE.buffer_width = 0;
	scene_buffer->WLR_PRIVATE.buffer_height = 0;
	scene_buffer->WLR_PRIVATE.buffer_is_opaque = false;
	scene_buffer->WLR_PRIVATE.is_single_pixel_buffer = false;
}

static void scene_buffer_destroy_texture(struct wlr_scene_buffer *scene_buffer) {
	if (!wl_list_empty(&scene_buffer->WLR_PRIVATE.renderer_destroy.link)) {
		wl_list_remove(&scene_buffer->WLR_PRIVATE.renderer_destroy.link);
		wl_list_init(&scene_buffer->WLR_PRIVATE.renderer_destroy.link);
	}
	if (scene_buffer->WLR_PRIVATE.texture) {
		wlr_texture_destroy(scene_buffer->WLR_PRIVATE.texture);
		scene_buffer->WLR_PRIVATE.texture = NULL;
	}
}

void wlr_scene_node_destroy(struct wlr_scene_node *node) {
	if (!node) {
		return;
	}

	struct wlr_scene *scene = scene_node_get_root(node);
	int lx = 0, ly = 0;
	bool was_visible = scene && wlr_scene_node_coords(node, &lx, &ly);
	pixman_region32_t old_damage;
	pixman_region32_init(&old_damage);
	if (was_visible) {
		scene_node_bbox(node, lx, ly, &old_damage);
	}
	if (!pixman_region32_empty(&node->WLR_PRIVATE.visible)) {
		pixman_region32_union(&old_damage, &old_damage, &node->WLR_PRIVATE.visible);
	}

	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child, *tmp;
		wl_list_for_each_safe(child, tmp, &tree->children, link) {
			wlr_scene_node_destroy(child);
		}
	}

	wl_signal_emit_mutable(&node->events.destroy, NULL);
	wlr_addon_set_finish(&node->addons);
	wl_list_remove(&node->link);
	pixman_region32_fini(&node->WLR_PRIVATE.visible);

	if (scene && !pixman_region32_empty(&old_damage) && node->parent != NULL) {
		scene_damage_outputs(scene, &old_damage);
	}
	pixman_region32_fini(&old_damage);

	switch (node->type) {
	case WLR_SCENE_NODE_TREE:;
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		if (!node->parent) {
			// Root struct wlr_scene
			struct wlr_scene *root_scene = wl_container_of(tree, root_scene, tree);
			struct wlr_scene_output *so, *tmp_so;
			wl_list_for_each_safe(so, tmp_so, &root_scene->outputs, link) {
				wlr_scene_output_destroy(so);
			}
			if (root_scene->linux_dmabuf_v1) {
				wl_list_remove(&root_scene->WLR_PRIVATE.linux_dmabuf_v1_destroy.link);
			}
			if (root_scene->gamma_control_manager_v1) {
				wl_list_remove(&root_scene->WLR_PRIVATE.gamma_control_manager_v1_destroy.link);
				wl_list_remove(&root_scene->WLR_PRIVATE.gamma_control_manager_v1_set_gamma.link);
			}
			if (root_scene->color_manager_v1) {
				wl_list_remove(&root_scene->WLR_PRIVATE.color_manager_v1_destroy.link);
			}
			free(root_scene);
		} else {
			free(tree);
		}
		break;
	case WLR_SCENE_NODE_RECT:
		free(wlr_scene_rect_from_node(node));
		break;
	case WLR_SCENE_NODE_BUFFER:;
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
		scene_buffer_clear_buffer(buffer);
		scene_buffer_destroy_texture(buffer);
		if (buffer->WLR_PRIVATE.wait_timeline) {
			wlr_drm_syncobj_timeline_unref(buffer->WLR_PRIVATE.wait_timeline);
		}
		pixman_region32_fini(&buffer->opaque_region);
		free(buffer);
		break;
	}
}

void wlr_scene_node_set_enabled(struct wlr_scene_node *node, bool enabled) {
	if (node->enabled == enabled) {
		return;
	}
	// Capture old damage before disabling
	pixman_region32_t old_damage;
	pixman_region32_init(&old_damage);
	struct wlr_scene *scene = scene_node_get_root(node);
	if (!enabled && scene) {
		int lx = 0, ly = 0;
		if (wlr_scene_node_coords(node, &lx, &ly)) {
			scene_node_bbox(node, lx, ly, &old_damage);
		}
		if (!pixman_region32_empty(&node->WLR_PRIVATE.visible)) {
			pixman_region32_union(&old_damage, &old_damage, &node->WLR_PRIVATE.visible);
		}
	}

	node->enabled = enabled;
	if (!enabled && scene && !pixman_region32_empty(&old_damage)) {
		scene_damage_outputs(scene, &old_damage);
	}
	pixman_region32_fini(&old_damage);

	scene_node_update(node, NULL);
}

void wlr_scene_node_set_position(struct wlr_scene_node *node, int x, int y) {
	if (node->x == x && node->y == y) {
		return;
	}
	struct wlr_scene *scene = scene_node_get_root(node);
	pixman_region32_t old_damage;
	pixman_region32_init(&old_damage);
	int lx = 0, ly = 0;
	if (scene && wlr_scene_node_coords(node, &lx, &ly)) {
		scene_node_bbox(node, lx, ly, &old_damage);
	}

	node->x = x;
	node->y = y;

	if (scene && !pixman_region32_empty(&old_damage)) {
		scene_damage_outputs(scene, &old_damage);
	}
	pixman_region32_fini(&old_damage);

	scene_node_update(node, NULL);
}

void wlr_scene_node_place_above(struct wlr_scene_node *node,
		struct wlr_scene_node *sibling) {
	assert(node != sibling && node->parent == sibling->parent);
	if (node->link.prev == &sibling->link) {
		return;
	}
	wl_list_remove(&node->link);
	wl_list_insert(&sibling->link, &node->link);
	scene_node_update(node, NULL);
}

void wlr_scene_node_place_below(struct wlr_scene_node *node,
		struct wlr_scene_node *sibling) {
	assert(node != sibling && node->parent == sibling->parent);
	if (node->link.next == &sibling->link) {
		return;
	}
	wl_list_remove(&node->link);
	wl_list_insert(sibling->link.prev, &node->link);
	scene_node_update(node, NULL);
}

void wlr_scene_node_raise_to_top(struct wlr_scene_node *node) {
	struct wlr_scene_node *current_top = wl_container_of(
		node->parent->children.prev, current_top, link);
	if (node == current_top) {
		return;
	}
	wlr_scene_node_place_above(node, current_top);
}

void wlr_scene_node_lower_to_bottom(struct wlr_scene_node *node) {
	struct wlr_scene_node *current_bottom = wl_container_of(
		node->parent->children.next, current_bottom, link);
	if (node == current_bottom) {
		return;
	}
	wlr_scene_node_place_below(node, current_bottom);
}

void wlr_scene_node_reparent(struct wlr_scene_node *node,
		struct wlr_scene_tree *new_parent) {
	assert(new_parent != NULL && node->parent != NULL);
	if (node->parent == new_parent) {
		return;
	}

	struct wlr_scene *old_scene = scene_node_get_root(node);
	pixman_region32_t old_damage;
	pixman_region32_init(&old_damage);
	int lx = 0, ly = 0;
	if (old_scene && wlr_scene_node_coords(node, &lx, &ly)) {
		scene_node_bbox(node, lx, ly, &old_damage);
	}

	wl_list_remove(&node->link);
	node->parent = new_parent;
	wl_list_insert(new_parent->children.prev, &node->link);

	if (old_scene && !pixman_region32_empty(&old_damage)) {
		scene_damage_outputs(old_scene, &old_damage);
	}
	pixman_region32_fini(&old_damage);

	// If moved out of an output or into staging, make sure old scene buffers update outputs
	struct wlr_scene *new_scene = scene_node_get_root(node);
	if (old_scene && old_scene != new_scene) {
		scene_update_buffer_outputs_recursive(old_scene, node, 0, 0, false);
	}
	scene_node_update(node, NULL);
}

bool wlr_scene_node_coords(struct wlr_scene_node *node, int *lx, int *ly) {
	assert(node);
	int x = 0, y = 0;
	bool enabled = true;
	while (node) {
		x += node->x;
		y += node->y;
		enabled = enabled && node->enabled;
		node = node->parent ? &node->parent->node : NULL;
	}
	*lx = x;
	*ly = y;
	return enabled;
}

static void scene_node_for_each_buffer_rec(struct wlr_scene_node *node,
		int lx, int ly, wlr_scene_buffer_iterator_func_t iterator, void *user_data) {
	if (!node->enabled) {
		return;
	}
	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			scene_node_for_each_buffer_rec(child, lx + child->x, ly + child->y,
				iterator, user_data);
		}
	} else if (node->type == WLR_SCENE_NODE_BUFFER) {
		iterator(wlr_scene_buffer_from_node(node), lx, ly, user_data);
	}
}

void wlr_scene_node_for_each_buffer(struct wlr_scene_node *node,
		wlr_scene_buffer_iterator_func_t iterator, void *user_data) {
	if (!node || !node->enabled) {
		return;
	}
	scene_node_for_each_buffer_rec(node, node->x, node->y, iterator, user_data);
}

struct wlr_scene_node *wlr_scene_node_at(struct wlr_scene_node *node,
		double lx, double ly, double *nx, double *ny) {
	if (!node || !node->enabled) {
		return NULL;
	}

	double rx = lx - node->x;
	double ry = ly - node->y;

	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each_reverse(child, &tree->children, link) {
			struct wlr_scene_node *found = wlr_scene_node_at(child, rx, ry, nx, ny);
			if (found) {
				return found;
			}
		}
		return NULL;
	}

	if (node->type == WLR_SCENE_NODE_RECT) {
		struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
		if (!rect->accepts_input) {
			return NULL;
		}
		if (rx >= 0 && rx < rect->width && ry >= 0 && ry < rect->height) {
			if (nx) *nx = rx;
			if (ny) *ny = ry;
			return node;
		}
		return NULL;
	}

	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
		int w = 0, h = 0;
		scene_buffer_get_dest_size(buffer, &w, &h);
		if (rx >= 0 && rx < w && ry >= 0 && ry < h) {
			double sx = rx;
			double sy = ry;
			if (buffer->point_accepts_input &&
					!buffer->point_accepts_input(buffer, &sx, &sy)) {
				return NULL;
			}
			if (nx) *nx = sx;
			if (ny) *ny = sy;
			return node;
		}
		return NULL;
	}

	return NULL;
}

struct wlr_scene *wlr_scene_create(void) {
	struct wlr_scene *scene = calloc(1, sizeof(*scene));
	if (!scene) {
		return NULL;
	}
	scene_node_init(&scene->tree.node, WLR_SCENE_NODE_TREE, NULL);
	wl_list_init(&scene->tree.children);
	wl_list_init(&scene->outputs);
	scene->restack_xwayland_surfaces = true;
	scene->WLR_PRIVATE.direct_scanout = true;
	scene->WLR_PRIVATE.calculate_visibility = true;
	return scene;
}

static void handle_linux_dmabuf_v1_destroy(struct wl_listener *listener, void *data) {
	struct wlr_scene *scene = wl_container_of(
		listener, scene, WLR_PRIVATE.linux_dmabuf_v1_destroy);
	wl_list_remove(&scene->WLR_PRIVATE.linux_dmabuf_v1_destroy.link);
	scene->linux_dmabuf_v1 = NULL;
}

void wlr_scene_set_linux_dmabuf_v1(struct wlr_scene *scene,
		struct wlr_linux_dmabuf_v1 *linux_dmabuf_v1) {
	assert(!scene->linux_dmabuf_v1);
	scene->linux_dmabuf_v1 = linux_dmabuf_v1;
	scene->WLR_PRIVATE.linux_dmabuf_v1_destroy.notify = handle_linux_dmabuf_v1_destroy;
	wl_signal_add(&linux_dmabuf_v1->events.destroy,
		&scene->WLR_PRIVATE.linux_dmabuf_v1_destroy);
}

static void handle_gamma_control_destroy(struct wl_listener *listener, void *data) {
	struct wlr_scene *scene = wl_container_of(
		listener, scene, WLR_PRIVATE.gamma_control_manager_v1_destroy);
	wl_list_remove(&scene->WLR_PRIVATE.gamma_control_manager_v1_destroy.link);
	wl_list_remove(&scene->WLR_PRIVATE.gamma_control_manager_v1_set_gamma.link);
	scene->gamma_control_manager_v1 = NULL;
}

static void handle_gamma_control_set_gamma(struct wl_listener *listener, void *data) {
	struct wlr_scene *scene = wl_container_of(
		listener, scene, WLR_PRIVATE.gamma_control_manager_v1_set_gamma);
	const struct wlr_gamma_control_manager_v1_set_gamma_event *event = data;
	struct wlr_scene_output *so = wlr_scene_get_scene_output(scene, event->output);
	if (!so) {
		return;
	}
	so->WLR_PRIVATE.gamma_lut_changed = true;
	so->WLR_PRIVATE.gamma_lut = event->control;
	wlr_damage_ring_add_whole(&so->damage_ring);
	wlr_output_schedule_frame(so->output);
}

void wlr_scene_set_gamma_control_manager_v1(struct wlr_scene *scene,
		struct wlr_gamma_control_manager_v1 *gamma_control) {
	assert(!scene->gamma_control_manager_v1);
	if (gamma_control->fallback_gamma_size == 0) {
		gamma_control->fallback_gamma_size = 1024;
	}
	scene->gamma_control_manager_v1 = gamma_control;
	scene->WLR_PRIVATE.gamma_control_manager_v1_destroy.notify = handle_gamma_control_destroy;
	wl_signal_add(&gamma_control->events.destroy,
		&scene->WLR_PRIVATE.gamma_control_manager_v1_destroy);
	scene->WLR_PRIVATE.gamma_control_manager_v1_set_gamma.notify = handle_gamma_control_set_gamma;
	wl_signal_add(&gamma_control->events.set_gamma,
		&scene->WLR_PRIVATE.gamma_control_manager_v1_set_gamma);
}

static void handle_color_manager_destroy(struct wl_listener *listener, void *data) {
	struct wlr_scene *scene = wl_container_of(
		listener, scene, WLR_PRIVATE.color_manager_v1_destroy);
	wl_list_remove(&scene->WLR_PRIVATE.color_manager_v1_destroy.link);
	scene->color_manager_v1 = NULL;
}

void wlr_scene_set_color_manager_v1(struct wlr_scene *scene,
		struct wlr_color_manager_v1 *manager) {
	assert(!scene->color_manager_v1);
	scene->color_manager_v1 = manager;
	scene->WLR_PRIVATE.color_manager_v1_destroy.notify = handle_color_manager_destroy;
	wl_signal_add(&manager->events.destroy,
		&scene->WLR_PRIVATE.color_manager_v1_destroy);
}

struct wlr_scene_tree *wlr_scene_tree_create(struct wlr_scene_tree *parent) {
	assert(parent);
	struct wlr_scene_tree *tree = calloc(1, sizeof(*tree));
	if (!tree) {
		return NULL;
	}
	scene_node_init(&tree->node, WLR_SCENE_NODE_TREE, parent);
	wl_list_init(&tree->children);
	return tree;
}

struct wlr_scene_buffer *wlr_scene_buffer_from_node(struct wlr_scene_node *node) {
	assert(node->type == WLR_SCENE_NODE_BUFFER);
	return wl_container_of(node, (struct wlr_scene_buffer *)NULL, node);
}

struct wlr_scene_tree *wlr_scene_tree_from_node(struct wlr_scene_node *node) {
	assert(node->type == WLR_SCENE_NODE_TREE);
	return wl_container_of(node, (struct wlr_scene_tree *)NULL, node);
}

struct wlr_scene_rect *wlr_scene_rect_from_node(struct wlr_scene_node *node) {
	assert(node->type == WLR_SCENE_NODE_RECT);
	return wl_container_of(node, (struct wlr_scene_rect *)NULL, node);
}

struct wlr_scene_rect *wlr_scene_rect_create(struct wlr_scene_tree *parent,
		int width, int height, const float color[static 4]) {
	assert(parent);
	struct wlr_scene_rect *rect = calloc(1, sizeof(*rect));
	if (!rect) {
		return NULL;
	}
	scene_node_init(&rect->node, WLR_SCENE_NODE_RECT, parent);
	rect->width = width;
	rect->height = height;
	memcpy(rect->color, color, sizeof(rect->color));
	rect->accepts_input = true;
	rect->corners = corner_radii_none();
	rect->clipped_region = clipped_region_get_default();
	scene_node_update(&rect->node, NULL);
	return rect;
}

void wlr_scene_rect_set_size(struct wlr_scene_rect *rect, int width, int height) {
	if (rect->width == width && rect->height == height) {
		return;
	}
	struct wlr_scene *scene = scene_node_get_root(&rect->node);
	pixman_region32_t old_damage;
	pixman_region32_init(&old_damage);
	int lx = 0, ly = 0;
	if (scene && wlr_scene_node_coords(&rect->node, &lx, &ly) &&
			rect->width > 0 && rect->height > 0) {
		pixman_region32_union_rect(&old_damage, &old_damage, lx, ly,
			rect->width, rect->height);
	}

	rect->width = width;
	rect->height = height;

	if (scene && !pixman_region32_empty(&old_damage)) {
		scene_damage_outputs(scene, &old_damage);
	}
	pixman_region32_fini(&old_damage);
	scene_node_update(&rect->node, NULL);
}

void wlr_scene_rect_set_color(struct wlr_scene_rect *rect, const float color[static 4]) {
	if (memcmp(rect->color, color, sizeof(rect->color)) == 0) {
		return;
	}
	memcpy(rect->color, color, sizeof(rect->color));
	scene_node_update(&rect->node, NULL);
}

void wlr_scene_rect_set_corner_radius(struct wlr_scene_rect *rect, int corner_radius) {
	wlr_scene_rect_set_corner_radii(rect, corner_radii_all(corner_radius));
}

void wlr_scene_rect_set_corner_radii(struct wlr_scene_rect *rect,
		struct fx_corner_radii corners) {
	if (fx_corner_radii_eq(rect->corners, corners)) {
		return;
	}
	rect->corners = corners;
	scene_node_update(&rect->node, NULL);
}

void wlr_scene_rect_set_clipped_region(struct wlr_scene_rect *rect,
		struct clipped_region clipped_region) {
	if (memcmp(&rect->clipped_region.area, &clipped_region.area, sizeof(struct wlr_box)) == 0 &&
			fx_corner_radii_eq(rect->clipped_region.corners, clipped_region.corners)) {
		return;
	}
	rect->clipped_region = clipped_region;
	scene_node_update(&rect->node, NULL);
}

struct wlr_scene_buffer *wlr_scene_buffer_create(struct wlr_scene_tree *parent,
		struct wlr_buffer *buffer) {
	assert(parent);
	struct wlr_scene_buffer *scene_buffer = calloc(1, sizeof(*scene_buffer));
	if (!scene_buffer) {
		return NULL;
	}
	scene_node_init(&scene_buffer->node, WLR_SCENE_NODE_BUFFER, parent);
	wl_signal_init(&scene_buffer->events.outputs_update);
	wl_signal_init(&scene_buffer->events.output_enter);
	wl_signal_init(&scene_buffer->events.output_leave);
	wl_signal_init(&scene_buffer->events.output_sample);
	wl_signal_init(&scene_buffer->events.frame_done);
	pixman_region32_init(&scene_buffer->opaque_region);
	wl_list_init(&scene_buffer->WLR_PRIVATE.buffer_release.link);
	wl_list_init(&scene_buffer->WLR_PRIVATE.renderer_destroy.link);
	scene_buffer->opacity = 1.0f;
	scene_buffer->corners = corner_radii_none();

	wlr_scene_buffer_set_buffer(scene_buffer, buffer);
	return scene_buffer;
}

void wlr_scene_buffer_set_buffer(struct wlr_scene_buffer *scene_buffer,
		struct wlr_buffer *buffer) {
	wlr_scene_buffer_set_buffer_with_options(scene_buffer, buffer, NULL);
}

void wlr_scene_buffer_set_buffer_with_damage(struct wlr_scene_buffer *scene_buffer,
		struct wlr_buffer *buffer, const pixman_region32_t *region) {
	struct wlr_scene_buffer_set_buffer_options options = {
		.damage = region,
	};
	wlr_scene_buffer_set_buffer_with_options(scene_buffer, buffer, &options);
}

void wlr_scene_buffer_set_buffer_with_options(struct wlr_scene_buffer *scene_buffer,
		struct wlr_buffer *buffer, const struct wlr_scene_buffer_set_buffer_options *options) {
	const pixman_region32_t *damage = options ? options->damage : NULL;

	if (scene_buffer->WLR_PRIVATE.wait_timeline) {
		wlr_drm_syncobj_timeline_unref(scene_buffer->WLR_PRIVATE.wait_timeline);
		scene_buffer->WLR_PRIVATE.wait_timeline = NULL;
		scene_buffer->WLR_PRIVATE.wait_point = 0;
	}
	if (options && options->wait_timeline) {
		scene_buffer->WLR_PRIVATE.wait_timeline =
			wlr_drm_syncobj_timeline_ref(options->wait_timeline);
		scene_buffer->WLR_PRIVATE.wait_point = options->wait_point;
	}

	if (scene_buffer->buffer == buffer && buffer != NULL) {
		if (damage && scene_buffer->WLR_PRIVATE.texture) {
			if (!wlr_texture_update_from_buffer(scene_buffer->WLR_PRIVATE.texture,
					buffer, damage)) {
				scene_buffer_destroy_texture(scene_buffer);
			}
		} else if (!damage) {
			scene_buffer_destroy_texture(scene_buffer);
		}
		scene_node_update(&scene_buffer->node, damage);
		return;
	}

	// Capture old bounds if shrinking or removing buffer
	int old_w = 0, old_h = 0;
	scene_buffer_get_dest_size(scene_buffer, &old_w, &old_h);
	int lx = 0, ly = 0;
	struct wlr_scene *scene = scene_node_get_root(&scene_buffer->node);
	bool was_visible = scene && wlr_scene_node_coords(&scene_buffer->node, &lx, &ly);

	if (buffer && scene_buffer->WLR_PRIVATE.texture && damage) {
		if (!wlr_texture_update_from_buffer(scene_buffer->WLR_PRIVATE.texture,
				buffer, damage)) {
			scene_buffer_destroy_texture(scene_buffer);
		}
	} else {
		scene_buffer_destroy_texture(scene_buffer);
	}

	scene_buffer_clear_buffer(scene_buffer);

	if (buffer) {
		scene_buffer->buffer = wlr_buffer_lock(buffer);
		scene_buffer->WLR_PRIVATE.buffer_width = buffer->width;
		scene_buffer->WLR_PRIVATE.buffer_height = buffer->height;

		struct wlr_single_pixel_buffer_v1 *spb =
			wlr_single_pixel_buffer_v1_try_from_buffer(buffer);
		if (spb) {
			scene_buffer->WLR_PRIVATE.is_single_pixel_buffer = true;
			scene_buffer->WLR_PRIVATE.single_pixel_buffer_color[0] = spb->r;
			scene_buffer->WLR_PRIVATE.single_pixel_buffer_color[1] = spb->g;
			scene_buffer->WLR_PRIVATE.single_pixel_buffer_color[2] = spb->b;
			scene_buffer->WLR_PRIVATE.single_pixel_buffer_color[3] = spb->a;
			scene_buffer->WLR_PRIVATE.buffer_is_opaque = (spb->a == UINT32_MAX);
		} else {
			scene_buffer->WLR_PRIVATE.buffer_is_opaque = wlr_buffer_is_opaque(buffer);
		}
	}

	int new_w = 0, new_h = 0;
	scene_buffer_get_dest_size(scene_buffer, &new_w, &new_h);
	if (was_visible && (old_w != new_w || old_h != new_h)) {
		pixman_region32_t old_reg;
		pixman_region32_init_rect(&old_reg, lx, ly, old_w, old_h);
		scene_damage_outputs(scene, &old_reg);
		pixman_region32_fini(&old_reg);
		damage = NULL;
	}

	if (damage && scene_buffer->dst_width == 0 && scene_buffer->dst_height == 0 &&
			wlr_fbox_empty(&scene_buffer->src_box) &&
			scene_buffer->transform == WL_OUTPUT_TRANSFORM_NORMAL) {
		scene_node_update(&scene_buffer->node, damage);
	} else {
		scene_node_update(&scene_buffer->node, NULL);
	}
}

void wlr_scene_buffer_set_opaque_region(struct wlr_scene_buffer *scene_buffer,
		const pixman_region32_t *region) {
	if (region) {
		pixman_region32_copy(&scene_buffer->opaque_region, region);
	} else {
		pixman_region32_clear(&scene_buffer->opaque_region);
	}
}

void wlr_scene_buffer_set_source_box(struct wlr_scene_buffer *scene_buffer,
		const struct wlr_fbox *box) {
	struct wlr_fbox new_box = box ? *box : (struct wlr_fbox){0};
	if (memcmp(&scene_buffer->src_box, &new_box, sizeof(new_box)) == 0) {
		return;
	}
	scene_buffer->src_box = new_box;
	scene_node_update(&scene_buffer->node, NULL);
}

void wlr_scene_buffer_set_dest_size(struct wlr_scene_buffer *scene_buffer,
		int width, int height) {
	if (scene_buffer->dst_width == width && scene_buffer->dst_height == height) {
		return;
	}
	struct wlr_scene *scene = scene_node_get_root(&scene_buffer->node);
	int old_w = 0, old_h = 0;
	scene_buffer_get_dest_size(scene_buffer, &old_w, &old_h);
	int lx = 0, ly = 0;
	if (scene && wlr_scene_node_coords(&scene_buffer->node, &lx, &ly) &&
			old_w > 0 && old_h > 0) {
		pixman_region32_t reg;
		pixman_region32_init_rect(&reg, lx, ly, old_w, old_h);
		scene_damage_outputs(scene, &reg);
		pixman_region32_fini(&reg);
	}

	scene_buffer->dst_width = width;
	scene_buffer->dst_height = height;
	scene_node_update(&scene_buffer->node, NULL);
}

void wlr_scene_buffer_set_transform(struct wlr_scene_buffer *scene_buffer,
		enum wl_output_transform transform) {
	if (scene_buffer->transform == transform) {
		return;
	}
	scene_buffer->transform = transform;
	scene_node_update(&scene_buffer->node, NULL);
}

void wlr_scene_buffer_set_opacity(struct wlr_scene_buffer *scene_buffer,
		float opacity) {
	if (scene_buffer->opacity == opacity) {
		return;
	}
	scene_buffer->opacity = opacity;
	scene_node_update(&scene_buffer->node, NULL);
}

void wlr_scene_buffer_set_filter_mode(struct wlr_scene_buffer *scene_buffer,
		enum wlr_scale_filter_mode filter_mode) {
	if (scene_buffer->filter_mode == filter_mode) {
		return;
	}
	scene_buffer->filter_mode = filter_mode;
	scene_node_update(&scene_buffer->node, NULL);
}

void wlr_scene_buffer_set_corner_radius(struct wlr_scene_buffer *scene_buffer, int radii) {
	wlr_scene_buffer_set_corner_radii(scene_buffer, corner_radii_all(radii));
}

void wlr_scene_buffer_set_corner_radii(struct wlr_scene_buffer *scene_buffer,
		struct fx_corner_radii corner_radii) {
	if (fx_corner_radii_eq(scene_buffer->corners, corner_radii)) {
		return;
	}
	scene_buffer->corners = corner_radii;
	scene_node_update(&scene_buffer->node, NULL);
}

void wlr_scene_buffer_set_transfer_function(struct wlr_scene_buffer *scene_buffer,
		enum wlr_color_transfer_function transfer_function) {
	if (scene_buffer->transfer_function == transfer_function) {
		return;
	}
	scene_buffer->transfer_function = transfer_function;
	scene_node_update(&scene_buffer->node, NULL);
}

void wlr_scene_buffer_set_primaries(struct wlr_scene_buffer *scene_buffer,
		enum wlr_color_named_primaries primaries) {
	if (scene_buffer->primaries == primaries) {
		return;
	}
	scene_buffer->primaries = primaries;
	scene_node_update(&scene_buffer->node, NULL);
}

void wlr_scene_buffer_set_color_encoding(struct wlr_scene_buffer *scene_buffer,
		enum wlr_color_encoding encoding) {
	if (scene_buffer->color_encoding == encoding) {
		return;
	}
	scene_buffer->color_encoding = encoding;
	scene_node_update(&scene_buffer->node, NULL);
}

void wlr_scene_buffer_set_color_range(struct wlr_scene_buffer *scene_buffer,
		enum wlr_color_range range) {
	if (scene_buffer->color_range == range) {
		return;
	}
	scene_buffer->color_range = range;
	scene_node_update(&scene_buffer->node, NULL);
}

void wlr_scene_buffer_send_frame_done(struct wlr_scene_buffer *scene_buffer,
		struct wlr_scene_frame_done_event *event) {
	wl_signal_emit_mutable(&scene_buffer->events.frame_done, event);
}

void scene_buffer_send_output_enter(struct wlr_scene_buffer *buffer,
		struct wlr_scene_output *output) {
	wl_signal_emit_mutable(&buffer->events.output_enter, output);
}

void scene_buffer_send_output_leave(struct wlr_scene_buffer *buffer,
		struct wlr_scene_output *output) {
	wl_signal_emit_mutable(&buffer->events.output_leave, output);
}
