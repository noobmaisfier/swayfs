#include <cairo.h>
#include <fcft/fcft.h>
#include <pixman.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "log.h"
#include "stringop.h"
#include "sway/font.h"

static bool fcft_ready = false;

static void ensure_fcft_init(void) {
	if (!fcft_ready) {
		fcft_init(FCFT_LOG_COLORIZE_NEVER, false, FCFT_LOG_CLASS_ERROR);
		fcft_ready = true;
	}
}

size_t escape_markup_text(const char *src, char *dest) {
	size_t length = 0;
	if (dest) {
		dest[0] = '\0';
	}
	while (src[0]) {
		switch (src[0]) {
		case '&':
			length += 5;
			lenient_strcat(dest, "&amp;");
			break;
		case '<':
			length += 4;
			lenient_strcat(dest, "&lt;");
			break;
		case '>':
			length += 4;
			lenient_strcat(dest, "&gt;");
			break;
		case '\'':
			length += 6;
			lenient_strcat(dest, "&apos;");
			break;
		case '"':
			length += 6;
			lenient_strcat(dest, "&quot;");
			break;
		default:
			if (dest) {
				dest[length] = *src;
				dest[length + 1] = '\0';
			}
			length += 1;
		}
		src++;
	}
	return length;
}

// Strip <tags> and resolve common entities for markup=true.
// fcft has no markup engine; swaybar markup becomes plain text.
static char *strip_markup(const char *text) {
	size_t len = strlen(text);
	char *out = malloc(len + 1);
	if (!out) {
		return NULL;
	}
	size_t j = 0;
	for (size_t i = 0; i < len;) {
		if (text[i] == '<') {
			size_t k = i + 1;
			while (k < len && text[k] != '>') {
				k++;
			}
			i = k < len ? k + 1 : len;
		} else if (text[i] == '&') {
			if (!strncmp(&text[i], "&amp;", 5)) {
				out[j++] = '&'; i += 5;
			} else if (!strncmp(&text[i], "&lt;", 4)) {
				out[j++] = '<'; i += 4;
			} else if (!strncmp(&text[i], "&gt;", 4)) {
				out[j++] = '>'; i += 4;
			} else if (!strncmp(&text[i], "&apos;", 6)) {
				out[j++] = '\''; i += 6;
			} else if (!strncmp(&text[i], "&quot;", 6)) {
				out[j++] = '"'; i += 6;
			} else {
				out[j++] = text[i++];
			}
		} else {
			out[j++] = text[i++];
		}
	}
	out[j] = '\0';
	return out;
}

// Decode one UTF-8 sequence. Returns bytes consumed, -1 on error.
static int utf8_decode_one(const char *s, size_t left, uint32_t *cp) {
	unsigned char c = (unsigned char)s[0];
	if (c < 0x80) {
		*cp = c;
		return 1;
	} else if ((c & 0xE0) == 0xC0) {
		if (left < 2) {
			return -1;
		}
		*cp = ((uint32_t)(c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
		return 2;
	} else if ((c & 0xF0) == 0xE0) {
		if (left < 3) {
			return -1;
		}
		*cp = ((uint32_t)(c & 0x0F) << 12) |
			((uint32_t)((unsigned char)s[1] & 0x3F) << 6) |
			((unsigned char)s[2] & 0x3F);
		return 3;
	} else if ((c & 0xF8) == 0xF0) {
		if (left < 4) {
			return -1;
		}
		*cp = ((uint32_t)(c & 0x07) << 18) |
			((uint32_t)((unsigned char)s[1] & 0x3F) << 12) |
			((uint32_t)((unsigned char)s[2] & 0x3F) << 6) |
			((unsigned char)s[3] & 0x3F);
		return 4;
	}
	return -1;
}

static uint32_t *utf8_to_utf32(const char *s, size_t *out_len) {
	size_t slen = strlen(s);
	uint32_t *out = malloc(sizeof(uint32_t) * (slen + 1));
	if (!out) {
		return NULL;
	}
	size_t n = 0;
	for (size_t i = 0; i < slen;) {
		uint32_t cp;
		int bytes = utf8_decode_one(&s[i], slen - i, &cp);
		if (bytes < 0) {
			cp = 0xFFFD;
			bytes = 1;
		}
		out[n++] = cp;
		i += bytes;
	}
	*out_len = n;
	return out;
}

static bool str_contains_ci(const char *haystack, const char *needle) {
	size_t nlen = strlen(needle);
	for (const char *p = haystack; *p; p++) {
		if (!strncasecmp(p, needle, nlen)) {
			return true;
		}
	}
	return false;
}

struct sway_font *sway_font_from_string(const char *str) {
	ensure_fcft_init();
	if (!str || !str[0]) {
		str = "monospace 10";
	}
	// Strip optional "pango:" prefix for compat.
	if (!strncmp(str, "pango:", 6)) {
		str += 6;
	}
	struct sway_font *font = calloc(1, sizeof(*font));
	if (!font) {
		return NULL;
	}
	font->pattern = strdup(str);
	font->size = 10;
	font->family = strdup("monospace");
	if (!font->pattern || !font->family) {
		sway_font_free(font);
		return NULL;
	}

	// Parse trailing size number, detect Bold/Italic.
	char *tmp = strdup(str);
	if (tmp) {
		// Detect style before splitting.
		if (str_contains_ci(tmp, "bold")) {
			font->bold = true;
		}
		if (str_contains_ci(tmp, "italic") || str_contains_ci(tmp, "oblique")) {
			font->italic = true;
		}
		// Trailing integer = size.
		size_t tlen = strlen(tmp);
		while (tlen > 0 && tmp[tlen - 1] == ' ') {
			tmp[--tlen] = '\0';
		}
		char *last_space = strrchr(tmp, ' ');
		if (last_space) {
			char *end = NULL;
			long sz = strtol(last_space + 1, &end, 10);
			if (end && *end == '\0' && sz > 0 && sz < 256) {
				font->size = (int)sz;
				*last_space = '\0';
			}
		}
		// Remaining (minus style words) is family.
		// Remove Bold/Italic/Regular words for family name.
		char fam[256] = {0};
		size_t fo = 0;
		char *save = NULL;
		for (char *tok = strtok_r(tmp, " ", &save); tok;
				tok = strtok_r(NULL, " ", &save)) {
			if (!strcasecmp(tok, "bold") || !strcasecmp(tok, "italic") ||
					!strcasecmp(tok, "oblique") || !strcasecmp(tok, "regular") ||
					!strcasecmp(tok, "book")) {
				continue;
			}
			if (fo > 0 && fo < sizeof(fam) - 1) {
				fam[fo++] = ' ';
			}
			size_t tl = strlen(tok);
			if (fo + tl >= sizeof(fam) - 1) {
				break;
			}
			memcpy(&fam[fo], tok, tl);
			fo += tl;
		}
		if (fo > 0) {
			free(font->family);
			font->family = strdup(fam);
		}
		free(tmp);
	}

	// Load base fcft font at scale=1. px approx pt*4/3 to match pango 96dpi.
	int px = (font->size * 4 + 2) / 3;
	if (px < 6) {
		px = 6;
	}
	char attrs[128];
	snprintf(attrs, sizeof(attrs), "size=%d%s%s", px,
		font->bold ? ":weight=bold" : "",
		font->italic ? ":slant=italic" : "");
	const char *names[1] = { font->family };
	font->fcft = fcft_from_name(1, names, attrs);
	if (!font->fcft) {
		// Fallback to monospace.
		const char *fallback[1] = { "monospace" };
		font->fcft = fcft_from_name(1, fallback, attrs);
	}
	if (!font->fcft) {
		sway_log(SWAY_ERROR, "fcft_from_name failed for '%s'", str);
		sway_font_free(font);
		return NULL;
	}
	font->fcft_px = px;
	font->fcft_scale = 1.0;
	return font;
}

static void sway_font_build_attrs(const struct sway_font *font, int px,
		char *attrs, size_t len) {
	snprintf(attrs, len, "size=%d%s%s", px,
		font->bold ? ":weight=bold" : "",
		font->italic ? ":slant=italic" : "");
}

// Forward: width cache invalidation on free.
static void width_cache_invalidate_font(const struct sway_font *font);

static bool sway_font_ensure_scale(struct sway_font *font, double scale) {
	if (!font || !font->fcft) {
		return false;
	}
	if (scale <= 0) {
		scale = 1.0;
	}
	if (font->fcft_scale == scale) {
		return true;
	}
	int px = (int)((font->size * 4 + 2) / 3 * scale + 0.5);
	if (px < 6) {
		px = 6;
	}
	if (px == font->fcft_px) {
		font->fcft_scale = scale;
		return true;
	}
	// 1-slot LRU: swapping back to the previous scale is free.
	if (font->prev_fcft && font->prev_px == px) {
		struct fcft_font *tmp = font->fcft;
		font->fcft = font->prev_fcft;
		font->prev_fcft = tmp;
		int tpx = font->fcft_px;
		font->fcft_px = font->prev_px;
		font->prev_px = tpx;
		double ts = font->fcft_scale;
		font->fcft_scale = scale;
		font->prev_scale = ts;
		return true;
	}
	char attrs[128];
	sway_font_build_attrs(font, px, attrs, sizeof(attrs));
	const char *names[1] = { font->family };
	struct fcft_font *nf = fcft_from_name(1, names, attrs);
	if (!nf) {
		return false;
	}
	// Keep the old font as LRU instead of destroying it.
	if (font->prev_fcft) {
		fcft_destroy(font->prev_fcft);
	}
	font->prev_fcft = font->fcft;
	font->prev_px = font->fcft_px;
	font->prev_scale = font->fcft_scale;
	font->fcft = nf;
	font->fcft_px = px;
	font->fcft_scale = scale;
	return true;
}

// Purge fcft's internal per-font glyph cache (no clear API in 3.x) by
// reloading the same px size. Called when enough text runs have gone by
// that CJK/emoji-heavy sessions could have pinned several Mo.
static void sway_font_maybe_purge(struct sway_font *font) {
	if (++font->runs_since_purge < 8000) {
		return;
	}
	font->runs_since_purge = 0;
	char attrs[128];
	sway_font_build_attrs(font, font->fcft_px, attrs, sizeof(attrs));
	const char *names[1] = { font->family };
	struct fcft_font *nf = fcft_from_name(1, names, attrs);
	if (!nf) {
		return;
	}
	fcft_destroy(font->fcft);
	font->fcft = nf;
	// LRU entry belongs to another px or is now stale; drop it so a swap
	// cannot resurrect the purged (fat) cache.
	if (font->prev_fcft) {
		fcft_destroy(font->prev_fcft);
		font->prev_fcft = NULL;
		font->prev_px = 0;
	}
	sway_log(SWAY_DEBUG, "purged fcft glyph cache for '%s'", font->pattern);
}

void sway_font_free(struct sway_font *font) {
	if (!font) {
		return;
	}
	width_cache_invalidate_font(font);
	if (font->fcft) {
		fcft_destroy(font->fcft);
	}
	if (font->prev_fcft) {
		fcft_destroy(font->prev_fcft);
	}
	free(font->pattern);
	free(font->family);
	free(font);
}

struct sway_font *sway_font_copy(const struct sway_font *src) {
	if (!src) {
		return NULL;
	}
	return sway_font_from_string(src->pattern);
}

bool sway_font_equal(const struct sway_font *a, const struct sway_font *b) {
	if (a == b) {
		return true;
	}
	if (!a || !b) {
		return false;
	}
	return strcmp(a->pattern, b->pattern) == 0;
}

char *sway_font_to_string(const struct sway_font *font) {
	if (!font) {
		return NULL;
	}
	return strdup(font->pattern);
}

void get_text_metrics(const struct sway_font *font, int *height, int *baseline) {
	if (height) {
		*height = 0;
	}
	if (baseline) {
		*baseline = 0;
	}
	if (!font || !font->fcft) {
		return;
	}
	if (height) {
		*height = font->fcft->height;
	}
	if (baseline) {
		*baseline = font->fcft->ascent;
	}
}

// Tiny width cache: titlebars measure then render the same string, so the
// second rasterize is pure waste. 64 entries, ~2K worst case, linear scan.
#define WIDTH_CACHE_N 64
static struct {
	const struct sway_font *font;
	char *pattern; // font pattern copy (ptr reuse after free is unsafe)
	char *text;
	int px;
	int width, height, baseline;
	unsigned long gen;
} width_cache[WIDTH_CACHE_N];
static unsigned long width_cache_gen;

static bool width_cache_lookup(const struct sway_font *font, int px,
		const char *text, int *w, int *h, int *b) {
	for (int i = 0; i < WIDTH_CACHE_N; i++) {
		if (!width_cache[i].text || width_cache[i].font != font ||
				width_cache[i].px != px) {
			continue;
		}
		if (strcmp(width_cache[i].text, text) != 0) {
			continue;
		}
		if (width_cache[i].pattern &&
				strcmp(width_cache[i].pattern, font->pattern) != 0) {
			continue;
		}
		if (w) {
			*w = width_cache[i].width;
		}
		if (h) {
			*h = width_cache[i].height;
		}
		if (b) {
			*b = width_cache[i].baseline;
		}
		width_cache[i].gen = ++width_cache_gen;
		return true;
	}
	return false;
}

static void width_cache_store(const struct sway_font *font, int px,
		const char *text, int w, int h, int b) {
	int slot = 0;
	unsigned long oldest = width_cache[0].gen;
	for (int i = 1; i < WIDTH_CACHE_N; i++) {
		if (!width_cache[i].text) {
			slot = i;
			break;
		}
		if (width_cache[i].gen < oldest) {
			oldest = width_cache[i].gen;
			slot = i;
		}
	}
	free(width_cache[slot].text);
	free(width_cache[slot].pattern);
	width_cache[slot].text = strdup(text);
	width_cache[slot].pattern = font->pattern ? strdup(font->pattern) : NULL;
	if (!width_cache[slot].text) {
		return;
	}
	width_cache[slot].font = font;
	width_cache[slot].px = px;
	width_cache[slot].width = w;
	width_cache[slot].height = h;
	width_cache[slot].baseline = b;
	width_cache[slot].gen = ++width_cache_gen;
}

static void width_cache_invalidate_font(const struct sway_font *font) {
	for (int i = 0; i < WIDTH_CACHE_N; i++) {
		if (width_cache[i].font == font) {
			free(width_cache[i].text);
			free(width_cache[i].pattern);
			width_cache[i].text = NULL;
			width_cache[i].pattern = NULL;
			width_cache[i].font = NULL;
		}
	}
}

void get_text_size(cairo_t *cairo, const struct sway_font *font,
		int *width, int *height, int *baseline,
		double scale, bool markup, const char *fmt, ...) {
	(void)cairo;
	if (width) {
		*width = 0;
	}
	if (height) {
		*height = 0;
	}
	if (baseline) {
		*baseline = 0;
	}
	if (!font) {
		return;
	}
	va_list args;
	va_start(args, fmt);
	char *buf = vformat_str(fmt, args);
	va_end(args);
	if (!buf) {
		return;
	}
	char *plain = buf;
	char *stripped = NULL;
	if (markup && (strchr(buf, '<') || strchr(buf, '&'))) {
		stripped = strip_markup(buf);
		if (stripped) {
			plain = stripped;
		}
	}
	if (plain[0] == '\0') {
		free(stripped);
		free(buf);
		return;
	}
	// Ensure scaled font for crisp HiDPI + correct extents.
	sway_font_ensure_scale((struct sway_font *)font, scale);
	struct fcft_font *fc = font->fcft;
	if (!fc) {
		free(stripped);
		free(buf);
		return;
	}
	// Measure+render fusion via cache: the render pass right after a
	// measure hits here and skips its own rasterize.
	int cw = 0, ch = 0, cb = 0;
	if (width_cache_lookup(font, font->fcft_px, plain, &cw, &ch, &cb)) {
		if (width) {
			*width = cw;
		}
		if (height) {
			*height = ch;
		}
		if (baseline) {
			*baseline = cb;
		}
		free(stripped);
		free(buf);
		return;
	}
	size_t ulen = 0;
	uint32_t *u32 = utf8_to_utf32(plain, &ulen);
	if (!u32 || ulen == 0) {
		free(u32);
		free(stripped);
		free(buf);
		return;
	}
	struct fcft_text_run *run = fcft_rasterize_text_run_utf32(
		fc, ulen, u32, FCFT_SUBPIXEL_NONE);
	long adv = 0;
	if (run) {
		for (size_t i = 0; i < run->count; i++) {
			adv += run->glyphs[i]->advance.x;
		}
		fcft_text_run_destroy(run);
	} else {
		// Fallback per-char advance.
		for (size_t i = 0; i < ulen; i++) {
			const struct fcft_glyph *g = fcft_rasterize_char_utf32(
				fc, u32[i], FCFT_SUBPIXEL_NONE);
			if (g) {
				adv += g->advance.x;
			}
		}
	}
	free(u32);
	// fcft advances are in unscaled px; we loaded scaled font, so direct.
	if (width) {
		*width = (int)adv;
	}
	if (height) {
		*height = fc->height;
	}
	if (baseline) {
		*baseline = fc->ascent;
	}
	width_cache_store(font, font->fcft_px, plain,
		(int)adv, fc->height, fc->ascent);
	sway_font_maybe_purge((struct sway_font *)font);
	free(stripped);
	free(buf);
}

void render_text(cairo_t *cairo, const struct sway_font *font,
		double scale, bool markup, const char *fmt, ...) {
	if (!cairo || !font) {
		return;
	}
	va_list args;
	va_start(args, fmt);
	char *buf = vformat_str(fmt, args);
	va_end(args);
	if (!buf) {
		return;
	}
	char *plain = buf;
	char *stripped = NULL;
	if (markup && (strchr(buf, '<') || strchr(buf, '&'))) {
		stripped = strip_markup(buf);
		if (stripped) {
			plain = stripped;
		}
	}
	if (plain[0] == '\0') {
		free(stripped);
		free(buf);
		return;
	}
	sway_font_ensure_scale((struct sway_font *)font, scale);
	struct fcft_font *fc = font->fcft;
	if (!fc) {
		free(stripped);
		free(buf);
		return;
	}
	size_t ulen = 0;
	uint32_t *u32 = utf8_to_utf32(plain, &ulen);
	if (!u32 || ulen == 0) {
		free(u32);
		free(stripped);
		free(buf);
		return;
	}
	struct fcft_text_run *run = fcft_rasterize_text_run_utf32(
		fc, ulen, u32, FCFT_SUBPIXEL_NONE);
	double px, py;
	cairo_get_current_point(cairo, &px, &py);
	// Baseline in cairo user space: current point is top-left in sway callers
	// (they move_to(x, y) then render). pango drew from top; emulate by
	// offsetting baseline by ascent.
	double base_y = py + fc->ascent;
	double pen_x = px;
	cairo_save(cairo);
	if (run) {
		for (size_t i = 0; i < run->count; i++) {
			const struct fcft_glyph *g = run->glyphs[i];
			if (!g || !g->pix || g->width <= 0 || g->height <= 0) {
				pen_x += g ? g->advance.x : 0;
				continue;
			}
			int gw = pixman_image_get_width(g->pix);
			int gh = pixman_image_get_height(g->pix);
			int stride = pixman_image_get_stride(g->pix);
			void *data = pixman_image_get_data(g->pix);
			if (!data || gw <= 0 || gh <= 0) {
				pen_x += g->advance.x;
				continue;
			}
			double dst_x = pen_x + g->x;
			double dst_y = base_y + g->y - gh;
			// glyph->y is bearing from baseline? fcft y is top offset
			// relative to baseline origin; empirical: dst = baseline + y - h.
			// Clamp obviously broken values from missing glyphs.
			if (g->is_color_glyph) {
				cairo_surface_t *surf = cairo_image_surface_create_for_data(
					data, CAIRO_FORMAT_ARGB32, gw, gh, stride);
				if (cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
					cairo_set_source_surface(cairo, surf, dst_x, dst_y);
					cairo_paint(cairo);
				}
				cairo_surface_destroy(surf);
			} else {
				cairo_surface_t *mask = cairo_image_surface_create_for_data(
					data, CAIRO_FORMAT_A8, gw, gh, stride);
				if (cairo_surface_status(mask) == CAIRO_STATUS_SUCCESS) {
					cairo_mask_surface(cairo, mask, dst_x, dst_y);
				}
				cairo_surface_destroy(mask);
			}
			pen_x += g->advance.x;
		}
		fcft_text_run_destroy(run);
	}
	cairo_restore(cairo);
	// Advance cairo point like pango_show_layout does (for callers chaining).
	cairo_move_to(cairo, pen_x, py);
	// Feed the width cache so a following measure of the same string is free,
	// and count the run toward the glyph-cache purge bound.
	width_cache_store(font, font->fcft_px, plain,
		(int)(pen_x - px), fc->height, fc->ascent);
	sway_font_maybe_purge((struct sway_font *)font);
	free(u32);
	free(stripped);
	free(buf);
}

struct sway_text_lines *sway_text_wrap(const struct sway_font *font,
		const char *text, int max_width) {
	struct sway_text_lines *res = calloc(1, sizeof(*res));
	if (!res) {
		return NULL;
	}
	if (!text || !text[0] || max_width <= 0) {
		return res;
	}
	// Split on '\n' first, then word-wrap each paragraph on spaces.
	// Measure with fcft advances at scale=1 (callers scale cairo themselves).
	char *copy = strdup(text);
	if (!copy) {
		return res;
	}
	size_t cap = 16;
	res->lines = malloc(sizeof(char *) * cap);
	if (!res->lines) {
		free(copy);
		free(res);
		return NULL;
	}
	char *save_para = NULL;
	for (char *para = strtok_r(copy, "\n", &save_para); para;
			para = strtok_r(NULL, "\n", &save_para)) {
		// Wrap paragraph.
		char *words = strdup(para);
		if (!words) {
			continue;
		}
		char *line_buf = malloc(strlen(para) + 1);
		if (!line_buf) {
			free(words);
			continue;
		}
		line_buf[0] = '\0';
		size_t line_len = 0;
		int line_w = 0;
		char *save_w = NULL;
		// Preserve empty paragraphs as empty line.
		if (para[0] == '\0') {
			if (res->count >= cap) {
				cap *= 2;
				char **nl = realloc(res->lines, sizeof(char *) * cap);
				if (!nl) {
					break;
				}
				res->lines = nl;
			}
			res->lines[res->count++] = strdup("");
			free(words);
			free(line_buf);
			continue;
		}
		for (char *w = strtok_r(words, " ", &save_w); w;
				w = strtok_r(NULL, " ", &save_w)) {
			// Width of "w" plus leading space if needed.
			int ww = 0, hh = 0;
			// Use internal measurement without cairo (scale=1).
			// Inline simple measure to avoid cairo NULL issues.
			size_t ulen = 0;
			uint32_t *u32 = utf8_to_utf32(w, &ulen);
			long adv = 0;
			if (u32 && font && font->fcft) {
				struct fcft_text_run *run = fcft_rasterize_text_run_utf32(
					font->fcft, ulen, u32, FCFT_SUBPIXEL_NONE);
				if (run) {
					for (size_t i = 0; i < run->count; i++) {
						adv += run->glyphs[i]->advance.x;
					}
					fcft_text_run_destroy(run);
				}
			}
			free(u32);
			ww = (int)adv;
			int space_w = line_len == 0 ? 0 : 4; // approx space
			if (line_len > 0 && line_w + space_w + ww > max_width) {
				// Emit line.
				if (res->count >= cap) {
					cap *= 2;
					char **nl = realloc(res->lines, sizeof(char *) * cap);
					if (!nl) {
						break;
					}
					res->lines = nl;
				}
				res->lines[res->count++] = strdup(line_buf);
				int lh = font && font->fcft ? font->fcft->height : 12;
				res->height += lh;
				if (line_w > res->width) {
					res->width = line_w;
				}
				line_buf[0] = '\0';
				line_len = 0;
				line_w = 0;
				space_w = 0;
			}
			if (line_len > 0) {
				strcat(line_buf, " ");
				line_len++;
				line_w += space_w;
			}
			strcat(line_buf, w);
			line_len += strlen(w);
			line_w += ww;
			(void)hh;
		}
		if (line_len > 0 || true) {
			if (res->count >= cap) {
				cap *= 2;
				char **nl = realloc(res->lines, sizeof(char *) * cap);
				if (!nl) {
					free(words);
					free(line_buf);
					break;
				}
				res->lines = nl;
			}
			res->lines[res->count++] = strdup(line_buf);
			int lh = font && font->fcft ? font->fcft->height : 12;
			res->height += lh;
			if (line_w > res->width) {
				res->width = line_w;
			}
		}
		free(words);
		free(line_buf);
	}
	free(copy);
	return res;
}

void sway_text_lines_free(struct sway_text_lines *lines) {
	if (!lines) {
		return;
	}
	for (size_t i = 0; i < lines->count; i++) {
		free(lines->lines[i]);
	}
	free(lines->lines);
	free(lines);
}
