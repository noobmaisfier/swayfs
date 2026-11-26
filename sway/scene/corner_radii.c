#include "sway/scene/corner_radii.h"

struct fx_corner_radii fx_corner_radii_extend(struct fx_corner_radii corners, int extend) {
	return corner_radii_new(
		corners.top_left ? corners.top_left + extend : 0,
		corners.top_right ? corners.top_right + extend : 0,
		corners.bottom_right ? corners.bottom_right + extend : 0,
		corners.bottom_left ? corners.bottom_left + extend : 0
	);
}

void fx_corner_radii_transform(enum wl_output_transform transform,
		struct fx_corner_radii *corners) {
	struct fx_corner_radii c = *corners;
	switch (transform) {
	case WL_OUTPUT_TRANSFORM_NORMAL:
		break;
	case WL_OUTPUT_TRANSFORM_90:
		*corners = (struct fx_corner_radii){ c.bottom_left, c.top_left, c.top_right, c.bottom_right };
		break;
	case WL_OUTPUT_TRANSFORM_180:
		*corners = (struct fx_corner_radii){ c.bottom_right, c.bottom_left, c.top_left, c.top_right };
		break;
	case WL_OUTPUT_TRANSFORM_270:
		*corners = (struct fx_corner_radii){ c.top_right, c.bottom_right, c.bottom_left, c.top_left };
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		*corners = (struct fx_corner_radii){ c.top_right, c.top_left, c.bottom_left, c.bottom_right };
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		*corners = (struct fx_corner_radii){ c.bottom_right, c.top_right, c.top_left, c.bottom_left };
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		*corners = (struct fx_corner_radii){ c.bottom_left, c.bottom_right, c.top_right, c.top_left };
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		*corners = (struct fx_corner_radii){ c.top_left, c.bottom_left, c.bottom_right, c.top_right };
		break;
	}
}

bool fx_corner_radii_eq(struct fx_corner_radii lhs, struct fx_corner_radii rhs) {
	return lhs.top_left == rhs.top_left &&
		lhs.top_right == rhs.top_right &&
		lhs.bottom_right == rhs.bottom_right &&
		lhs.bottom_left == rhs.bottom_left;
}

bool fx_corner_radii_is_empty(const struct fx_corner_radii *corners) {
	return corners->top_left == 0 &&
		corners->top_right == 0 &&
		corners->bottom_right == 0 &&
		corners->bottom_left == 0;
}

struct clipped_region clipped_region_get_default(void) {
	return (struct clipped_region){
		.area = {0},
		.corners = corner_radii_none(),
	};
}
