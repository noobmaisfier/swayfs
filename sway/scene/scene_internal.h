#ifndef SWAY_SCENE_INTERNAL_H
#define SWAY_SCENE_INTERNAL_H

#include "sway/scene/wlr_scene.h"

struct wlr_scene *scene_node_get_root(struct wlr_scene_node *node);
void scene_node_get_size(struct wlr_scene_node *node, int *width, int *height);
void scene_node_update(struct wlr_scene_node *node, const pixman_region32_t *damage);
void scene_damage_outputs(struct wlr_scene *scene, const pixman_region32_t *damage);
void scene_buffer_get_dest_size(struct wlr_scene_buffer *buffer, int *width, int *height);
void scene_buffer_send_output_enter(struct wlr_scene_buffer *buffer,
		struct wlr_scene_output *output);
void scene_buffer_send_output_leave(struct wlr_scene_buffer *buffer,
		struct wlr_scene_output *output);

#endif
