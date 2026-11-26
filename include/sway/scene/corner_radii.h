#ifndef SWAY_SCENE_CORNER_RADII_H
#define SWAY_SCENE_CORNER_RADII_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-protocol.h>
#include <wlr/util/box.h>

// Clockwise corner radii in logical pixels, matching scenefx layout.
struct fx_corner_radii {
	uint16_t top_left;
	uint16_t top_right;
	uint16_t bottom_right;
	uint16_t bottom_left;
};

#define CORNER_RADIUS_MAX (UINT16_MAX)

static inline uint16_t corner_radius_clamp(int radius) {
	if (radius <= 0) {
		return 0;
	}
	if (radius > CORNER_RADIUS_MAX) {
		return CORNER_RADIUS_MAX;
	}
	return (uint16_t)radius;
}

static inline struct fx_corner_radii corner_radii_new(int top_left, int top_right,
		int bottom_right, int bottom_left) {
	return (struct fx_corner_radii){
		corner_radius_clamp(top_left),
		corner_radius_clamp(top_right),
		corner_radius_clamp(bottom_right),
		corner_radius_clamp(bottom_left),
	};
}

static inline struct fx_corner_radii corner_radii_all(int radius) {
	return corner_radii_new(radius, radius, radius, radius);
}

static inline struct fx_corner_radii corner_radii_none(void) {
	return corner_radii_all(0);
}

static inline struct fx_corner_radii corner_radii_top(int radius) {
	return corner_radii_new(radius, radius, 0, 0);
}

static inline struct fx_corner_radii corner_radii_bottom(int radius) {
	return corner_radii_new(0, 0, radius, radius);
}

static inline struct fx_corner_radii corner_radii_left(int radius) {
	return corner_radii_new(radius, 0, 0, radius);
}

static inline struct fx_corner_radii corner_radii_right(int radius) {
	return corner_radii_new(0, radius, radius, 0);
}

static inline struct fx_corner_radii fx_corner_radii_filter(
		struct fx_corner_radii input, struct fx_corner_radii filter) {
	return corner_radii_new(
		input.top_left && filter.top_left ? input.top_left : 0,
		input.top_right && filter.top_right ? input.top_right : 0,
		input.bottom_right && filter.bottom_right ? input.bottom_right : 0,
		input.bottom_left && filter.bottom_left ? input.bottom_left : 0
	);
}

struct fx_corner_radii fx_corner_radii_extend(struct fx_corner_radii corners, int extend);
void fx_corner_radii_transform(enum wl_output_transform transform,
		struct fx_corner_radii *corners);
bool fx_corner_radii_eq(struct fx_corner_radii lhs, struct fx_corner_radii rhs);
bool fx_corner_radii_is_empty(const struct fx_corner_radii *corners);

struct clipped_region {
	struct wlr_box area;
	struct fx_corner_radii corners;
};

struct clipped_region clipped_region_get_default(void);

#endif
