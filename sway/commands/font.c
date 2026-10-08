#include <string.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/font.h"
#include "log.h"
#include "stringop.h"

struct cmd_results *cmd_font(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "font", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	char *font = join_args(argv, argc);
	free(config->font);

	if (has_prefix(font, "pango:")) {
		config->pango_markup = true;
		config->font = strdup(font + strlen("pango:"));
		free(font);
	} else {
		config->pango_markup = false;
		config->font = font;
	}

	// Parse the font early so we can reject it if invalid.
	// Also avoids re-parsing each time we render text.
	struct sway_font *font_description = sway_font_from_string(config->font);

	if (!font_description || !font_description->family) {
		sway_font_free(font_description);
		return cmd_results_new(CMD_FAILURE, "Invalid font family.");
	}

	if (font_description->size == 0) {
		sway_font_free(font_description);
		return cmd_results_new(CMD_FAILURE, "Invalid font size.");
	}

	if (config->font_description != NULL) {
		sway_font_free(config->font_description);
	}

	config->font_description = font_description;
	config_update_font_height();

	return cmd_results_new(CMD_SUCCESS, NULL);
}
