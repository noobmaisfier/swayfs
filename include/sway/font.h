#ifndef _SWAY_FONT_H
#define _SWAY_FONT_H
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <cairo.h>
#include "stringop.h"

struct sway_font {
	char *pattern; // original string, e.g. "monospace 10"
	char *family;
	int size; // pt, default 10
	bool bold;
	bool italic;
	struct fcft_font *fcft;
	int fcft_px; // cached px size of loaded fcft font
	double fcft_scale; // scale it was loaded for
	// 1-slot LRU: previous scale's font, avoids destroy/reload thrash when
	// a window moves between outputs of different scales.
	struct fcft_font *prev_fcft;
	int prev_px;
	double prev_scale;
	// Glyph-cache bound: fcft 3.x has no cache-clear API; the per-font
	// glyph cache grows with distinct glyphs seen (CJK/emoji = Mo).
	// Count text runs and reload the font to purge past the threshold.
	unsigned long runs_since_purge;
};

struct sway_font *sway_font_from_string(const char *str);
void sway_font_free(struct sway_font *font);
struct sway_font *sway_font_copy(const struct sway_font *src);
bool sway_font_equal(const struct sway_font *a, const struct sway_font *b);
char *sway_font_to_string(const struct sway_font *font);

size_t escape_markup_text(const char *src, char *dest);

void get_text_size(cairo_t *cairo, const struct sway_font *font,
	int *width, int *height, int *baseline,
	double scale, bool markup, const char *fmt, ...) _SWAY_ATTRIB_PRINTF(8, 9);
void get_text_metrics(const struct sway_font *font, int *height, int *baseline);
void render_text(cairo_t *cairo, const struct sway_font *font,
	double scale, bool markup, const char *fmt, ...) _SWAY_ATTRIB_PRINTF(5, 6);

// Multi-line helpers replacing PangoLayout wrapping (swaynag details).
// Wraps plain text to max_width px (scale=1 coordinates), returns lines.
struct sway_text_lines {
	char **lines;
	size_t count;
	int width;
	int height; // total
};
struct sway_text_lines *sway_text_wrap(const struct sway_font *font,
	const char *text, int max_width);
void sway_text_lines_free(struct sway_text_lines *lines);

#endif
