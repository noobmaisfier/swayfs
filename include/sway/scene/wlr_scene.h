#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE
#endif

#ifndef WLR_TYPES_WLR_SCENE_H
#define WLR_TYPES_WLR_SCENE_H

#include <pixman.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/util/addon.h>
#include <wlr/util/box.h>

#include "sway/scene/corner_radii.h"

struct wlr_output;
struct wlr_output_layout;
struct wlr_output_layout_output;
struct wlr_xdg_surface;
struct wlr_layer_surface_v1;
struct wlr_drag_icon;
struct wlr_surface;

struct wlr_scene_node;
struct wlr_scene_buffer;
struct wlr_scene_output_layout;

struct wlr_presentation;
struct wlr_linux_dmabuf_v1;
struct wlr_gamma_control_manager_v1;
struct wlr_color_manager_v1;
struct wlr_output_state;

typedef bool (*wlr_scene_buffer_point_accepts_input_func_t)(
	struct wlr_scene_buffer *buffer, double *sx, double *sy);

typedef void (*wlr_scene_buffer_iterator_func_t)(
	struct wlr_scene_buffer *buffer, int sx, int sy, void *user_data);

enum wlr_scene_node_type {
	WLR_SCENE_NODE_TREE,
	WLR_SCENE_NODE_RECT,
	WLR_SCENE_NODE_BUFFER,
};

struct wlr_scene_node {
	enum wlr_scene_node_type type;
	struct wlr_scene_tree *parent;

	struct wl_list link; // wlr_scene_tree.children

	bool enabled;
	int x, y; // relative to parent

	struct {
		struct wl_signal destroy;
	} events;

	void *data;

	struct wlr_addon_set addons;

	struct {
		pixman_region32_t visible;
	} WLR_PRIVATE;
};

enum wlr_scene_debug_damage_option {
	WLR_SCENE_DEBUG_DAMAGE_NONE,
	WLR_SCENE_DEBUG_DAMAGE_RERENDER,
	WLR_SCENE_DEBUG_DAMAGE_HIGHLIGHT
};

struct wlr_scene_tree {
	struct wlr_scene_node node;

	struct wl_list children; // wlr_scene_node.link
};

struct wlr_scene {
	struct wlr_scene_tree tree;

	struct wl_list outputs; // wlr_scene_output.link

	struct wlr_linux_dmabuf_v1 *linux_dmabuf_v1;
	struct wlr_gamma_control_manager_v1 *gamma_control_manager_v1;
	struct wlr_color_manager_v1 *color_manager_v1;

	bool restack_xwayland_surfaces;

	struct {
		struct wl_listener linux_dmabuf_v1_destroy;
		struct wl_listener gamma_control_manager_v1_destroy;
		struct wl_listener gamma_control_manager_v1_set_gamma;
		struct wl_listener color_manager_v1_destroy;

		enum wlr_scene_debug_damage_option debug_damage_option;
		bool direct_scanout;
		bool calculate_visibility;
		bool highlight_transparent_region;
	} WLR_PRIVATE;
};

struct wlr_scene_surface {
	struct wlr_scene_buffer *buffer;
	struct wlr_surface *surface;

	struct {
		struct wlr_box clip;

		struct wlr_addon addon;

		struct wl_listener outputs_update;
		struct wl_listener output_sample;
		struct wl_listener frame_done;
		struct wl_listener surface_destroy;
		struct wl_listener surface_commit;
	} WLR_PRIVATE;
};

struct wlr_scene_rect {
	struct wlr_scene_node node;
	int width, height;
	float color[4];

	struct fx_corner_radii corners;
	bool accepts_input;
	struct clipped_region clipped_region;
};

struct wlr_scene_outputs_update_event {
	struct wlr_scene_output **active;
	size_t size;
};

struct wlr_scene_output_sample_event {
	struct wlr_scene_output *output;
	bool direct_scanout;
	struct wlr_drm_syncobj_timeline *release_timeline;
	uint64_t release_point;
};

struct wlr_scene_frame_done_event {
	struct wlr_scene_output *output;
	struct timespec when;
};

struct wlr_scene_buffer {
	struct wlr_scene_node node;

	struct wlr_buffer *buffer;

	struct {
		struct wl_signal outputs_update;
		struct wl_signal output_enter;
		struct wl_signal output_leave;
		struct wl_signal output_sample;
		struct wl_signal frame_done;
	} events;

	wlr_scene_buffer_point_accepts_input_func_t point_accepts_input;

	struct wlr_scene_output *primary_output;

	float opacity;
	enum wlr_scale_filter_mode filter_mode;
	struct wlr_fbox src_box;
	int dst_width, dst_height;
	enum wl_output_transform transform;
	pixman_region32_t opaque_region;
	enum wlr_color_transfer_function transfer_function;
	enum wlr_color_named_primaries primaries;
	enum wlr_color_encoding color_encoding;
	enum wlr_color_range color_range;

	struct {
		uint64_t active_outputs;
		struct wlr_texture *texture;
		struct wlr_linux_dmabuf_feedback_v1_init_options prev_feedback_options;

		bool own_buffer;
		int buffer_width, buffer_height;
		bool buffer_is_opaque;

		struct wlr_drm_syncobj_timeline *wait_timeline;
		uint64_t wait_point;

		struct wl_listener buffer_release;
		struct wl_listener renderer_destroy;

		bool is_single_pixel_buffer;
		uint32_t single_pixel_buffer_color[4];
	} WLR_PRIVATE;

	struct fx_corner_radii corners;
};

struct wlr_scene_output {
	struct wlr_output *output;
	struct wl_list link; // wlr_scene.outputs
	struct wlr_scene *scene;
	struct wlr_addon addon;

	struct wlr_damage_ring damage_ring;

	int x, y;

	struct {
		struct wl_signal destroy;
	} events;

	struct {
		pixman_region32_t pending_commit_damage;

		uint8_t index;

		uint8_t dmabuf_feedback_debounce;
		bool prev_scanout;

		bool gamma_lut_changed;
		struct wlr_gamma_control_v1 *gamma_lut;
		struct wlr_color_transform *gamma_lut_color_transform;

		struct wlr_color_transform *prev_gamma_lut_color_transform;
		struct wlr_color_transform *prev_supplied_color_transform;
		struct wlr_color_transform *combined_color_transform;

		struct wl_listener output_commit;
		struct wl_listener output_damage;
		struct wl_listener output_needs_frame;

		struct wl_list damage_highlight_regions;

		struct wl_array render_list;

		struct wlr_drm_syncobj_timeline *in_timeline;
		uint64_t in_point;
		struct wlr_drm_syncobj_timeline *out_timeline;
		uint64_t out_point;
	} WLR_PRIVATE;

	// Intermediate swapchain when color_transform is active with rounded corners
	struct wlr_swapchain *ct_swapchain;
};

struct wlr_scene_timer {
	int64_t pre_render_duration;
	struct wlr_render_timer *render_timer;
};

struct wlr_scene_layer_surface_v1 {
	struct wlr_scene_tree *tree;
	struct wlr_layer_surface_v1 *layer_surface;

	struct {
		struct wl_listener tree_destroy;
		struct wl_listener layer_surface_destroy;
		struct wl_listener layer_surface_map;
		struct wl_listener layer_surface_unmap;
	} WLR_PRIVATE;
};

void wlr_scene_node_destroy(struct wlr_scene_node *node);
void wlr_scene_node_set_enabled(struct wlr_scene_node *node, bool enabled);
void wlr_scene_node_set_position(struct wlr_scene_node *node, int x, int y);
void wlr_scene_node_place_above(struct wlr_scene_node *node,
	struct wlr_scene_node *sibling);
void wlr_scene_node_place_below(struct wlr_scene_node *node,
	struct wlr_scene_node *sibling);
void wlr_scene_node_raise_to_top(struct wlr_scene_node *node);
void wlr_scene_node_lower_to_bottom(struct wlr_scene_node *node);
void wlr_scene_node_reparent(struct wlr_scene_node *node,
	struct wlr_scene_tree *new_parent);
bool wlr_scene_node_coords(struct wlr_scene_node *node, int *lx, int *ly);
void wlr_scene_node_for_each_buffer(struct wlr_scene_node *node,
	wlr_scene_buffer_iterator_func_t iterator, void *user_data);
struct wlr_scene_node *wlr_scene_node_at(struct wlr_scene_node *node,
	double lx, double ly, double *nx, double *ny);

struct wlr_scene *wlr_scene_create(void);
void wlr_scene_set_linux_dmabuf_v1(struct wlr_scene *scene,
	struct wlr_linux_dmabuf_v1 *linux_dmabuf_v1);
void wlr_scene_set_gamma_control_manager_v1(struct wlr_scene *scene,
	struct wlr_gamma_control_manager_v1 *gamma_control);
void wlr_scene_set_color_manager_v1(struct wlr_scene *scene,
	struct wlr_color_manager_v1 *manager);

struct wlr_scene_tree *wlr_scene_tree_create(struct wlr_scene_tree *parent);
struct wlr_scene_surface *wlr_scene_surface_create(struct wlr_scene_tree *parent,
	struct wlr_surface *surface);

struct wlr_scene_buffer *wlr_scene_buffer_from_node(struct wlr_scene_node *node);
struct wlr_scene_tree *wlr_scene_tree_from_node(struct wlr_scene_node *node);
struct wlr_scene_rect *wlr_scene_rect_from_node(struct wlr_scene_node *node);
struct wlr_scene_surface *wlr_scene_surface_try_from_buffer(
	struct wlr_scene_buffer *scene_buffer);
void wlr_scene_surface_send_frame_done(struct wlr_scene_surface *scene_surface,
	const struct timespec *when);

struct wlr_scene_rect *wlr_scene_rect_create(struct wlr_scene_tree *parent,
	int width, int height, const float color[static 4]);
void wlr_scene_rect_set_size(struct wlr_scene_rect *rect, int width, int height);
void wlr_scene_rect_set_color(struct wlr_scene_rect *rect, const float color[static 4]);
void wlr_scene_rect_set_corner_radius(struct wlr_scene_rect *rect, int corner_radius);
void wlr_scene_rect_set_corner_radii(struct wlr_scene_rect *rect,
	struct fx_corner_radii corners);
void wlr_scene_rect_set_clipped_region(struct wlr_scene_rect *rect,
	struct clipped_region clipped_region);

struct wlr_scene_buffer *wlr_scene_buffer_create(struct wlr_scene_tree *parent,
	struct wlr_buffer *buffer);
void wlr_scene_buffer_set_buffer(struct wlr_scene_buffer *scene_buffer,
	struct wlr_buffer *buffer);
void wlr_scene_buffer_set_buffer_with_damage(struct wlr_scene_buffer *scene_buffer,
	struct wlr_buffer *buffer, const pixman_region32_t *region);

struct wlr_scene_buffer_set_buffer_options {
	const pixman_region32_t *damage;
	struct wlr_drm_syncobj_timeline *wait_timeline;
	uint64_t wait_point;
};

void wlr_scene_buffer_set_buffer_with_options(struct wlr_scene_buffer *scene_buffer,
	struct wlr_buffer *buffer, const struct wlr_scene_buffer_set_buffer_options *options);
void wlr_scene_buffer_set_opaque_region(struct wlr_scene_buffer *scene_buffer,
	const pixman_region32_t *region);
void wlr_scene_buffer_set_source_box(struct wlr_scene_buffer *scene_buffer,
	const struct wlr_fbox *box);
void wlr_scene_buffer_set_dest_size(struct wlr_scene_buffer *scene_buffer,
	int width, int height);
void wlr_scene_buffer_set_transform(struct wlr_scene_buffer *scene_buffer,
	enum wl_output_transform transform);
void wlr_scene_buffer_set_opacity(struct wlr_scene_buffer *scene_buffer,
	float opacity);
void wlr_scene_buffer_set_filter_mode(struct wlr_scene_buffer *scene_buffer,
	enum wlr_scale_filter_mode filter_mode);
void wlr_scene_buffer_set_corner_radius(struct wlr_scene_buffer *scene_buffer,
	int radii);
void wlr_scene_buffer_set_corner_radii(struct wlr_scene_buffer *scene_buffer,
	struct fx_corner_radii corner_radii);
void wlr_scene_buffer_set_transfer_function(struct wlr_scene_buffer *scene_buffer,
	enum wlr_color_transfer_function transfer_function);
void wlr_scene_buffer_set_primaries(struct wlr_scene_buffer *scene_buffer,
	enum wlr_color_named_primaries primaries);
void wlr_scene_buffer_set_color_encoding(struct wlr_scene_buffer *scene_buffer,
	enum wlr_color_encoding encoding);
void wlr_scene_buffer_set_color_range(struct wlr_scene_buffer *scene_buffer,
	enum wlr_color_range range);
void wlr_scene_buffer_send_frame_done(struct wlr_scene_buffer *scene_buffer,
	struct wlr_scene_frame_done_event *event);

struct wlr_scene_output *wlr_scene_output_create(struct wlr_scene *scene,
	struct wlr_output *output);
void wlr_scene_output_destroy(struct wlr_scene_output *scene_output);
void wlr_scene_output_set_position(struct wlr_scene_output *scene_output,
	int lx, int ly);

struct wlr_scene_output_state_options {
	struct wlr_scene_timer *timer;
	struct wlr_color_transform *color_transform;
	struct wlr_swapchain *swapchain;
};

bool wlr_scene_output_needs_frame(struct wlr_scene_output *scene_output);
bool wlr_scene_output_commit(struct wlr_scene_output *scene_output,
	const struct wlr_scene_output_state_options *options);
bool wlr_scene_output_build_state(struct wlr_scene_output *scene_output,
	struct wlr_output_state *state, const struct wlr_scene_output_state_options *options);

int64_t wlr_scene_timer_get_duration_ns(struct wlr_scene_timer *timer);
void wlr_scene_timer_finish(struct wlr_scene_timer *timer);

void wlr_scene_output_send_frame_done(struct wlr_scene_output *scene_output,
	struct timespec *now);
void wlr_scene_output_for_each_buffer(struct wlr_scene_output *scene_output,
	wlr_scene_buffer_iterator_func_t iterator, void *user_data);
struct wlr_scene_output *wlr_scene_get_scene_output(struct wlr_scene *scene,
	struct wlr_output *output);

struct wlr_scene_output_layout *wlr_scene_attach_output_layout(struct wlr_scene *scene,
	struct wlr_output_layout *output_layout);
void wlr_scene_output_layout_add_output(struct wlr_scene_output_layout *sol,
	struct wlr_output_layout_output *lo, struct wlr_scene_output *so);

struct wlr_scene_tree *wlr_scene_subsurface_tree_create(
	struct wlr_scene_tree *parent, struct wlr_surface *surface);
void wlr_scene_subsurface_tree_set_clip(struct wlr_scene_node *node,
	const struct wlr_box *clip);

struct wlr_scene_tree *wlr_scene_xdg_surface_create(
	struct wlr_scene_tree *parent, struct wlr_xdg_surface *xdg_surface);

struct wlr_scene_layer_surface_v1 *wlr_scene_layer_surface_v1_create(
	struct wlr_scene_tree *parent, struct wlr_layer_surface_v1 *layer_surface);
void wlr_scene_layer_surface_v1_configure(
	struct wlr_scene_layer_surface_v1 *scene_layer_surface,
	const struct wlr_box *full_area, struct wlr_box *usable_area);

struct wlr_scene_tree *wlr_scene_drag_icon_create(
	struct wlr_scene_tree *parent, struct wlr_drag_icon *drag_icon);

#endif
